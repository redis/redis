/* Exported API */
void bioInit(void);
void bioCreateBackgroundJob(int type, void *data);
unsigned long long bioPendingJobsOfType(int type);
void bioWaitPendingJobsLE(int type, unsigned long long num);

/* Background job opcodes */
#define REDIS_BIO_ZERO_OP_ID    0 /* We don't use zero as it is the most likely
                                   * passed value in case of bugs/races. */
#define REDIS_BIO_CLOSE_FILE    1 /* Deferred close(2) syscall. */
#define REDIS_BIO_MAX_OP_ID     1
