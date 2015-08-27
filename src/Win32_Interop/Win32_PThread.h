

#include <windows.h>
#include <errno.h>

#ifndef _SIGSET_T_
#define _SIGSET_T_
typedef size_t _sigset_t;
#define sigset_t _sigset_t
#endif /* _SIGSET_T_ */

#ifndef SIG_SETMASK
#define SIG_SETMASK (0)
#define SIG_BLOCK   (1)
#define SIG_UNBLOCK (2)
#endif /* SIG_SETMASK */

/* threads avoiding pthread.h */
#define pthread_mutex_t CRITICAL_SECTION
#define pthread_attr_t ssize_t

#define pthread_mutex_init(a,b) (InitializeCriticalSectionAndSpinCount((a), 0x80000400),0)
#define pthread_mutex_destroy(a) DeleteCriticalSection((a))
#define pthread_mutex_lock EnterCriticalSection
#define pthread_mutex_unlock LeaveCriticalSection

#define pthread_equal(t1, t2) ((t1) == (t2))

#define pthread_attr_init(x) (*(x) = 0)
#define pthread_attr_getstacksize(x, y) (*(y) = *(x))
#define pthread_attr_setstacksize(x, y) (*(x) = y)

#define pthread_t unsigned int

int pthread_create(pthread_t *thread, const void *unused, void *(*start_routine)(void*), void *arg);

pthread_t pthread_self(void);

typedef struct {
    CRITICAL_SECTION waiters_lock;
    LONG waiters;
    int was_broadcast;
    HANDLE sema;
    HANDLE continue_broadcast;
} pthread_cond_t;

int pthread_cond_init(pthread_cond_t *cond, const void *unused);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);

int pthread_detach(pthread_t thread);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
