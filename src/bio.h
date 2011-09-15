/* Exported API */
void bioInit(void);
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3);
unsigned long long bioPendingJobsOfType(int type);
void bioWaitPendingJobsLE(int type, unsigned long long num);
time_t bioOlderJobOfType(int type);

/* Background job opcodes */
#define REDIS_BIO_CLOSE_FILE    0 /* Deferred close(2) syscall. */
#define REDIS_BIO_AOF_FSYNC     1 /* Deferred AOF fsync. */
#define REDIS_BIO_NUM_OPS       2
