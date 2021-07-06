#include "server.h"
#include "call_reply.h"

#define REPLY_FLAG_ROOT (1<<0)
#define REPLY_FLAG_PARSED (1<<1)

struct CallReply {
    void* private_data;
    sds origilan_proto; /* available only for root reply */
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
        long long ll;    /* Reply value for integer reply. */
        double d;        /* Reply value for double reply. */
        struct CallReply *array; /* Array of sub-reply elements. */
    } val;
};

static void callReplySetSharedData(CallReply* rep, int type, const char* proto, size_t proto_len) {
    rep->type = type;
    rep->proto = proto;
    rep->proto_len = proto_len;
}

static void callReplyNull(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len);
}

static void callReplyEmptyBulk(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len);
}

static void callReplyEmptyMBulk(void* ctx, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len);
}

static void callReplyBulk(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyError(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_ERROR, proto, proto_len);
    rep->len = len;
    rep->val.str = str;
}

static void callReplySimpleStr(void* ctx, const char* str, size_t len, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyLong(void* ctx, long long val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_INTEGER, proto, proto_len);
    rep->val.ll = val;
}

static void callReplyDouble(void* ctx, double val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_DOUBLE, proto, proto_len);
    rep->val.d = val;
}

static void callReplyBool(void* ctx, int val, const char* proto, size_t proto_len) {
    CallReply* rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_BOOL, proto, proto_len);
    rep->val.ll = val;
}

static void callReplyParseCollection(ReplyParser* parser, CallReply* rep, size_t len, const char* proto, size_t elements_per_enptry) {
    rep->len = len;
    rep->val.array = zcalloc(elements_per_enptry * len * sizeof(CallReply));
    for (size_t i = 0 ; i < len * elements_per_enptry ; i += elements_per_enptry) {
        for (size_t j = 0 ; j < elements_per_enptry ; ++j) {
            parseReply(parser, rep->val.array + i + j);
            rep->val.array[i + j].flags |= REPLY_FLAG_PARSED;
            rep->val.array[i + j].private_data = rep->private_data;
        }
    }

    rep->proto = proto;
    rep->proto_len = parser->curr_location - proto;
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
}

static void callReplyMap(ReplyParser* parser, void* ctx, size_t len, const char* proto) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_MAP;
    callReplyParseCollection(parser, rep, len, proto, 2);
}

static void callReplyParseError(void* ctx) {
    CallReply* rep = ctx;
    rep->type = REDISMODULE_REPLY_UNKNOWN;
}

static void freeCallReplyInternal(CallReply* rep) {
    if (rep->type == REDISMODULE_REPLY_ARRAY || rep->type == REDISMODULE_REPLY_SET) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i);
        }
        zfree(rep->val.array);
    }

    if (rep->type == REDISMODULE_REPLY_MAP) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i * 2);
            freeCallReplyInternal(rep->val.array + i * 2 + 1);
        }
        zfree(rep->val.array);
    }
}

void freeCallReply(CallReply* rep) {
    if (!(rep->flags & REPLY_FLAG_ROOT)) {
        return;
    }

    if (rep->flags & REPLY_FLAG_PARSED) {
        freeCallReplyInternal(rep);
    }
    sdsfree(rep->origilan_proto);
    zfree(rep);

}

static void callReplyParse(CallReply* rep) {
    if (rep->flags & REPLY_FLAG_PARSED){
        return;
    }

    ReplyParser parser;
    parser.curr_location = rep->proto;
    parser.null_callback = callReplyNull;
    parser.bulk_callback = callReplyBulk;
    parser.empty_bulk_callback = callReplyEmptyBulk;
    parser.empty_mbulk_callback = callReplyEmptyMBulk;
    parser.error_callback = callReplyError;
    parser.simple_str_callback = callReplySimpleStr;
    parser.long_callback = callReplyLong;
    parser.array_callback = callReplyArray;
    parser.set_callback = callReplySet;
    parser.map_callback = callReplyMap;
    parser.double_callback = callReplyDouble;
    parser.bool_callback = callReplyBool;
    parser.error = callReplyParseError;

    parseReply(&parser, rep);
    rep->flags |= REPLY_FLAG_PARSED;
}

int callReplyType(CallReply* rep) {
    if (!rep) return REDISMODULE_REPLY_UNKNOWN;
    callReplyParse(rep);
    return rep->type;
}

const char* callReplyGetStr(CallReply* rep, size_t* len) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_STRING &&
        rep->type != REDISMODULE_REPLY_ERROR) return NULL;
    if (len) *len = rep->len;
    return rep->val.str;
}

long long callReplyGetLongLong(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_INTEGER) return LLONG_MIN;
    return rep->val.ll;
}

double callReplyGetDouble(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_DOUBLE) return LLONG_MIN;
    return rep->val.d;
}

int callReplyGetBool(CallReply* rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_BOOL) return INT_MIN;
    return rep->val.ll;
}

size_t callReplyGetLen(CallReply* rep) {
    callReplyParse(rep);
    switch(rep->type) {
    case REDISMODULE_REPLY_STRING:
    case REDISMODULE_REPLY_ERROR:
    case REDISMODULE_REPLY_ARRAY:
    case REDISMODULE_REPLY_SET:
    case REDISMODULE_REPLY_MAP:
        return rep->len;
    default:
        return 0;
    }
}

static CallReply* callReplyGetCollectionElement(CallReply* rep, size_t idx, int elements_per_entry) {
    if (idx >= rep->len * elements_per_entry) return NULL; // real len is rep->len * elements_per_entry
    return rep->val.array+idx;
}

CallReply* callReplyGetArrElement(CallReply* rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_ARRAY) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

CallReply* callReplyGetSetElement(CallReply* rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_SET) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

CallReply* callReplyGetMapKey(CallReply* rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_MAP) return NULL;
    return callReplyGetCollectionElement(rep, idx * 2, 2);

}

CallReply* callReplyGetMapVal(CallReply* rep, size_t idx){
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_MAP) return NULL;
    return callReplyGetCollectionElement(rep, idx * 2 + 1, 2);
}


const char* callReplyGetProto(CallReply* rep, size_t* proto_len) {
    *proto_len = rep->proto_len;
    return rep->proto;
}

void* callReplyGetPrivateData(CallReply* rep) {
    return rep->private_data;
}

CallReply* callReplyCreate(sds reply, void* private_data) {
    CallReply* res = zmalloc(sizeof(*res));
    res->flags = REPLY_FLAG_ROOT;
    res->origilan_proto = reply;
    res->proto = reply;
    res->proto_len = sdslen(reply);
    res->private_data = private_data;

    return res;
}

