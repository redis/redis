/*
 * Implementation of enhanced error handling for Redis
 */

#include "enhanced_errors.h"
#include "server.h"
#include <stdarg.h>

/* Define common error templates */
const RedisEnhancedError ERR_MEMORY_LIMIT = {
    .message = "Memory limit exceeded",
    .details = "The server has reached its memory limit and cannot allocate more memory",
    .suggestion = "Consider:\n1. Increasing maxmemory setting\n2. Enabling maxmemory-policy for automatic memory management\n3. Checking for memory leaks in your application",
    .documentation = "https://redis.io/topics/memory-optimization",
    .category = ERR_CATEGORY_MEMORY
};

const RedisEnhancedError ERR_SYNTAX_INVALID = {
    .message = "Invalid syntax",
    .details = "The command syntax is incorrect",
    .suggestion = "Check command documentation for correct usage and parameters",
    .documentation = "https://redis.io/commands",
    .category = ERR_CATEGORY_SYNTAX
};

const RedisEnhancedError ERR_KEY_NOT_FOUND = {
    .message = "Key not found",
    .details = "The specified key does not exist in the database",
    .suggestion = "Verify key name and ensure it hasn't expired",
    .documentation = "https://redis.io/commands/get",
    .category = ERR_CATEGORY_DATA
};

const RedisEnhancedError ERR_WRONG_TYPE = {
    .message = "Operation against a key holding the wrong kind of value",
    .details = "The command being executed does not support the type of value stored at the key",
    .suggestion = "Use TYPE command to check the value type and ensure using compatible commands",
    .documentation = "https://redis.io/topics/data-types-intro",
    .category = ERR_CATEGORY_DATA
};

/* Function to add enhanced error reply to client */
void addEnhancedErrorReply(client *c, const RedisEnhancedError *err) {
    sds reply = sdsnew("-ERR ");
    reply = sdscat(reply, err->message);
    reply = sdscat(reply, "\r\nDetails: ");
    reply = sdscat(reply, err->details);
    reply = sdscat(reply, "\r\nSuggestion: ");
    reply = sdscat(reply, err->suggestion);
    reply = sdscat(reply, "\r\nMore info: ");
    reply = sdscat(reply, err->documentation);
    reply = sdscat(reply, "\r\n");
    
    addReplyProto(c, reply, sdslen(reply));
    sdsfree(reply);
}

/* Function to add formatted enhanced error reply */
void addEnhancedErrorReplyFormat(client *c, const RedisEnhancedError *err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds msg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    
    sds reply = sdsnew("-ERR ");
    reply = sdscat(reply, msg);
    reply = sdscat(reply, "\r\nDetails: ");
    reply = sdscat(reply, err->details);
    reply = sdscat(reply, "\r\nSuggestion: ");
    reply = sdscat(reply, err->suggestion);
    reply = sdscat(reply, "\r\nMore info: ");
    reply = sdscat(reply, err->documentation);
    reply = sdscat(reply, "\r\n");
    
    addReplyProto(c, reply, sdslen(reply));
    sdsfree(msg);
    sdsfree(reply);
}

/* Function to log enhanced error for debugging */
void logEnhancedError(const RedisEnhancedError *err) {
    serverLog(LL_WARNING,
        "Enhanced Error [%d]:\nMessage: %s\nDetails: %s\nSuggestion: %s\nMore info: %s",
        err->category, err->message, err->details,
        err->suggestion, err->documentation);
}
