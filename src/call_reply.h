#ifndef SRC_CALL_REPLY_H_
#define SRC_CALL_REPLY_H_

#include "reply_parser.h"

/* --------------------------------------------------------
 * Opaque struct used by module API to parse and
 * analyze commands replies which returns using RM_Call
 * -------------------------------------------------------- */
typedef struct CallReply CallReply;

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
CallReply* callReplyCreate(sds reply, void* private_data);

/**
 * Return the call reply type (REDISMODULE_REPLY_...)
 */
int callReplyType(CallReply* rep);

/**
 * Return reply as string and len, applicabale for:
 * * REDISMODULE_REPLY_STRING
 * * REDISMODULE_REPLY_ERROR
 */
const char* callReplyGetStr(CallReply* rep, size_t* len);

/**
 * Return long long value of the reply, applicabale for:
 * * REDISMODULE_REPLY_INTEGER
 */
long long callReplyGetLongLong(CallReply* rep);

/**
 * Return double value of the reply, applicabale for:
 * * REDISMODULE_REPLY_DOUBLE
 */
double callReplyGetDouble(CallReply* rep);

/**
 * Return bool value of the reply, applicabale for:
 * * REDISMODULE_REPLY_BOOL
 */
int callReplyGetBool(CallReply* rep);

/**
 * Return reply len, applicabale for:
 * * REDISMODULE_REPLY_STRING
 * * REDISMODULE_REPLY_ERROR
 * * REDISMODULE_REPLY_ARRAY
 * * REDISMODULE_REPLY_SET
 * * REDISMODULE_REPLY_MAP
 */
size_t callReplyGetLen(CallReply* rep);

/**
 * Return array reply element at a given index, applicabale for:
 * * REDISMODULE_REPLY_ARRAY
 */
CallReply* callReplyGetArrElement(CallReply* rep, size_t idx);

/**
 * Return set reply element at a given index, applicabale for:
 * * REDISMODULE_REPLY_SET
 */
CallReply* callReplyGetSetElement(CallReply* rep, size_t idx);

/**
 * Return map reply key at a given index, applicabale for:
 * * REDISMODULE_REPLY_MAP
 */
CallReply* callReplyGetMapKey(CallReply* rep, size_t idx);

/**
 * Return map reply value at a given index, applicabale for:
 * * REDISMODULE_REPLY_MAP
 */
CallReply* callReplyGetMapVal(CallReply* rep, size_t idx);

/**
 * Return the current reply blob. The return value is borrowed
 * and can only be used as long as the CallReply is alive
 */
const char* callReplyGetProto(CallReply* rep, size_t* len);

/**
 * Return CallReply private data as it was give when the reply was
 * created using callReplyCreate
 */
void* callReplyGetPrivateData(CallReply* rep);

/**
 * Free the given call reply and its children (in case of nested reply) recursively
 */
void freeCallReply(CallReply* rep);


#endif /* SRC_CALL_REPLY_H_ */
