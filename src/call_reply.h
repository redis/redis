#ifndef SRC_CALL_REPLY_H_
#define SRC_CALL_REPLY_H_

#include "reply_parser.h"

typedef struct CallReply CallReply;

CallReply* callReplyCreate(sds reply, void* private_data);
int callReplyType(CallReply* rep);
const char* callReplyGetStr(CallReply* rep, size_t* len);
long long callReplyGetLongLong(CallReply* rep);
double callReplyGetDouble(CallReply* rep);
int callReplyGetBool(CallReply* rep);
size_t callReplyGetLen(CallReply* rep);
CallReply* callReplyGetArrElement(CallReply* rep, size_t idx);
CallReply* callReplyGetSetElement(CallReply* rep, size_t idx);
CallReply* callReplyGetMapKey(CallReply* rep, size_t idx);
CallReply* callReplyGetMapVal(CallReply* rep, size_t idx);
CallReply* callReplyGetAttribute(CallReply* rep);
CallReply* callReplyGetAttributeKey(CallReply* rep, size_t idx);
CallReply* callReplyGetAttributeVal(CallReply* rep, size_t idx);
const char* callReplyGetBigNumber(CallReply* rep, size_t* len);
const char* callReplyGetVerbatimFormat(CallReply* rep);
const char* callReplyGetVerbatimString(CallReply* rep, size_t* len);
const char* callReplyGetProto(CallReply* rep, size_t* len);
void* callReplyGetPrivateData(CallReply* rep);
void freeCallReply(CallReply* rep);


#endif /* SRC_CALL_REPLY_H_ */
