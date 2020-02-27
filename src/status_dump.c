// Create a new thread to dump status periodically.

#include "server.h"

#include <sys/param.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "status_dump.h"

static const char* filename = "redis.running.status";
static const char* temp_filename = "redis.running.status.temp";

// Dump current status to file.
static void dump_current_status() {
  struct rusage self_ru;
  char cwd[MAXPATHLEN];
  getrusage(RUSAGE_SELF, &self_ru);
  float cpu_time = (float)self_ru.ru_stime.tv_sec +
                   ((float)self_ru.ru_stime.tv_usec / USEC_PER_SEC) +
                   (float)self_ru.ru_utime.tv_sec +
                   ((float)self_ru.ru_utime.tv_usec / USEC_PER_SEC);
  long long report_mstime = mstime();
  const char* role = server.masterhost == NULL ? "master" : "slave";
  long long current_command_start_mstime =
      __atomic_load_n(&server.current_command_start_mstime, __ATOMIC_SEQ_CST);
  long long total_commands_processed =
      __atomic_load_n(&server.stat_numcommands, __ATOMIC_SEQ_CST);
  const char* is_busy = current_command_start_mstime != 0 ? "true" : "false";

  FILE* fp = fopen(temp_filename, "wt+");
  if (!fp) {
    char* cwdp = getcwd(cwd, MAXPATHLEN);
    serverLog(LL_WARNING,
              "Dumping running status: failed to open file %s"
              "(in server root dir %s) for dumping. errno: %s",
              temp_filename, cwdp ? cwdp : "unknown", strerror(errno));
    return;
  }
  if (fprintf(fp,
              "run_id:%s\r\n"
              "report_mstime:%lld\r\n"
              "cpu_time:%f\r\n"
              "role:%s\r\n"
              "is_busy:%s\r\n"
              "current_command_start_mstime:%lld\r\n"
              "total_commands_processed:%lld\r\n",
              server.runid, report_mstime, cpu_time, role, is_busy,
              current_command_start_mstime, total_commands_processed) < 0) {
    char* cwdp = getcwd(cwd, MAXPATHLEN);
    serverLog(LL_WARNING,
              "Dumping running status: failed to write file %s"
              "(in server root dir %s) for dumping. errno: %s",
              temp_filename, cwdp ? cwdp : "unknown", strerror(errno));
    goto werr;
  }
  if (fclose(fp) == EOF) {
    char* cwdp = getcwd(cwd, MAXPATHLEN);
    serverLog(LL_WARNING,
              "Dumping running status: failed to close handle for file %s"
              "(in server root dir %s) for dumping. errno: %s",
              temp_filename, cwdp ? cwdp : "unknown", strerror(errno));
    fp = NULL;
    goto werr;
  }
  fp = NULL;
  /* Use RENAME to make sure the status file is changed atomically only
   * if the generate status file is ok. */
  if (rename(temp_filename, filename) == -1) {
    char* cwdp = getcwd(cwd, MAXPATHLEN);
    serverLog(
        LL_WARNING,
        "Dumping running status: failed to move temp file %s to the final "
        "destination %s (in server root dir %s) for dumping. errno: %s",
        temp_filename, filename, cwdp ? cwdp : "unknown", strerror(errno));
    goto werr;
  }
  return;

werr:
  if (fp != NULL) {
    fclose(fp);
  }
  unlink(temp_filename);
  return;
}

static int current_status_dump_interval_sec = 0;

// A loop to dump status periodically.
static void* status_dump_proc(void* arg) {
  while (1) {
    long long start = ustime();

    int ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (ret != 0) {
      serverLog(LL_WARNING,
                "Fatal: Failed to call pthread_setcancelstate for status dump "
                "thread - %d",
                ret);
    }

    dump_current_status();

    ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    if (ret != 0) {
      serverLog(LL_WARNING,
                "Fatal: Failed to call pthread_setcancelstate for status dump "
                "thread - %d",
                ret);
    }

    long long usec_to_sleep = ((long long)current_status_dump_interval_sec * USEC_PER_SEC) - (ustime() - start);
    if (usec_to_sleep < 0) {
      usec_to_sleep = 0;
    }
    struct timespec ts;
    ts.tv_sec = usec_to_sleep / USEC_PER_SEC;
    ts.tv_nsec = (usec_to_sleep % USEC_PER_SEC) * 1000;
    nanosleep(&ts, NULL);
  }
  return NULL;
}

// The thread to dump status.
static pthread_t status_dump_thread = 0;

// Start a thread to dump status periodically.
static void start_status_dump() {
  if (pthread_create(&status_dump_thread, NULL, status_dump_proc, NULL) != 0) {
    serverLog(LL_WARNING, "Fatal: Can't initialize status dump thread.");
    status_dump_thread = 0;
  } else {
    serverLog(LL_NOTICE, "Starting to periodically dump status to file. Dump interval: %d sec", current_status_dump_interval_sec);
  }
}

// Stop the thread for status dumping.
static void stop_status_dump() {
  if (status_dump_thread == 0) {
    return;
  }
  pid_t pid = getpid();
  if (pid != server.pid) {
    // This is child process, no status dump thread is running.
    status_dump_thread = 0;
    return;
  }
  int err = pthread_cancel(status_dump_thread);
  if (err != 0) {
    status_dump_thread = 0;
    serverLog(LL_WARNING,
              "Failed to pthread_cancel status dump thread: %s",
              strerror(err));
    return;
  }

  err = pthread_join(status_dump_thread, NULL);
  if (err == 0) {
    serverLog(LL_WARNING, "Status dump thread terminated");
  } else {
    serverLog(LL_WARNING,
              "Failed to pthread_join status dump thread: %s",
              strerror(err));
  }
  status_dump_thread = 0;
}

void reset_status_dump_thread(int new_status_dump_interval_sec) {
  stop_status_dump();
  if (new_status_dump_interval_sec > 0) {
    current_status_dump_interval_sec = new_status_dump_interval_sec;
    start_status_dump();
  }
}

void update_status_dump_interval(int new_status_dump_interval_sec) {
  if (new_status_dump_interval_sec == get_status_dump_interval_sec()) {
    serverLog(LL_NOTICE, "No update as new status dump interval is same as current: %d sec", get_status_dump_interval_sec());
    return;
  }
  reset_status_dump_thread(new_status_dump_interval_sec);
}

int get_status_dump_interval_sec() {
  if (status_dump_thread == 0) {
    return 0;
  } else {
    return current_status_dump_interval_sec;
  }
}

