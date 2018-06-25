// jemalloc C++ threaded test
// Author: Rustam Abdullaev
// Public Domain

#include <atomic>
#include <functional>
#include <future>
#include <random>
#include <thread>
#include <vector>
#include <stdio.h>
#include <jemalloc/jemalloc.h>

using std::vector;
using std::thread;
using std::uniform_int_distribution;
using std::minstd_rand;

int test_threads()
{
  je_malloc_conf = "narenas:3";
  int narenas = 0;
  size_t sz = sizeof(narenas);
  je_mallctl("opt.narenas", (void *)&narenas, &sz, NULL, 0);
  if (narenas != 3) {
    printf("Error: unexpected number of arenas: %d\n", narenas);
    return 1;
  }
  static const int sizes[] = { 7, 16, 32, 60, 91, 100, 120, 144, 169, 199, 255, 400, 670, 900, 917, 1025, 3333, 5190, 13131, 49192, 99999, 123123, 255265, 2333111 };
  static const int numSizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
  vector<thread> workers;
  static const int numThreads = narenas + 1, numAllocsMax = 25, numIter1 = 50, numIter2 = 50;
  je_malloc_stats_print(NULL, NULL, NULL);
  size_t allocated1;
  size_t sz1 = sizeof(allocated1);
  je_mallctl("stats.active", (void *)&allocated1, &sz1, NULL, 0);
  printf("\nPress Enter to start threads...\n");
  getchar();
  printf("Starting %d threads x %d x %d iterations...\n", numThreads, numIter1, numIter2);
  for (int i = 0; i < numThreads; i++) {
    workers.emplace_back([tid=i]() {
      uniform_int_distribution<int> sizeDist(0, numSizes - 1);
      minstd_rand rnd(tid * 17);
      uint8_t* ptrs[numAllocsMax];
      int ptrsz[numAllocsMax];
      for (int i = 0; i < numIter1; ++i) {
        thread t([&]() {
          for (int i = 0; i < numIter2; ++i) {
            const int numAllocs = numAllocsMax - sizeDist(rnd);
            for (int j = 0; j < numAllocs; j += 64) {
              const int x = sizeDist(rnd);
              const int sz = sizes[x];
              ptrsz[j] = sz;
              ptrs[j] = (uint8_t*)je_malloc(sz);
              if (!ptrs[j]) {
                printf("Unable to allocate %d bytes in thread %d, iter %d, alloc %d. %d\n", sz, tid, i, j, x);
                exit(1);
              }
              for (int k = 0; k < sz; k++)
                ptrs[j][k] = tid + k;
            }
            for (int j = 0; j < numAllocs; j += 64) {
              for (int k = 0, sz = ptrsz[j]; k < sz; k++)
                if (ptrs[j][k] != (uint8_t)(tid + k)) {
                  printf("Memory error in thread %d, iter %d, alloc %d @ %d : %02X!=%02X\n", tid, i, j, k, ptrs[j][k], (uint8_t)(tid + k));
                  exit(1);
                }
              je_free(ptrs[j]);
            }
          }
        });
        t.join();
      }
    });
  }
  for (thread& t : workers) {
    t.join();
  }
  je_malloc_stats_print(NULL, NULL, NULL);
  size_t allocated2;
  je_mallctl("stats.active", (void *)&allocated2, &sz1, NULL, 0);
  size_t leaked = allocated2 - allocated1;
  printf("\nDone. Leaked: %zd bytes\n", leaked);
  bool failed = leaked > 65536; // in case C++ runtime allocated something (e.g. iostream locale or facet)
  printf("\nTest %s!\n", (failed ? "FAILED" : "successful"));
  printf("\nPress Enter to continue...\n");
  getchar();
  return failed ? 1 : 0;
}
