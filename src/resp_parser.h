/*
 * Copyright (c) 2021-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef SRC_RESP_PARSER_H_
#define SRC_RESP_PARSER_H_

#include <stddef.h>

typedef struct ReplyParser ReplyParser;

typedef struct ReplyParserCallbacks {
    /* Called when the parser reaches an empty mbulk ('*-1') */
    void (*null_array_callback)(void *ctx, const char *proto, size_t proto_len);

    /* Called when the parser reaches an empty bulk ('$-1') (bulk len is -1) */
    void (*null_bulk_string_callback)(void *ctx, const char *proto, size_t proto_len);

    /* Called when the parser reaches a bulk ('$'), which is passed as 'str' along with its length 'len' */
    void (*bulk_string_callback)(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);

    /* Called when the parser reaches an error ('-'), which is passed as 'str' along with its length 'len' */
    void (*error_callback)(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);

    /* Called when the parser reaches a simple string ('+'), which is passed as 'str' along with its length 'len' */
    void (*simple_str_callback)(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);

    /* Called when the parser reaches a long long value (':'), which is passed as an argument 'val' */
    void (*long_callback)(void *ctx, long long val, const char *proto, size_t proto_len);

    /* Called when the parser reaches an array ('*'). The array length is passed as an argument 'len' */
    void (*array_callback)(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);

    /* Called when the parser reaches a set ('~'). The set length is passed as an argument 'len' */
    void (*set_callback)(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);

    /* Called when the parser reaches a map ('%'). The map length is passed as an argument 'len' */
    void (*map_callback)(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);

    /* Called when the parser reaches a bool ('#'), which is passed as an argument 'val' */
    void (*bool_callback)(void *ctx, int val, const char *proto, size_t proto_len);

    /* Called when the parser reaches a double (','), which is passed as an argument 'val' */
    void (*double_callback)(void *ctx, double val, const char *proto, size_t proto_len);

    /* Called when the parser reaches a big number ('('), which is passed as 'str' along with its length 'len' */
    void (*big_number_callback)(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len);

    /* Called when the parser reaches a string ('='), which is passed as 'str' along with its 'format' and length 'len' */
    void (*verbatim_string_callback)(void *ctx, const char *format, const char *str, size_t len, const char *proto, size_t proto_len);

    /* Called when the parser reaches an attribute ('|'). The attribute length is passed as an argument 'len' */
    void (*attribute_callback)(struct ReplyParser *parser, void *ctx, size_t len, const char *proto);

    /* Called when the parser reaches a null ('_') */
    void (*null_callback)(void *ctx, const char *proto, size_t proto_len);

    void (*error)(void *ctx);
} ReplyParserCallbacks;

struct ReplyParser {
    /* The current location in the reply buffer, needs to be set to the beginning of the reply */
    const char *curr_location;
    ReplyParserCallbacks callbacks;
};

int parseReply(ReplyParser *parser, void *p_ctx);

#endif /* SRC_RESP_PARSER_H_ */
