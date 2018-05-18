/*
 * Copyright (C) 2016 Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memkind/internal/memkind_log.h>

#include <stdio.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef enum {
    MESSAGE_TYPE_INFO,
    MESSAGE_TYPE_ERROR,
    MESSAGE_TYPE_FATAL,
    MESSAGE_TYPE_MAX_VALUE,
} message_type_t;

static char* message_prefixes[MESSAGE_TYPE_MAX_VALUE] = {
    [MESSAGE_TYPE_INFO]     = "MEMKIND_INFO",
    [MESSAGE_TYPE_ERROR]    = "MEMKIND_ERROR",
    [MESSAGE_TYPE_FATAL]    = "MEMKIND_FATAL",
};

static bool log_enabled;

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static void log_init_once(void)
{
    char *memkind_debug_env= getenv("MEMKIND_DEBUG");

    if (memkind_debug_env) {
        if(strcmp(memkind_debug_env, "1") == 0) {
               log_enabled = true;
        }
        else {
            fprintf(stderr, "MEMKIND_WARNING: debug option \"%s\" unknown; Try man memkind for available options.\n", memkind_debug_env);
        }
    }
}

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_generic(message_type_t type, const char * format, va_list args)
{
    pthread_once(&init_once, log_init_once);
    if(log_enabled || (type == MESSAGE_TYPE_FATAL))
    {
        pthread_mutex_lock(&log_lock);
        fprintf(stderr, "%s: ", message_prefixes[type]);
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
        pthread_mutex_unlock(&log_lock);
    }
}

void log_info(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    log_generic(MESSAGE_TYPE_INFO, format, args);
    va_end(args);
}

void log_err(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    log_generic(MESSAGE_TYPE_ERROR, format, args);
    va_end(args);
}

void log_fatal(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    log_generic(MESSAGE_TYPE_FATAL, format, args);
    va_end(args);
}

