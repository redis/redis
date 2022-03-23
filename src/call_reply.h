/*
 * Copyright (c) 2009-2021, Redis Labs Ltd.
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

#ifndef SRC_CALL_REPLY_H_
#define SRC_CALL_REPLY_H_

#include "resp_parser.h"

typedef struct CallReply CallReply;

CallReply *callReplyCreate(sds reply, list *deferred_error_list, void *private_data);
CallReply *callReplyCreateError(sds reply, void *private_data);
int callReplyType(CallReply *rep);
const char *callReplyGetString(CallReply *rep, size_t *len);
long long callReplyGetLongLong(CallReply *rep);
double callReplyGetDouble(CallReply *rep);
int callReplyGetBool(CallReply *rep);
size_t callReplyGetLen(CallReply *rep);
CallReply *callReplyGetArrayElement(CallReply *rep, size_t idx);
CallReply *callReplyGetSetElement(CallReply *rep, size_t idx);
int callReplyGetMapElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val);
CallReply *callReplyGetAttribute(CallReply *rep);
int callReplyGetAttributeElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val);
const char *callReplyGetBigNumber(CallReply *rep, size_t *len);
const char *callReplyGetVerbatim(CallReply *rep, size_t *len, const char **format);
const char *callReplyGetProto(CallReply *rep, size_t *len);
void *callReplyGetPrivateData(CallReply *rep);
int callReplyIsResp3(CallReply *rep);
list *callReplyDeferredErrorList(CallReply *rep);
void freeCallReply(CallReply *rep);

#endif /* SRC_CALL_REPLY_H_ */
