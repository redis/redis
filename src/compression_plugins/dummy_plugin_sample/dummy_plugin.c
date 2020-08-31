/*
 * dummy_plugin.c : simple RLE algorithm for strings containing alphabetical
 * characters
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *get_name() { return "DUMMY_COMP_"; }

/* Dummy options struct */
typedef struct dummyOptions {
    int test1;
    int test2;
} DummyOptions;

/* Parse arguments and initialize additional options, if needed */
void *init_dummy_options(void **argv, int argc) {
    if (argc >= 2) {
        DummyOptions *test_opt = (DummyOptions *)malloc(sizeof(DummyOptions));
        test_opt->test1 = atoi(argv[1]);
        test_opt->test2 = atoi(argv[2]);
        return (void *)test_opt;
    }
    return NULL;
}

/* Cleanup object */
void dummy_options_cleanup(void *options) {
    if (options) {
        free(options);
    }
}

/* Using a RLE compresssion algorithm */
unsigned int dummy_compress(const void *const in_data, unsigned int in_len,
                            void *out_data, unsigned int out_len,
                            void *options) {

    const char *input = (const char *)in_data;
    unsigned int char_count = 1;

    /* Just to testing the interface. No use of options in the dummy compression
     */
    if (options) {
        DummyOptions *opt = (DummyOptions *)options;
        assert(opt->test1 >= 0);
        assert(opt->test2 >= 0);
    }

    /* write the compressed output */
    char *out_idx = (char *)(out_data);
    for (const char *curr_char = input, *const end = input + in_len;
         curr_char != end; ++curr_char) {
        unsigned int max_size = out_len - strlen(out_idx);

        if (*(curr_char + 1) == '\0') {
            snprintf(out_idx, max_size, "%d%c", char_count, *curr_char);
            break;
        } else if (*curr_char != *(curr_char + 1)) {
            snprintf(out_idx, max_size, "%d%c", char_count, *curr_char);
            char_count = 1;
            unsigned int move_idx = strlen(out_idx);
            out_idx += move_idx;
        } else {
            ++char_count;
        }
    }
    return strlen((char *)out_data);
}

unsigned int dummy_decompress(const void *const in_data, unsigned int in_len,
                              void *out_data, unsigned int out_len,
                              void *options) {

    const char *input = (const char *)in_data;

    /* Just to testing the interface. No use of options in the dummy compression
     */
    if (options) {
        DummyOptions *opt = (DummyOptions *)options;
        assert(opt->test1 >= 0);
        assert(opt->test2 >= 0);
    }

    /* write the decompressed output*/
    char *out_idx = (char *)(out_data);
    for (const char *curr_char = input,
                    *Num = input, *const end = input + in_len;
         curr_char != end; ++curr_char) {
        if (isdigit(*curr_char)) {
            continue;
        }
        for (int i = 0; i < atoi(Num); ++i) {
            *out_idx++ = *curr_char;
        }
        Num = curr_char + 1;
    }
    out_idx = '\0';

    /* Verify output length is equal to the original (uncompressed) input
     * length*/
    if (strlen((char *)out_data) != out_len) {
        return 0;
    }
    return 1;
}

int CompressionPlugin_OnLoad(CompressionPlugin *cp) {
    cp->get_name = &get_name;
    cp->compress = &dummy_compress;
    cp->decompress = &dummy_decompress;
    cp->init_options = &init_dummy_options;
    cp->free_options = &dummy_options_cleanup;
    return 1;
}

typedef struct dummyData {
    unsigned int sz;
    char compressed[];
} dummyData;

int main(int argc, char **argv) {
    const char *in_data = "aaaabbbbccccdddd";
    // const char *in_data = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    unsigned int in_len = strlen(in_data);
    dummyData *compressed_data = malloc(sizeof(dummyData) + in_len - 1);
    DummyOptions *opt = init_dummy_options((void **)argv, argc);
    if ((compressed_data->sz = dummy_compress(
             in_data, in_len, compressed_data->compressed, in_len, opt)) == 0) {
        printf("Compression failed!");
        return 0;
    }
    printf("compressed: %s\n", compressed_data->compressed);

    dummyData *decompressed_data = malloc(sizeof(dummyData) + in_len);
    const char *comp_data = "4a4b4c4d";
    if ((dummy_decompress(comp_data, strlen(comp_data),
                          decompressed_data->compressed, in_len, opt)) == 0) {
        printf("Decompression failed!");
        return 0;
    }
    printf("decompressed: %s\n", decompressed_data->compressed);
    dummy_options_cleanup(opt);
    return 1;
}
