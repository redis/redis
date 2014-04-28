/* Every time the Redis Git SHA1 or Dirty status changes only this file
 * small file is recompiled, as we access this information in all the other
 * files using this functions. */

#include "release.h"

char *redisGitSHA1(void) {
    return REDIS_GIT_SHA1;
}

char *redisGitDirty(void) {
    return REDIS_GIT_DIRTY;
}
