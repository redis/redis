/*
 * Copyright (c) 2013, Nickey Woo <thenickey at gmail dot com>
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
#include <sys/types.h>
#include <dirent.h>

int system_module_init()
{
    redisLog(REDIS_WARNING, "\033[31mlibsystem_module.so initialize ...\033[0m");
    return 0;
}

void lsCommand(redisClient* c)
{
    DIR* pstDir = opendir((const char*)c->argv[1]->ptr);
    if (pstDir == NULL) {
        addReplyErrorFormat(c, "opendir fail, %s", strerror(errno));
        return;
    }

    list* dir = listCreate();
    struct dirent* pstDirent = NULL;
    while ((pstDirent = readdir(pstDir)) != NULL) {

        if(strcmp(pstDirent->d_name, ".") == 0 ||
            strcmp(pstDirent->d_name, "..") == 0)
            continue;

        sds path = sdsdup(c->argv[1]->ptr);
        size_t len = sdslen(path);
        if (path[len - 1] != '/')
            path = sdscat(path, "/");

        path = sdscat(path, pstDirent->d_name);
        listAddNodeTail(dir, path);
    }
    closedir(pstDir);

    addReplyMultiBulkLen(c, listLength(dir));
    listIter* iter = listGetIterator(dir, AL_START_HEAD);
    listNode* node = NULL;
    while ((node = listNext(iter))) {
        addReplyBulkCString(c, node->value);
        sdsfree(node->value);
        listDelNode(dir, node);
    }
    listRelease(dir);
}

START_FUNCTIONS(system_module_functions)
    FUNCTION("ls", lsCommand, 2, "r", REDIS_CMD_READONLY),
END_FUNCTIONS()

redisPlugin system_module = {
    "system",
    system_module_init,
    system_module_functions
};

REDIS_GET_MODULE(system_module)


