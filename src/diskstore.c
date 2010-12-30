/* diskstore.c implements a very simple disk backed key-value store used
 * by Redis for the "disk" backend. This implementation uses the filesystem
 * to store key/value pairs. Every file represents a given key.
 *
 * The key path is calculated using the SHA1 of the key name. For instance
 * the key "foo" is stored as a file name called:
 *
 *  /0b/ee/0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 * The couples of characters from the hex output of SHA1 are also used
 * to locate two two levels of directories to store the file (as most
 * filesystems are not able to handle too many files in a single dir).
 *
 * In the end there are 65536 final directories (256 directories inside
 * every 256 top level directories), so that with 1 billion of files every
 * directory will contain in the average 15258 entires, that is ok with
 * most filesystems implementation.
 *
 * Note that since Redis supports multiple databases, the actual key name
 * is:
 *
 *  /0b/ee/<dbid>_0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 *  so for instance if the key is inside DB 0:
 *
 *  /0b/ee/0_0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 * The actaul implementation of this disk store is highly dependant to the
 * filesystem implementation itself. This implementation may be replaced by
 * a B+TREE implementation in future implementations.
 *
 * Data ok every key is serialized using the same format used for .rdb
 * serialization. Everything is serialized on every entry: key name,
 * ttl information in case of keys with an associated expire time, and the
 * serialized value itself.
 *
 * Because the format is the same of the .rdb files it is trivial to create
 * an .rdb file starting from this format just by mean of scanning the
 * directories and concatenating entries, with the sole addition of an
 * .rdb header at the start and the end-of-db opcode at the end.
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

#include <fcntl.h>
#include <sys/stat.h>

int create256dir(char *prefix) {
    char buf[1024];
    int j;

    for (j = 0; j < 256; j++) {
        snprintf(buf,sizeof(buf),"%s%02x",prefix,j);
        if (mkdir(buf,0755) == -1) {
            redisLog(REDIS_WARNING,"Error creating dir %s for diskstore: %s",
                buf,strerror(errno));
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

int dsOpen(void) {
    struct stat sb;
    int retval, j;
    char *path = server.ds_path;
    char buf[1024];

    if ((retval = stat(path,&sb) == -1) && errno != ENOENT) {
        redisLog(REDIS_WARNING, "Error opening disk store at %s: %s",
                path, strerror(errno));
        return REDIS_ERR;
    }

    /* Directory already in place. Assume everything is ok. */
    if (retval == 0 && S_ISDIR(sb.st_mode)) {
        redisLog(REDIS_NOTICE,"Disk store %s exists", path);
        return REDIS_OK;
    }

    /* File exists but it's not a directory */
    if (retval == 0 && !S_ISDIR(sb.st_mode)) {
        redisLog(REDIS_WARNING,"Disk store at %s is not a directory", path);
        return REDIS_ERR;
    }

    /* New disk store, create the directory structure now, as creating
     * them in a lazy way is not a good idea, after very few insertions
     * we'll need most of the 65536 directories anyway. */
    redisLog(REDIS_NOTICE,"Disk store %s does not exist: creating", path);
    if (mkdir(path,0755) == -1) {
        redisLog(REDIS_WARNING,"Disk store init failed creating dir %s: %s",
            path, strerror(errno));
        return REDIS_ERR;
    }
    /* Create the top level 256 directories */
    snprintf(buf,sizeof(buf),"%s/",path);
    if (create256dir(buf) == REDIS_ERR) return REDIS_ERR;

    /* For every 256 top level dir, create 256 nested dirs */
    for (j = 0; j < 256; j++) {
        snprintf(buf,sizeof(buf),"%s/%02x/",path,j);
        if (create256dir(buf) == REDIS_ERR) return REDIS_ERR;
    }
    return REDIS_OK;
}

int dsClose(void) {
    return REDIS_OK;
}

int dsSet(redisDb *db, robj *key, robj *val) {
}

robj *dsGet(redisDb *db, robj *key) {
    return createStringObject("foo",3);
}

int dsDel(redisDb *db, robj *key) {
}

int dsExists(redisDb *db, robj *key) {
}

int dsFlushDb(int dbid) {
}
