#include "redis.h"
#include <sys/time.h>

/* initialized to zero, we depend on wrapping for cycling */
u_int16_t sequence;

void incridCommand(redisClient *c) {
    struct timeval tv;
    sds id_buf;

    if ((gettimeofday(&tv, NULL)) == -1) {
        addReplyError(c, "cannot get time of day");
	return;
    }

    id_buf = sdscatprintf(sdsempty(),
                          "+0x%08lx%08x%02x%02x%02x%02x%02x%02x%04x\r\n",
			  tv.tv_sec,
                          tv.tv_usec,
                          server.id_generation_name[0],
                          server.id_generation_name[1],
                          server.id_generation_name[2],
                          server.id_generation_name[3],
                          server.id_generation_name[4],
                          server.id_generation_name[5],
                          sequence++);
    addReplySds(c, id_buf);
}
