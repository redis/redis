#include "server.h"
#include "call_reply.h"

#define REPLY_FLAG_ROOT (1<<0)
#define REPLY_FLAG_PARSED (1<<1)
#define REPLY_FLAG_RESP3 (1<<2)

/* --------------------------------------------------------
 * Opaque struct used by module API to parse and
 * analyze commands replies which returns using RM_Call
 * -------------------------------------------------------- */
struct CallReply {
    void* private_data;
    sds original_proto; /* available only for root reply */
    const char* proto;
    size_t proto_len;
    int type;       /* REPLY_... */
    int flags;       /* REPLY_FLAG... */
    size_t len;     /* Len of strings or num of elements of arrays. */
    union {
        const char *str; /* String pointer for string and error replies. This
                            does not need to be freed, always points inside
                            a reply->proto buffer of the reply object or, in
                            case of array elements, of parent reply objects. */
        struct {
            const char *str;
            const char *format;
        }verbatim_str;   /* Reply value for verbatim string */
        long long ll;    /* Reply value for integer reply. */
        double d;        /* Reply value for double reply. */
        struct CallReply *array; /* Array of sub-reply elements. used for set, array, map, and attribute */
    } val;

    struct CallReply *attribute; /* attribute reply, NULL if not exists */
};

static void callReplySetSharedData(CallReply* rep, int type, const char* proto, size_t proto_len, int extra_flags) {
    rep->type = type;
    rep->proto = proto;
    rep->proto_len = proto_len;
    rep->flags |= extra_flags;
}

static void callReplyNull(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, REPLY_FLAG_RESP3);
}

static void callReplyNullBulkString(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, 0);
}

static void callReplyNullArray(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, 0);
}

static void callReplyBulkString(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyError(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_ERROR, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplySimpleStr(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyLong(void* ctx, long long val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_INTEGER, proto, proto_len, 0);
    rep->val.ll = val;
}

static void callReplyDouble(void* ctx, double val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_DOUBLE, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.d = val;
}

static void callReplyVerbatimString(void* ctx, const char* format, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_VERBATIM_STRING, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.verbatim_str.str = str;
    rep->val.verbatim_str.format = format;
}

static void callReplyBigNumber(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_BIG_NUMBER, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyBool(void* ctx, int val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_BOOL, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.ll = val;
}

static void callReplyParseCollection(ReplyParser* parser, CallReply* rep, size_t len, const char* proto, size_t elements_per_entry) {
    rep->len = len;
    rep->val.array = zcalloc(elements_per_entry * len * sizeof(CallReply));
    for (size_t i = 0 ; i < len * elements_per_entry ; i += elements_per_entry) {
        for (size_t j = 0 ; j < elements_per_entry ; ++j) {
            parseReply(parser, rep->val.array + i + j);
            rep->val.array[i + j].flags |= REPLY_FLAG_PARSED;
            rep->val.array[i + j].private_data = rep->private_data;
            if (rep->val.array[i + j].flags & REPLY_FLAG_RESP3) {
                /* if one of the subreplies are resp3 then the current reply is also resp3 */
                rep->flags |= REPLY_FLAG_RESP3;
            }
        }
    }
    rep->proto = proto;
    rep->proto_len = parser->curr_location - proto;
}

static void callReplyAttribute(ReplyParser* parser, void* ctx, size_t len, const char* proto) {
    CallReply* rep = ctx;
    rep->attribute = zcalloc(sizeof(CallReply));

    /* continue parsing the attribute reply */
    rep->attribute->len = len;
    rep->attribute->type = REDISMODULE_REPLY_ATTRIBUTE;
    callReplyParseCollection(parser, rep->attribute, len, proto, 2);
    rep->attribute->flags |= REPLY_FLAG_PARSED | REPLY_FLAG_RESP3;
    rep->attribute->private_data = rep->private_data;

    /* continue parsing the reply */
    parseReply(parser, rep);

    /* in this case we need to fix the proto address and len, it should start from the attribute */
    rep->proto = proto;
    rep->proto_len = parser->curr_location - proto;
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyArray(ReplyParser* parser, void* ctx, size_t len, const char* proto) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_ARRAY;
    callReplyParseCollection(parser, rep, len, proto, 1);
}

static void callReplySet(ReplyParser* parser, void* ctx, size_t len, const char* proto) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_SET;
    callReplyParseCollection(parser, rep, len, proto, 1);
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyMap(ReplyParser* parser, void* ctx, size_t len, const char* proto) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_MAP;
    callReplyParseCollection(parser, rep, len, proto, 2);
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyParseError(void* ctx) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_UNKNOWN;
}

/**
 * Recursivally free the current call reply and its sub-replies
 */
static void freeCallReplyInternal(CallReply* rep) {
    if (rep->type == REDISMODULE_REPLY_ARRAY || rep->type == REDISMODULE_REPLY_SET) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i);
        }
        zfree(rep->val.array);
    }

    if (rep->type == REDISMODULE_REPLY_MAP || rep->type == REDISMODULE_REPLY_ATTRIBUTE) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i * 2);
            freeCallReplyInternal(rep->val.array + i * 2 + 1);
        }
        zfree(rep->val.array);
    }

    if (rep->attribute) {
        freeCallReplyInternal(rep->attribute);
        zfree(rep->attribute);
    }
}

/**
 * Free the given call reply and its children (in case of nested reply) recursively.
 * If a private data was set when the CallReply was created it will not be free, its
 * the user responsibility to free it before free the CallReply.
 */
void freeCallReply(CallReply* rep) {
    if (!(rep->flags & REPLY_FLAG_ROOT)) {
        return;
    }
    if (rep->flags & REPLY_FLAG_PARSED) {
        freeCallReplyInternal(rep);
    }
    sdsfree(rep->original_proto);
    zfree(rep);
}

/**
 * Parsing the buffer located on rep->original_proto as CallReply
 * using ReplyParser.
 */
static void callReplyParse(CallReply* rep) {
    if (rep->flags & REPLY_FLAG_PARSED){
        return;
    }

    ReplyParser parser;
    parser.curr_location = rep->proto;
    parser.null_callback = callReplyNull;
    parser.bulk_string_callback = callReplyBulkString;
    parser.null_bulk_string_callback = callReplyNullBulkString;
    parser.null_array_callback = callReplyNullArray;
    parser.error_callback = callReplyError;
    parser.simple_str_callback = callReplySimpleStr;
    parser.long_callback = callReplyLong;
    parser.array_callback = callReplyArray;
    parser.set_callback = callReplySet;
    parser.map_callback = callReplyMap;
    parser.double_callback = callReplyDouble;
    parser.bool_callback = callReplyBool;
    parser.big_number_callback = callReplyBigNumber;
    parser.verbatim_string_callback = callReplyVerbatimString;
    parser.attribute_callback = callReplyAttribute;
    parser.error = callReplyParseError;

    parseReply(&parser, rep);
    rep->flags |= REPLY_FLAG_PARSED;
}

/**
 * Return the call reply type (REDISMODULE_REPLY_...)
 */
int callReplyType(CallReply* rep) {
    if (!rep) return REDISMODULE_REPLY_UNKNOWN;
    callReplyParse(rep);
    return rep->type;
}

/**
 * Return reply as string and len, applicabale for:
 * * REDISMODULE_REPLY_STRING
 * * REDISMODULE_REPLY_ERROR
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply
 * The returned value is not NULL terminated and its mandatory to
 * give the len argument
 */
const char* callReplyGetStr(CallReply* rep, size_t* len) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_STRING &&
        rep->type != REDISMODULE_REPLY_ERROR) return NULL;
    if (len) *len = rep->len;
    return rep->val.str;
}

/**
 * Return long long value of the reply, applicabale for:
 * * REDISMODULE_REPLY_INTEGER
 */
long long callReplyGetLongLong(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_INTEGER) return LLONG_MIN;
    return rep->val.ll;
}

/**
 * Return double value of the reply, applicabale for:
 * * REDISMODULE_REPLY_DOUBLE
 */
double callReplyGetDouble(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_DOUBLE) return LLONG_MIN;
    return rep->val.d;
}

/**
 * Return bool value of the reply, applicabale for:
 * * REDISMODULE_REPLY_BOOL
 */
int callReplyGetBool(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_BOOL) return INT_MIN;
    return rep->val.ll;
}

/**
 * Return reply len, applicabale for:
 * * REDISMODULE_REPLY_STRING
 * * REDISMODULE_REPLY_ERROR
 * * REDISMODULE_REPLY_ARRAY
 * * REDISMODULE_REPLY_SET
 * * REDISMODULE_REPLY_MAP
 * * REDISMODULE_REPLY_ATTRIBUTE
 */
size_t callReplyGetLen(CallReply* rep) {
    callReplyParse(rep);
    switch(rep->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
    case REDISMODULE_REPLY_ARRAY:
    case REDISMODULE_REPLY_SET:
    case REDISMODULE_REPLY_MAP:
    case REDISMODULE_REPLY_ATTRIBUTE:
        return rep->len;
    default:
        return 0;
    }
}

static CallReply* callReplyGetCollectionElement(CallReply* rep, size_t idx, int elements_per_entry) {
    if (idx >= rep->len * elements_per_entry) return NULL; // real len is rep->len * elements_per_entry
    return rep->val.array+idx;
}

/**
 * Return array reply element at a given index, applicabale for:
 * * REDISMODULE_REPLY_ARRAY
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply. In addition there is not need
 * to manually free the returned CallReply, it will be freed when
 * the root CallReplied will be freed.
 */
CallReply* callReplyGetArrElement(CallReply* rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_ARRAY) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

/**
 * Return set reply element at a given index, applicabale for:
 * * REDISMODULE_REPLY_SET
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply. In addition there is not need
 * to manually free the returned CallReply, it will be freed when
 * the root CallReplied will be freed.
 */
CallReply* callReplyGetSetElement(CallReply* rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_SET) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

static int callReplyGetMapElementInternal(CallReply* rep, size_t idx, CallReply** key, CallReply** val, int type) {
    callReplyParse(rep);
    if (rep->type != type) return C_ERR;
    if (idx >= rep->len) return C_ERR;
    if (key) *key = callReplyGetCollectionElement(rep, idx * 2, 2);
    if (val) *val = callReplyGetCollectionElement(rep, idx * 2 + 1, 2);
    return C_OK;
}

/**
 * Retrieve map reply key and value at a given index, applicabale for:
 * * REDISMODULE_REPLY_MAP
 *
 * The key and val are both output params which can be NULL (in this case they are not needed).
 * Return C_OK on success and C_ERR if reply type is wrong or if the idx is out of range.
 *
 * The returned values are only borrowed and their lifetime is
 * as long as the given CallReply. In addition there is no need
 * to manually free the returned CallReplies, it will be freed when
 * the root CallReplied will be freed.
 */
int callReplyGetMapElement(CallReply* rep, size_t idx, CallReply** key, CallReply** val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, REDISMODULE_REPLY_MAP);
}

/**
 * Return reply attribute if exists or NULL, applicabale for all replies.
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply. In addition there is no need
 * to manually free the returned CallReply, it will be freed when
 * the root CallReplied will be freed.
 */
CallReply* callReplyGetAttribute(CallReply* rep) {
    return rep->attribute;
}

/**
 * Retrieve attribute reply key and value at a given index, applicabale for:
 * * REDISMODULE_REPLY_ATTRIBUTE
 *
 * The key and val are both output params which can be NULL (in this case they are not needed).
 * Return C_OK on success and C_ERR if reply type is wrong or if the idx is out of range.
 *
 * The returned values are only borrowed and their lifetime is
 * as long as the given CallReply. In addition there is no need
 * to manually free the returned CallReplies, it will be freed when
 * the root CallReplied will be freed.
 */
int callReplyGetAttributeElement(CallReply* rep, size_t idx, CallReply** key, CallReply** val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, REDISMODULE_REPLY_MAP);
}

/**
 * Return big number reply value, applicabale for:
 * * REDISMODULE_REPLY_BIG_NUMBER
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply.
 * The returned value is promised to be a big number as describe
 * on RESP3 spacifications.
 * The returned value is not NULL terminated and its mandatory to
 * give the len argument
 */
const char* callReplyGetBigNumber(CallReply* rep, size_t* len) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_BIG_NUMBER) return NULL;
    *len = rep->len;
    return rep->val.str;
}

/**
 * Return verbatim string reply value, applicabale for:
 * * REDISMODULE_REPLY_VERBATIM_STRING
 *
 * An optional output argument can be given to get verbatim reply
 * format, or NULL if not needed.
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply.
 * The returned value is not NULL terminated and its mandatory to
 * give the len argument
 */
const char* callReplyGetVerbatim(CallReply* rep, size_t* len, const char** format){
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_VERBATIM_STRING) return NULL;
    *len = rep->len;
    if (format) *format = rep->val.verbatim_str.format;
    return rep->val.verbatim_str.str;
}

/**
 * Return the current reply blob. The return value is borrowed
 * and can only be used as long as the CallReply is alive
 *
 * The returned value is only borrowed and its lifetime is
 * as long as the given CallReply.
 */
const char* callReplyGetProto(CallReply* rep, size_t* proto_len) {
    *proto_len = rep->proto_len;
    return rep->proto;
}

/**
 * Return CallReply private data as it was give when the reply was
 * created using callReplyCreate
 */
void* callReplyGetPrivateData(CallReply* rep) {
    return rep->private_data;
}

/**
 * Return true if the reply or one of its children format is resp3
 */
int callReplyIsResp3(CallReply* rep) {
    return rep->flags & REPLY_FLAG_RESP3;
}

/**
 * Create a new CallReply struct from the give reply blob.
 * The function takes ownership on the reply blob which means
 * that it should not be used after calling this function.
 * The reply blob will be freed when the returned CallReply
 * object will be freed using freeCallReply.
 *
 * The given private_data can be retriv from the
 * returned CallReply object or any of its children (in case
 * of nested reply) using callReplyGetPrivateData,
 */
CallReply* callReplyCreate(sds reply, void* private_data) {
    CallReply* res = zmalloc(sizeof(*res));
    res->flags = REPLY_FLAG_ROOT;
    res->original_proto = reply;
    res->proto = reply;
    res->proto_len = sdslen(reply);
    res->private_data = private_data;
    res->attribute = NULL;
    return res;
}
