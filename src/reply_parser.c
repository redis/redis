#include "reply_parser.h"
#include "server.h"

/* ----------------------------------------------------------------------------------------
 * A resp parser used to parse replies returned by RM_Call
 * or Lua redis.call. The parser introduce callbacks
 * that needs to be set by the user. Each callback represents
 * a different reply type. Each callback gets a p_ctx that
 * was given to the parseReply function. The callbacks also give the
 * protocol (underline blob) of the current reply and the size.
 *
 * 3 special callbacks also get the parser object itself
 * (array_callback, set_callback, map_callback). This is because
 * it's their responsibility to continue the parsing by calling
 * parseReply. When the user continues parsing he can give a
 * different p_ctx (this is used by CallReply to give the nested reply as the p_ctx).
 * Also notice that those 3 callbacks do not get the proto len, this is because
 * When calling those callbacks the proto len is still unknown. the user
 * can calculate the len after finish parsing the entire collection.
 * ---------------------------------------------------------------------------------------- */

static int parseBulk(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long bulklen;
    parser->curr_location = p + 2; /* for \r\n */

    string2ll(proto+1,p-proto-1,&bulklen);
    if (bulklen == -1) {
        parser->null_bulk_string_callback(p_ctx, proto, parser->curr_location - proto);
    } else {
        const char* str = parser->curr_location;
        parser->curr_location += bulklen;
        parser->curr_location += 2; /* for \r\n */
        parser->bulk_string_callback(p_ctx, str, bulklen, proto, parser->curr_location - proto);
    }

    return C_OK;
}

static int parseSimpleString(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    parser->simple_str_callback(p_ctx, proto+1, p-proto-1, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseError(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; // for \r\n
    parser->error_callback(p_ctx, proto+1, p-proto-1, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseLong(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    long long val;
    string2ll(proto+1,p-proto-1,&val);
    parser->long_callback(p_ctx, val, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseAttributes(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long len;
    string2ll(proto+1,p-proto-1,&len);
    p += 2;
    parser->curr_location = p;
    parser->attribute_callback(parser, p_ctx, len, proto);
    return C_OK;
}

static int parseVerbatimString(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long bulklen;
    parser->curr_location = p + 2; /* for \r\n */
    string2ll(proto+1,p-proto-1,&bulklen);
    const char* format = parser->curr_location;
    parser->curr_location += bulklen;
    parser->curr_location += 2; /* for \r\n */
    parser->verbatim_string_callback(p_ctx, format, format + 4, bulklen - 4, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseBigNumber(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    parser->big_number_callback(p_ctx, proto+1, p-proto-1, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseNull(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    parser->null_callback(p_ctx, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseDouble(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    char buf[MAX_LONG_DOUBLE_CHARS+1];
    size_t len = p-proto-1;
    double d;
    if (len <= MAX_LONG_DOUBLE_CHARS) {
        memcpy(buf,proto+1,len);
        buf[len] = '\0';
        d = strtod(buf,NULL); /* We expect a valid representation. */
    } else {
        d = 0;
    }
    parser->double_callback(p_ctx, d, proto, parser->curr_location - proto);
    return C_OK;
}

static int parseBool(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    parser->curr_location = p + 2; /* for \r\n */
    parser->bool_callback(p_ctx, proto[1] == 't', proto, parser->curr_location - proto);
    return C_OK;
}

static int parseArray(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long len;
    string2ll(proto+1,p-proto-1,&len);
    p += 2;
    parser->curr_location = p;
    if (len == -1) {
        parser->null_array_callback(p_ctx, proto, parser->curr_location - proto);
    } else {
        parser->array_callback(parser, p_ctx, len, proto);
    }
    return C_OK;
}

static int parseSet(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long len;
    string2ll(proto+1,p-proto-1,&len);
    p += 2;
    parser->curr_location = p;
    parser->set_callback(parser, p_ctx, len, proto);
    return C_OK;
}

static int parseMap(ReplyParser* parser, void* p_ctx) {
    const char *proto = parser->curr_location;
    char *p = strchr(proto+1,'\r');
    long long len;
    string2ll(proto+1,p-proto-1,&len);
    p += 2;
    parser->curr_location = p;
    parser->map_callback(parser, p_ctx, len, proto);
    return C_OK;
}

/**
 * Parse a reply set on parser->curr_location
 */
int parseReply(ReplyParser* parser, void* p_ctx) {
    switch(parser->curr_location[0]) {
    case '$': return parseBulk(parser, p_ctx);
    case '+': return parseSimpleString(parser, p_ctx);
    case '-': return parseError(parser, p_ctx);
    case ':': return parseLong(parser, p_ctx);
    case '*': return parseArray(parser, p_ctx);
    case '~': return parseSet(parser, p_ctx);
    case '%': return parseMap(parser, p_ctx);
    case '#': return parseBool(parser, p_ctx);
    case ',': return parseDouble(parser, p_ctx);
    case '_': return parseNull(parser, p_ctx);
    case '(': return parseBigNumber(parser, p_ctx);
    case '=': return parseVerbatimString(parser, p_ctx);
    case '|': return parseAttributes(parser, p_ctx);
    default: if (parser->error) parser->error(p_ctx);
    }
    return C_OK;
}

