/*
 * Enhanced error handling for Redis
 * This file contains improved error messages and debugging information
 */

#ifndef __REDIS_ENHANCED_ERRORS_H
#define __REDIS_ENHANCED_ERRORS_H

#include "server.h"

/* Error categories for better organization and handling */
typedef enum {
    ERR_CATEGORY_MEMORY,      /* Memory-related errors */
    ERR_CATEGORY_NETWORK,     /* Network and connection errors */
    ERR_CATEGORY_SYNTAX,      /* Command syntax errors */
    ERR_CATEGORY_DATA,        /* Data-related errors */
    ERR_CATEGORY_CLUSTER,     /* Cluster-specific errors */
    ERR_CATEGORY_REPLICATION, /* Replication-related errors */
    ERR_CATEGORY_INTERNAL,    /* Internal server errors */
    ERR_CATEGORY_CLIENT       /* Client-related errors */
} RedisErrorCategory;

/* Enhanced error message structure */
typedef struct {
    const char *message;          /* Main error message */
    const char *details;          /* Detailed explanation */
    const char *suggestion;       /* Suggested solution */
    const char *documentation;    /* Link to relevant documentation */
    RedisErrorCategory category;  /* Error category */
} RedisEnhancedError;

/* Function declarations */
void addEnhancedErrorReply(client *c, const RedisEnhancedError *err);
void addEnhancedErrorReplyFormat(client *c, const RedisEnhancedError *err, const char *fmt, ...);
void logEnhancedError(const RedisEnhancedError *err);

/* Common error templates */
extern const RedisEnhancedError ERR_MEMORY_LIMIT;
extern const RedisEnhancedError ERR_SYNTAX_INVALID;
extern const RedisEnhancedError ERR_KEY_NOT_FOUND;
extern const RedisEnhancedError ERR_WRONG_TYPE;

#endif /* __REDIS_ENHANCED_ERRORS_H */
