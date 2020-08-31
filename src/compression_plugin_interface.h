/* comression.h interface for pluggable compression
 * Copyright (c) 2020, Fatima Saleem <fatima.saleem@intel.com>
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

#ifndef __COMPRESSION_PLUGIN_INTERFACE_H__
#define __COMPRESSION_PLUGIN_INTERFACE_H__

#include <stddef.h>

/* Compression/decompression types compression plugins NEED to adhere to.
 * Copy the following struct or include this file in your plugin
 *
 * get_name: A compression plugin must return a unique name in order to be
 * loaded in Redis.
 * init_options: A compression plugin may optionally parse arguments and
 * interpret them as options/flags, if needed. the function should return
 * pointer to an object with options/flags, otherwise return NULL.
 * free_options: If a compression plugin implements init_options, give it a
 * chance to cleanup the object, if needed.
 * compress: A compression plugin must implement a compress function. Compress
 * in_len bytes stored at the memory block starting at in_data and write the
 * result to out_data, up to a maximum length of out_len bytes. If the output
 * buffer is not large enough or any error occurs return 0, otherwise return
 * the number of bytes used.
 * decompress: A compression plugin must implement a decompress function.
 * Decompress compressed data stored at location in_data and length in_len. The
 * result will be stored at out_data up to a maximum of out_len characters.
 * If the output buffer is not large enough to hold the decompressed
 * data, a 0 is returned. Otherwise the number of decompressed bytes (i.e. the
 * original length of the data) is returned.
 */
typedef struct compressionPlugin {
    const char *(*get_name)(void);
    void *(*init_options)(void **argv, int argc);
    void (*free_options)(void *options);
    unsigned int (*compress)(const void *const in_data, unsigned int in_len,
                             void *out_data, unsigned int out_len,
                             void *options);
    unsigned int (*decompress)(const void *const in_data, unsigned int in_len,
                               void *out_data, unsigned int out_len,
                               void *options);
} CompressionPlugin;

typedef struct compressionPluginCtx {
    void *handle;
    const char *name;
    void *options; /* Optionally stored options or arguments passsed to plugin.
                      The structure that 'options' point to, can be defined and
                      interpreted by plugin */
    CompressionPlugin *compression_plugin;
} CompressionPluginCtx;

/* Plugin compression Redis helpers */
void compressionPluginInit(void);
void compressionPluginLoadFromQueue(void);
CompressionPluginCtx *compressionPluginCtxLookupByName(char *name);
int compressionPluginUnload(char *name);
#endif /* __COMPRESSION_PLUGIN_INTERFACE_H__ */
