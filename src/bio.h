/* Exported API */
void bioInit(void);
void bioCreateBackgroundJob(int type, void *data);

/* Background job opcodes */
#define REDIS_BIO_CLOSE_FILE    1
