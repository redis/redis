/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "liburing.h"
#include "helpers.h"
#include "../src/syscall.h"

/*
 * This syzbot test is known broken on some archs, just allow the ones that
 * are regularly tested.
 */
#if defined(__i386__) || defined(__x86_64__) || defined(__arm__) || \
    defined(__aarch64__)

static uint64_t current_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        exit(1);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#define SIZEOF_IO_URING_SQE 64
#define SIZEOF_IO_URING_CQE 16
#define SQ_TAIL_OFFSET 64
#define SQ_RING_MASK_OFFSET 256
#define SQ_RING_ENTRIES_OFFSET 264
#define CQ_RING_ENTRIES_OFFSET 268
#define CQ_CQES_OFFSET 320

#define IORING_OFF_SQES 0x10000000ULL

static void kill_and_wait(int pid, int* status)
{
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(-1, status, __WALL) != pid) {
    }
}

#define WAIT_FLAGS __WALL

static uint64_t r[3] = {0xffffffffffffffff, 0x0, 0x0};

static long syz_io_uring_setup(volatile long a0, volatile long a1,
volatile long a2, volatile long a3, volatile long a4, volatile long
a5)
{
    uint32_t entries = (uint32_t)a0;
    struct io_uring_params* setup_params = (struct io_uring_params*)a1;
    void* vma1 = (void*)a2;
    void* vma2 = (void*)a3;
    void** ring_ptr_out = (void**)a4;
    void** sqes_ptr_out = (void**)a5;
    uint32_t fd_io_uring = __sys_io_uring_setup(entries, setup_params);
    uint32_t sq_ring_sz = setup_params->sq_off.array +
setup_params->sq_entries * sizeof(uint32_t);
    uint32_t cq_ring_sz = setup_params->cq_off.cqes +
setup_params->cq_entries * SIZEOF_IO_URING_CQE;
    uint32_t ring_sz = sq_ring_sz > cq_ring_sz ? sq_ring_sz : cq_ring_sz;
    *ring_ptr_out = mmap(vma1, ring_sz, PROT_READ | PROT_WRITE,
MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd_io_uring,
IORING_OFF_SQ_RING);
    uint32_t sqes_sz = setup_params->sq_entries * SIZEOF_IO_URING_SQE;
    *sqes_ptr_out = mmap(vma2, sqes_sz, PROT_READ | PROT_WRITE,
MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd_io_uring, IORING_OFF_SQES);
    return fd_io_uring;
}

static long syz_io_uring_submit(volatile long a0, volatile long a1,
volatile long a2, volatile long a3)
{
    char* ring_ptr = (char*)a0;
    char* sqes_ptr = (char*)a1;
    char* sqe = (char*)a2;
    uint32_t sqes_index = (uint32_t)a3;
    uint32_t sq_ring_entries = *(uint32_t*)(ring_ptr + SQ_RING_ENTRIES_OFFSET);
    uint32_t cq_ring_entries = *(uint32_t*)(ring_ptr + CQ_RING_ENTRIES_OFFSET);
    uint32_t sq_array_off = (CQ_CQES_OFFSET + cq_ring_entries *
SIZEOF_IO_URING_CQE + 63) & ~63;
    if (sq_ring_entries)
        sqes_index %= sq_ring_entries;
    char* sqe_dest = sqes_ptr + sqes_index * SIZEOF_IO_URING_SQE;
    memcpy(sqe_dest, sqe, SIZEOF_IO_URING_SQE);
    uint32_t sq_ring_mask = *(uint32_t*)(ring_ptr + SQ_RING_MASK_OFFSET);
    uint32_t* sq_tail_ptr = (uint32_t*)(ring_ptr + SQ_TAIL_OFFSET);
    uint32_t sq_tail = *sq_tail_ptr & sq_ring_mask;
    uint32_t sq_tail_next = *sq_tail_ptr + 1;
    uint32_t* sq_array = (uint32_t*)(ring_ptr + sq_array_off);
    *(sq_array + sq_tail) = sqes_index;
    __atomic_store_n(sq_tail_ptr, sq_tail_next, __ATOMIC_RELEASE);
    return 0;
}


static void trigger_bug(void)
{
    intptr_t res = 0;
    *(uint32_t*)0x20000204 = 0;
    *(uint32_t*)0x20000208 = 2;
    *(uint32_t*)0x2000020c = 0;
    *(uint32_t*)0x20000210 = 0;
    *(uint32_t*)0x20000218 = -1;
    memset((void*)0x2000021c, 0, 12);
    res = -1;
    res = syz_io_uring_setup(0x7987, 0x20000200, 0x20400000, 0x20ffd000, 0x200000c0, 0x200001c0);
    if (res != -1) {
        r[0] = res;
        r[1] = *(uint64_t*)0x200000c0;
        r[2] = *(uint64_t*)0x200001c0;
    }
    *(uint8_t*)0x20000180 = 0xb;
    *(uint8_t*)0x20000181 = 1;
    *(uint16_t*)0x20000182 = 0;
    *(uint32_t*)0x20000184 = 0;
    *(uint64_t*)0x20000188 = 4;
    *(uint64_t*)0x20000190 = 0x20000140;
    *(uint64_t*)0x20000140 = 0x77359400;
    *(uint64_t*)0x20000148 = 0;
    *(uint32_t*)0x20000198 = 1;
    *(uint32_t*)0x2000019c = 0;
    *(uint64_t*)0x200001a0 = 0;
    *(uint16_t*)0x200001a8 = 0;
    *(uint16_t*)0x200001aa = 0;
    memset((void*)0x200001ac, 0, 20);
    syz_io_uring_submit(r[1], r[2], 0x20000180, 1);
    *(uint32_t*)0x20000544 = 0;
    *(uint32_t*)0x20000548 = 0x36;
    *(uint32_t*)0x2000054c = 0;
    *(uint32_t*)0x20000550 = 0;
    *(uint32_t*)0x20000558 = r[0];
    memset((void*)0x2000055c, 0, 12);

}
int main(void)
{
    mmap((void *)0x20000000ul, 0x1000000ul, 7ul, MAP_ANON|MAP_PRIVATE, -1, 0ul);
    int pid = fork();
    if (pid < 0)
        exit(1);
    if (pid == 0) {
        trigger_bug();
        exit(0);
    }
    int status = 0;
    uint64_t start = current_time_ms();
    for (;;) {
        if (current_time_ms() - start < 1000) {
            continue;
        }
        kill_and_wait(pid, &status);
        break;
    }
    return 0;
}
#else
int main(void)
{
	return T_EXIT_SKIP;
}
#endif
