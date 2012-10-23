#include "config.h"

#ifdef __LINUX_GLIBC25__
int sync_file_range (int fd, __off64_t from, __off64_t to, unsigned int flags)
{
  return syscall (__NR_sync_file_range, fd,
                         __LONG_LONG_PAIR ((long) (from >> 32), (long) from),
                         __LONG_LONG_PAIR ((long) (to >> 32), (long) to),
                         flags);
}
#endif
