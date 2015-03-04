/* Abstraction layer for threading in tests */
#ifdef _WIN32
typedef HANDLE thd_t;
#else
typedef pthread_t thd_t;
#endif

void	thd_create(thd_t *thd, void *(*proc)(void *), void *arg);
void	thd_join(thd_t thd, void **ret);
