/*
 * lzf_plugin.c
 */

#include "lzf.h"
#include <stdlib.h>

#define UNUSED(x) (void)(x)
/* Compression/decompression types compression plugins NEED to adhere to */
typedef struct compressionPlugin {
    const char *(*get_name)(void);
    void *(*init_options)(void **argv, int argc);
    void (*free_options)(void *option);
    unsigned int (*compress)(const void *const in_data, unsigned int in_len,
                             void *out_data, unsigned int out_len,
                             void *options);
    unsigned int (*decompress)(const void *const in_data, unsigned int in_len,
                               void *out_data, unsigned int out_len,
                               void *options);
} CompressionPlugin;

const char *get_name() { return "LZF_COMP_"; }

/* Parse and initialize additional options, if need */
void *init_options(void **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    return NULL;
}

/* Cleanup option object */
void options_cleanup(void *options) {
    if (options) {
        free(options);
    }
}

unsigned int lzf_compress_wrapper(const void *const in_data,
                                  unsigned int in_len, void *out_data,
                                  unsigned int out_len, void *options) {
    UNUSED(options);
    return lzf_compress(in_data, in_len, out_data, out_len);
}

unsigned int lzf_decompress_wrapper(const void *const in_data,
                                    unsigned int in_len, void *out_data,
                                    unsigned int out_len, void *options) {
    UNUSED(options);
    return lzf_decompress(in_data, in_len, out_data, out_len);
}

int CompressionPlugin_OnLoad(CompressionPlugin *cp) {
    cp->get_name = &get_name;
    cp->compress = &lzf_compress_wrapper;
    cp->decompress = &lzf_decompress_wrapper;
    cp->init_options = &init_options;
    cp->free_options = &options_cleanup;
    return 1;
}
