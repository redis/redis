/* compression.c
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

#include "compression_plugin_interface.h"
#include "server.h"
#include <dlfcn.h>

/* Hash table of Compression Plugins. SDS ->
                                    CompressionPluginCtx ptr.*/
static dict *compression_plugins;

/* Initialization of compression plugin objects */
void compressionPluginInit(void) {
    server.loadcompression_queue = listCreate();
    compression_plugins = dictCreate(&keyptrDictType, NULL);
}

/* Returns the pointer to the CompressionPluginCtx for the given compression
   plugin name. If not found, return NULL */
CompressionPluginCtx *compressionPluginCtxLookupByName(char *name) {
    CompressionPluginCtx *ctx =
        dictFetchValue(compression_plugins, sdsnew(name));
    if (ctx != NULL) {
        if (ctx->compression_plugin != NULL) {
            return ctx;
        }
    }
    serverLog(LL_WARNING, "Compression plugin %s not found.", name);
    return NULL;
}

/* Add loaded compression interface to the compression plugin dictionary
 * On success C_OK is returned, otherwise C_ERR is returned */
int compressionAddPlugin(CompressionPluginCtx *ctx, void *handle) {
    ctx->handle = handle;
    sds name = sdsnew(ctx->name);
    if (dictAdd(compression_plugins, name, ctx) != DICT_OK) {
        return C_ERR;
    }
    return C_OK;
}

/* Load a plugin and initialize it. On success compression object is returned,
 * otherwise C_ERR is returned.
 *
 * A compression plugin "must" export CompressionPlugin_OnLoad function, which
 * should implement the compressionPlugin interface.
 * Example code fragment:
 *
 *      int CompressionPlugin_OnLoad(CompressionPlugin *cp) {
 *          // some code here ...
 *      }
 * And is supposed to always return 1.
 */
int compressionPluginLoad(const char *path, void **compression_argv,
                          int compression_argc, int isdefault) {
    void *plugin_handle;
    CompressionPluginCtx *plugin_ctx;
    int (*plugin_onload)(void *);

    plugin_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (plugin_handle == NULL) {
        serverLog(LL_WARNING, "Compression plugin %s failed to load: %s", path,
                  dlerror());
        return C_ERR;
    }
    plugin_onload = (int (*)(void *))(unsigned long)dlsym(
        plugin_handle, "CompressionPlugin_OnLoad");
    if (plugin_onload == NULL) {
        dlclose(plugin_handle);
        serverLog(
            LL_WARNING,
            "Compression Plugin %s does not export CompressionPlugin_OnLoad "
            "symbol. Compression plugin not loaded.",
            path);
        return C_ERR;
    }

    plugin_ctx = zmalloc(sizeof(CompressionPluginCtx));
    plugin_ctx->compression_plugin = zmalloc(sizeof(CompressionPlugin));
    if (plugin_onload(plugin_ctx->compression_plugin) == 0) {
        serverLog(LL_WARNING,
                  "Compression Plugin %s: CompressionPlugin_OnLoad failed. "
                  "Compression plugin not loaded.",
                  path);
        goto plugin_err_cleanup;
    }

    /* get_name() returns the plugin name, which is required by Redis */
    plugin_ctx->name = plugin_ctx->compression_plugin->get_name();
    if (!plugin_ctx->name) {
        serverLog(LL_NOTICE,
                  "The field 'name' is required to be set in the plugin. "
                  "Plugin cannnot be "
                  "loaded. Please fix compression plugin loaded from %s",
                  path);
        goto plugin_err_cleanup;
    }

    /* init_options() returns pointer to an options structure, it could be
     * NULL*/
    plugin_ctx->options = plugin_ctx->compression_plugin->init_options(
        compression_argv, compression_argc);

    /* Compression plugin interface successfully loaded!
     * Add the new compression interface exported by plugin to
     * compression_plugins. */
    if (compressionAddPlugin(plugin_ctx, plugin_handle) == C_ERR) {
        serverLog(LL_WARNING,
                  "Compression plugin %s cannot be added to Redis. The plugin "
                  "name %s may already exist.",
                  path, plugin_ctx->name);
        goto plugin_err_cleanup;
    }
    serverLog(LL_NOTICE, "Compression plugin '%s' loaded from %s",
              plugin_ctx->name, path);

    if (isdefault == 1) {
        /* Check if already set by another compression plugin*/
        if (server.compression_plugin_ctx) {
            serverLog(LL_WARNING,
                      "Compression plugin %s cannot be set as default "
                      "compressor. A default compressor "
                      "%s already exists.",
                      plugin_ctx->name, server.compression_plugin_ctx->name);
            goto plugin_err_cleanup;
        }
        server.compression_plugin_ctx = plugin_ctx;
        serverLog(LL_NOTICE, "Using %s as default compressor",
                  plugin_ctx->name);
    }
    return C_OK;

plugin_err_cleanup:
    if (plugin_ctx->options) {
        plugin_ctx->compression_plugin->free_options(plugin_ctx->options);
    }
    dlclose(plugin_handle);
    zfree(plugin_ctx->compression_plugin);
    zfree(plugin_ctx);
    return C_ERR;
}

/* Unload the compression plugin registered with the specified name. On success
 * C_OK is returned, otherwise C_ERR is returned */
int compressionPluginUnload(char *name) {
    CompressionPluginCtx *ctx =
        dictFetchValue(compression_plugins, sdsnew(name));
    if (ctx->options) {
        ctx->compression_plugin->free_options(ctx->options);

        /* Unload the dynamic library. */
        if (dlclose(ctx->handle) == -1) {
            char *error = dlerror();
            if (error == NULL)
                error = "Unknown error";
            serverLog(LL_WARNING,
                      "Error when trying to close the %s plugin: %s", name,
                      error);
            return C_ERR;
        }
        /* free structures */
        zfree(ctx->compression_plugin);
        zfree(ctx);
    }
    serverLog(LL_NOTICE, "Compression plugin %s unloaded successfully", name);
    return C_OK;
}

/* Load all the compression plugins in the server.loadcompression_queue list,
 * which is populated by `loadcompression` directives in the configuration file.
 * We can't load compression plugins directly when processing the configuration
 * file because the server must be fully initialized before loading compression
 * plugins.
 *
 * The function aborts the server on errors, since to start with missing
 * compression plugins specified in configuration file is not considered sane:
 * clients may rely on the existence of given commands, loading RDB also may
 * need some compression plugin to exist. */
void compressionPluginLoadFromQueue(void) {
    listIter li;
    listNode *ln;

    listRewind(server.loadcompression_queue, &li);
    while ((ln = listNext(&li))) {
        struct compressionLoadQueueEntry *loadcomp = ln->value;
        if (compressionPluginLoad(loadcomp->path, (void **)loadcomp->argv,
                                  loadcomp->argc,
                                  loadcomp->set_default) == C_ERR) {
            serverLog(LL_WARNING,
                      "Issue/s occured while configuring compression plugin %s "
                      "in Redis. Exiting.",
                      loadcomp->path);
            exit(1);
        }
    }
}
