/*
 * File: redis_table.c
 * Author: Raphael Drai
 * Email: raphael.drai@gmail.com
 * Date: October 3, 2025
 * Description: This program is a Redis module that implements SQL-like tables 
 * with full CRUD operations, explicit index control, comparison operators, 
 * and support for multiple data types.
 */

#include "redismodule.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Initial capacity for dynamic arrays in filtering operations
#define INITIAL_FILTER_CAPACITY 100

// Default maximum number of rows to scan in a single query operation
#define DEFAULT_MAX_ROWS_SCAN_LIMIT 100000

// Configurable scan limit (can be changed via CONFIG SET)
static long long g_max_rows_scan_limit = DEFAULT_MAX_ROWS_SCAN_LIMIT;

static inline RedisModuleString *fmt(RedisModuleCtx *ctx, const char *fmt, RedisModuleString *a) {
    return RedisModule_CreateStringPrintf(ctx, fmt, RedisModule_StringPtrLen(a, NULL));
}
static inline RedisModuleString *fmt2(RedisModuleCtx *ctx, const char *fmt, RedisModuleString *a, RedisModuleString *b) {
    return RedisModule_CreateStringPrintf(ctx, fmt, RedisModule_StringPtrLen(a, NULL), RedisModule_StringPtrLen(b, NULL));
}
static inline RedisModuleString *fmt3(RedisModuleCtx *ctx, const char *fmt, RedisModuleString *a, RedisModuleString *b, RedisModuleString *c) {
    return RedisModule_CreateStringPrintf(ctx, fmt,
        RedisModule_StringPtrLen(a, NULL),
        RedisModule_StringPtrLen(b, NULL),
        RedisModule_StringPtrLen(c, NULL));
}

// Split "col=value" or "col>value" etc into (col, op, value)
static int split_condition(RedisModuleCtx *ctx, RedisModuleString *in, 
                          RedisModuleString **colOut, char *opOut, RedisModuleString **valOut) {
    size_t len; const char *s = RedisModule_StringPtrLen(in, &len);
    const char *op = NULL;
    size_t oplen = 0;
    
    // Look for operators: >=, <=, >, <, =
    for (size_t i = 0; i < len - 1; i++) {
        if (s[i] == '>' && s[i+1] == '=') { op = &s[i]; oplen = 2; break; }
        if (s[i] == '<' && s[i+1] == '=') { op = &s[i]; oplen = 2; break; }
    }
    if (!op) {
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '=' || s[i] == '>' || s[i] == '<') { op = &s[i]; oplen = 1; break; }
        }
    }
    
    if (!op || op == s || (size_t)(op - s + oplen) >= len) return REDISMODULE_ERR;
    
    *colOut = RedisModule_CreateString(ctx, s, (size_t)(op - s));
    if (oplen == 2) {
        opOut[0] = op[0]; opOut[1] = op[1]; opOut[2] = '\0';
    } else {
        opOut[0] = op[0]; opOut[1] = '\0';
    }
    *valOut = RedisModule_CreateString(ctx, op + oplen, len - (size_t)(op - s) - oplen);
    return REDISMODULE_OK;
}

// Check if column is indexed
static int is_column_indexed(RedisModuleCtx *ctx, RedisModuleString *table, RedisModuleString *col) {
    RedisModule_AutoMemory(ctx);
    RedisModuleString *metaKey = fmt(ctx, "idx:meta:%s", table);
    RedisModuleCallReply *r = RedisModule_Call(ctx, "SISMEMBER", "ss", metaKey, col);
    return (r && RedisModule_CallReplyType(r) == REDISMODULE_REPLY_INTEGER && 
            RedisModule_CallReplyInteger(r) == 1) ? 1 : 0;
}

// Compare two values based on operator and type
static int compare_values(const char *v1, const char *v2, const char *op, const char *type) {
    if (strcmp(type, "integer") == 0) {
        long long n1 = atoll(v1);
        long long n2 = atoll(v2);
        if (strcmp(op, "=") == 0) return n1 == n2;
        if (strcmp(op, ">") == 0) return n1 > n2;
        if (strcmp(op, "<") == 0) return n1 < n2;
        if (strcmp(op, ">=") == 0) return n1 >= n2;
        if (strcmp(op, "<=") == 0) return n1 <= n2;
    } else if (strcmp(type, "float") == 0) {
        double d1 = atof(v1);
        double d2 = atof(v2);
        if (strcmp(op, "=") == 0) return d1 == d2;
        if (strcmp(op, ">") == 0) return d1 > d2;
        if (strcmp(op, "<") == 0) return d1 < d2;
        if (strcmp(op, ">=") == 0) return d1 >= d2;
        if (strcmp(op, "<=") == 0) return d1 <= d2;
    } else if (strcmp(type, "date") == 0) {
        // Date comparison as string (YYYY-MM-DD format sorts correctly)
        int cmp = strcmp(v1, v2);
        if (strcmp(op, "=") == 0) return cmp == 0;
        if (strcmp(op, ">") == 0) return cmp > 0;
        if (strcmp(op, "<") == 0) return cmp < 0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
    } else {
        // String comparison
        int cmp = strcmp(v1, v2);
        if (strcmp(op, "=") == 0) return cmp == 0;
        if (strcmp(op, ">") == 0) return cmp > 0;
        if (strcmp(op, "<") == 0) return cmp < 0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
    }
    return 0;
}

static int ensure_schema_exists(RedisModuleCtx *ctx, RedisModuleString *schemaName) {
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *k = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", schemaName), REDISMODULE_READ);
    return RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_EMPTY ? REDISMODULE_OK : REDISMODULE_ERR;
}

static int ensure_table_exists(RedisModuleCtx *ctx, RedisModuleString *fullTableName) {
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *k = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", fullTableName), REDISMODULE_READ);
    return RedisModule_KeyType(k) == REDISMODULE_KEYTYPE_HASH ? REDISMODULE_OK : REDISMODULE_ERR;
}

static RedisModuleString* extract_schema(RedisModuleCtx *ctx, RedisModuleString *fullTable) {
    size_t len; const char *s = RedisModule_StringPtrLen(fullTable, &len);
    const char *dot = memchr(s, '.', len);
    if (!dot) return NULL;
    return RedisModule_CreateString(ctx, s, (size_t)(dot - s));
}

static RedisModuleString* extract_table(RedisModuleCtx *ctx, RedisModuleString *fullTable) {
    size_t len; const char *s = RedisModule_StringPtrLen(fullTable, &len);
    const char *dot = memchr(s, '.', len);
    if (!dot) return NULL;
    return RedisModule_CreateString(ctx, dot + 1, len - (size_t)(dot - s) - 1);
}

static int validate_and_typecheck(RedisModuleCtx *ctx, RedisModuleString *fullTableName,
                                  RedisModuleString *col, RedisModuleString *val) {
    RedisModule_AutoMemory(ctx);
    RedisModuleKey *schemaKey = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", fullTableName), REDISMODULE_READ);
    if (RedisModule_KeyType(schemaKey) != REDISMODULE_KEYTYPE_HASH) return REDISMODULE_ERR;
    RedisModuleString *typeStr = NULL;
    if (RedisModule_HashGet(schemaKey, REDISMODULE_HASH_NONE, col, &typeStr, NULL) != REDISMODULE_OK || !typeStr) return REDISMODULE_ERR;
    size_t tlen; const char *t = RedisModule_StringPtrLen(typeStr, &tlen);
    
    if (tlen == 7 && strncasecmp(t, "integer", 7) == 0) {
        // Validate integer
        size_t vlen; const char *vs = RedisModule_StringPtrLen(val, &vlen);
        if (vlen == 0) return REDISMODULE_ERR;
        size_t i = 0; if (vs[0] == '-' || vs[0] == '+') i = 1; if (i >= vlen) return REDISMODULE_ERR;
        for (; i < vlen; i++) if (vs[i] < '0' || vs[i] > '9') return REDISMODULE_ERR;
    } else if (tlen == 5 && strncasecmp(t, "float", 5) == 0) {
        // Validate float (simple check for digits, optional decimal point)
        size_t vlen; const char *vs = RedisModule_StringPtrLen(val, &vlen);
        if (vlen == 0) return REDISMODULE_ERR;
        int hasDot = 0;
        size_t i = 0; if (vs[0] == '-' || vs[0] == '+') i = 1; if (i >= vlen) return REDISMODULE_ERR;
        for (; i < vlen; i++) {
            if (vs[i] == '.') { if (hasDot) return REDISMODULE_ERR; hasDot = 1; }
            else if (vs[i] < '0' || vs[i] > '9') return REDISMODULE_ERR;
        }
    } else if (tlen == 4 && strncasecmp(t, "date", 4) == 0) {
        // Validate date format YYYY-MM-DD
        size_t vlen; const char *vs = RedisModule_StringPtrLen(val, &vlen);
        if (vlen != 10) return REDISMODULE_ERR;
        if (vs[4] != '-' || vs[7] != '-') return REDISMODULE_ERR;
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (vs[i] < '0' || vs[i] > '9') return REDISMODULE_ERR;
        }
    }
    // String type (no validation needed)
    return REDISMODULE_OK;
}
// Validate string length (max 64 characters)
static int validate_string_length(RedisModuleCtx *ctx, RedisModuleString *str, const char *name) {
    size_t len;
    RedisModule_StringPtrLen(str, &len);
    if (len > 64) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "ERR incorrect %s name, it exceeds the limit of 64 characters", name);
        return RedisModule_ReplyWithError(ctx, error_msg);
    }
    return REDISMODULE_OK;
}
static int TableNamespaceCreateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    // Validate namespace length (max 64 characters)
    if (validate_string_length(ctx, argv[1], "namespace") != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    RedisModuleKey *k = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", argv[1]), REDISMODULE_WRITE);
    if (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, "ERR namespace already exists");
    RedisModule_StringSet(k, RedisModule_CreateString(ctx, "1", 1));
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* ================== TABLE.NAMESPACE.VIEW [<namespace>] ================== */
static int TableNamespaceViewCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1 && argc != 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    
    const char *filter_namespace = NULL;
    size_t filter_len = 0;
    if (argc == 2) {
        filter_namespace = RedisModule_StringPtrLen(argv[1], &filter_len);
    }
    
    // Use SCAN instead of KEYS to avoid blocking Redis on large keyspaces
    // Collect namespace:table pairs with dynamic allocation
    typedef struct {
        char namespace[256];
        char table[256];
    } TableEntry;
    
    size_t capacity = 100;
    size_t count = 0;
    TableEntry *entries = RedisModule_Alloc(sizeof(TableEntry) * capacity);
    if (entries == NULL) {
        return RedisModule_ReplyWithError(ctx, "ERR out of memory");
    }
    
    // SCAN cursor-based iteration (non-blocking)
    unsigned long long cursor = 0;
    do {
        // Convert cursor to string for SCAN command
        char cursorBuf[32];
        snprintf(cursorBuf, sizeof(cursorBuf), "%llu", cursor);
        
        // Call SCAN with cursor as string, MATCH pattern
        RedisModuleCallReply *scanReply = RedisModule_Call(ctx, "SCAN", "ccc", 
                                                            cursorBuf,
                                                            "MATCH", "schema:*.*");
        if (!scanReply || RedisModule_CallReplyType(scanReply) != REDISMODULE_REPLY_ARRAY) {
            RedisModule_Free(entries);
            return RedisModule_ReplyWithArray(ctx, 0);
        }
        
        // Extract new cursor from reply[0]
        RedisModuleCallReply *cursorReply = RedisModule_CallReplyArrayElement(scanReply, 0);
        if (!cursorReply) break;
        
        size_t cursorStrLen;
        const char *cursorStr = RedisModule_CallReplyStringPtr(cursorReply, &cursorStrLen);
        if (!cursorStr) break;
        
        cursor = strtoull(cursorStr, NULL, 10);
        
        // Extract keys array from reply[1]
        RedisModuleCallReply *keysReply = RedisModule_CallReplyArrayElement(scanReply, 1);
        if (!keysReply || RedisModule_CallReplyType(keysReply) != REDISMODULE_REPLY_ARRAY) {
            continue;
        }
        
        size_t n = RedisModule_CallReplyLength(keysReply);
        for (size_t i = 0; i < n; i++) {
            RedisModuleCallReply *keyReply = RedisModule_CallReplyArrayElement(keysReply, i);
            size_t keylen;
            const char *keystr = RedisModule_CallReplyStringPtr(keyReply, &keylen);
            
            // Skip if not in format "schema:namespace.table"
            if (keylen < 8 || strncmp(keystr, "schema:", 7) != 0) continue;
            
            const char *fullname = keystr + 7;  // Skip "schema:"
            size_t fullname_len = keylen - 7;
            
            // Find the dot separator
            const char *dot = memchr(fullname, '.', fullname_len);
            if (!dot) continue;  // Not a table (just a namespace marker)
            
            size_t ns_len = (size_t)(dot - fullname);
            size_t tbl_len = fullname_len - ns_len - 1;
            
            // Apply filter if provided
            if (filter_namespace && (ns_len != filter_len || strncmp(fullname, filter_namespace, ns_len) != 0)) {
                continue;
            }
            
            // Resize array if needed
            if (count >= capacity) {
                capacity *= 2;
                TableEntry *newEntries = RedisModule_Realloc(entries, sizeof(TableEntry) * capacity);
                if (newEntries == NULL) {
                    RedisModule_Free(entries);
                    return RedisModule_ReplyWithError(ctx, "ERR out of memory");
                }
                entries = newEntries;
            }
            
            // Store entry
            if (ns_len < 256 && tbl_len < 256) {
                strncpy(entries[count].namespace, fullname, ns_len);
                entries[count].namespace[ns_len] = '\0';
                strncpy(entries[count].table, dot + 1, tbl_len);
                entries[count].table[tbl_len] = '\0';
                count++;
            }
        }
    } while (cursor != 0);
    
    // Simple bubble sort by namespace (then by table)
    if (count > 1) {
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = 0; j < count - i - 1; j++) {
                int cmp = strcmp(entries[j].namespace, entries[j+1].namespace);
                if (cmp > 0 || (cmp == 0 && strcmp(entries[j].table, entries[j+1].table) > 0)) {
                    TableEntry temp = entries[j];
                    entries[j] = entries[j+1];
                    entries[j+1] = temp;
                }
            }
        }
    }
    
    // Reply with array of "namespace:table" strings
    RedisModule_ReplyWithArray(ctx, count);
    for (size_t i = 0; i < count; i++) {
        RedisModuleString *result = RedisModule_CreateStringPrintf(ctx, "%s:%s", entries[i].namespace, entries[i].table);
        RedisModule_ReplyWithString(ctx, result);
    }
    
    RedisModule_Free(entries);
    return REDISMODULE_OK;
}

/* ================== TABLE.SCHEMA.VIEW <namespace.table> ================== */
static int TableSchemaViewCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");
    
    // Get all columns from table schema
    RedisModuleCallReply *fields = RedisModule_Call(ctx, "HGETALL", "s", fmt(ctx, "schema:%s", argv[1]));
    if (!fields || RedisModule_CallReplyType(fields) != REDISMODULE_REPLY_ARRAY) {
        return RedisModule_ReplyWithArray(ctx, 0);
    }
    
    size_t n = RedisModule_CallReplyLength(fields);
    size_t numCols = n / 2;
    
    // Reply format: array of [column, type, indexed]
    RedisModule_ReplyWithArray(ctx, numCols);
    
    for (size_t i = 0; i < n; i += 2) {
        RedisModuleCallReply *colReply = RedisModule_CallReplyArrayElement(fields, i);
        RedisModuleCallReply *typeReply = RedisModule_CallReplyArrayElement(fields, i + 1);
        
        RedisModuleString *col = RedisModule_CreateStringFromCallReply(colReply);
        RedisModuleString *type = RedisModule_CreateStringFromCallReply(typeReply);
        
        // Check if indexed
        int indexed = is_column_indexed(ctx, argv[1], col);
        
        // Reply with array: [column, type, indexed]
        RedisModule_ReplyWithArray(ctx, 3);
        RedisModule_ReplyWithString(ctx, col);
        RedisModule_ReplyWithString(ctx, type);
        RedisModule_ReplyWithSimpleString(ctx, indexed ? "true" : "false");
    }
    
    return REDISMODULE_OK;
}

/* ================== TABLE.SCHEMA.CREATE <namespace.table> <col:type:index> ... ================== */
static int TableSchemaCreateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    RedisModuleString *schema = extract_schema(ctx, argv[1]);
    if (!schema) return RedisModule_ReplyWithError(ctx, "ERR table name must be namespace.table");

    // Validate namespace length (max 64 characters)
    if (validate_string_length(ctx, schema, "namespace") != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    // Validate table name length (max 64 characters)
    RedisModuleString *table = extract_table(ctx, argv[1]);
    if (!table || validate_string_length(ctx, table, "table") != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    if (ensure_schema_exists(ctx, schema) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR namespace does not exist");

    RedisModuleKey *schemaKey = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", argv[1]), REDISMODULE_WRITE);
    if (RedisModule_KeyType(schemaKey) != REDISMODULE_KEYTYPE_EMPTY)
        return RedisModule_ReplyWithError(ctx, "ERR table schema already exists");

    RedisModuleString *metaKey = fmt(ctx, "idx:meta:%s", argv[1]);

    // Parse col:type:index (index is optional, defaults to true for backward compat)
    for (int i = 2; i < argc; i++) {
        size_t len; const char *s = RedisModule_StringPtrLen(argv[i], &len);
        const char *colon1 = memchr(s, ':', len);
        if (!colon1 || colon1 == s) 
            return RedisModule_ReplyWithError(ctx, "ERR format: <col:type> or <col:type:index>");
        
        size_t col_len = (size_t)(colon1 - s);
        const char *colon2 = memchr(colon1 + 1, ':', len - col_len - 1);
        
        RedisModuleString *col = RedisModule_CreateString(ctx, s, col_len);
        RedisModuleString *typ;
        int indexed = 1; // default true
        
        if (colon2) {
            // col:type:index format
            typ = RedisModule_CreateString(ctx, colon1 + 1, (size_t)(colon2 - colon1 - 1));
            const char *idx_str = colon2 + 1;
            size_t idx_len = len - (size_t)(colon2 - s) - 1;
            if (idx_len == 5 && strncasecmp(idx_str, "false", 5) == 0) indexed = 0;
            else if (idx_len == 4 && strncasecmp(idx_str, "true", 4) == 0) indexed = 1;
            else return RedisModule_ReplyWithError(ctx, "ERR index must be 'true' or 'false'");
        } else {
            // col:type format (backward compat - index defaults to true)
            typ = RedisModule_CreateString(ctx, colon1 + 1, len - col_len - 1);
        }
        
        RedisModule_HashSet(schemaKey, REDISMODULE_HASH_NONE, col, typ, NULL);
        if (indexed) {
            RedisModule_Call(ctx, "SADD", "ss", metaKey, col);
        }
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* ================== TABLE.SCHEMA.ALTER <namespace.table> ADD/DROP COLUMN/INDEX ... ================== */
static int TableSchemaAlterCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");
    
    size_t oplen; const char *op = RedisModule_StringPtrLen(argv[2], &oplen);
    size_t targetlen; const char *target = RedisModule_StringPtrLen(argv[3], &targetlen);
    
    RedisModuleKey *schemaKey = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", argv[1]), REDISMODULE_WRITE);
    RedisModuleString *metaKey = fmt(ctx, "idx:meta:%s", argv[1]);
    
    if (oplen == 3 && strncasecmp(op, "ADD", 3) == 0) {
        if (targetlen == 6 && strncasecmp(target, "COLUMN", 6) == 0) {
            // ADD COLUMN col:type[:index]
            if (argc != 5) return RedisModule_ReplyWithError(ctx, "ERR ADD COLUMN requires col:type[:index]");
            size_t len; const char *s = RedisModule_StringPtrLen(argv[4], &len);
            const char *colon1 = memchr(s, ':', len);
            if (!colon1) return RedisModule_ReplyWithError(ctx, "ERR format: col:type[:index]");
            
            RedisModuleString *col = RedisModule_CreateString(ctx, s, (size_t)(colon1 - s));
            const char *colon2 = memchr(colon1 + 1, ':', len - (size_t)(colon1 - s) - 1);
            RedisModuleString *typ;
            int indexed = 1;
            
            if (colon2) {
                typ = RedisModule_CreateString(ctx, colon1 + 1, (size_t)(colon2 - colon1 - 1));
                const char *idx = colon2 + 1;
                if (strncasecmp(idx, "false", 5) == 0) indexed = 0;
            } else {
                typ = RedisModule_CreateString(ctx, colon1 + 1, len - (size_t)(colon1 - s) - 1);
            }
            
            RedisModule_HashSet(schemaKey, REDISMODULE_HASH_NONE, col, typ, NULL);
            if (indexed) RedisModule_Call(ctx, "SADD", "ss", metaKey, col);
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
            
        } else if (targetlen == 5 && strncasecmp(target, "INDEX", 5) == 0) {
            // ADD INDEX col - build index for existing data
            if (argc != 5) return RedisModule_ReplyWithError(ctx, "ERR ADD INDEX requires column name");
            RedisModuleString *col = argv[4];
            
            // Verify column exists in table schema
            RedisModuleString *typeStr = NULL;
            if (RedisModule_HashGet(schemaKey, REDISMODULE_HASH_NONE, col, &typeStr, NULL) != REDISMODULE_OK || !typeStr)
                return RedisModule_ReplyWithError(ctx, "ERR column does not exist");
            
            // Add to index metadata
            RedisModule_Call(ctx, "SADD", "ss", metaKey, col);
            
            // Build index for all existing rows
            RedisModuleString *rowsSet = fmt(ctx, "rows:%s", argv[1]);
            RedisModuleCallReply *rows = RedisModule_Call(ctx, "SMEMBERS", "s", rowsSet);
            if (rows && RedisModule_CallReplyType(rows) == REDISMODULE_REPLY_ARRAY) {
                size_t n = RedisModule_CallReplyLength(rows);
                for (size_t i = 0; i < n; i++) {
                    RedisModuleCallReply *e = RedisModule_CallReplyArrayElement(rows, i);
                    RedisModuleString *rowId = RedisModule_CreateStringFromCallReply(e);
                    RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], rowId);
                    
                    // Get column value from row
                    RedisModuleCallReply *valReply = RedisModule_Call(ctx, "HGET", "ss", rowKey, col);
                    if (valReply && RedisModule_CallReplyType(valReply) == REDISMODULE_REPLY_STRING) {
                        RedisModuleString *val = RedisModule_CreateStringFromCallReply(valReply);
                        RedisModuleString *idxKey = fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val);
                        RedisModule_Call(ctx, "SADD", "ss", idxKey, rowId);
                    }
                }
            }
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
        }
    } else if (oplen == 4 && strncasecmp(op, "DROP", 4) == 0) {
        if (targetlen == 5 && strncasecmp(target, "INDEX", 5) == 0) {
            // DROP INDEX col - remove index metadata and delete all index keys
            // WARNING: Known race condition - metadata removed before keys deleted
            // Concurrent queries may return empty results during deletion
            // TODO (v2.2): Reverse order or implement soft-delete tombstone
            if (argc != 5) return RedisModule_ReplyWithError(ctx, "ERR DROP INDEX requires column name");
            RedisModuleString *col = argv[4];
            
            // Remove from index metadata (ATOMIC - fast)
            // RACE CONDITION: Queries checking after this point will think index doesn't exist
            RedisModule_Call(ctx, "SREM", "ss", metaKey, col);
            
            // Delete all index keys for this column using SCAN (NON-ATOMIC - slow)
            // RACE CONDITION: Keys being deleted while queries might try to use them
            // Build pattern: idx:table:col:*
            RedisModuleString *pattern = fmt2(ctx, "idx:%s:%s:*", argv[1], col);
            size_t patternLen;
            const char *patternStr = RedisModule_StringPtrLen(pattern, &patternLen);
            
            // Use SCAN to find and delete index keys
            unsigned long long cursor = 0;
            do {
                // Convert cursor to string for SCAN command
                char cursorBuf[32];
                snprintf(cursorBuf, sizeof(cursorBuf), "%llu", cursor);
                
                RedisModuleCallReply *scanReply = RedisModule_Call(ctx, "SCAN", "ccc",
                                                                    cursorBuf,
                                                                    "MATCH", patternStr);
                if (!scanReply || RedisModule_CallReplyType(scanReply) != REDISMODULE_REPLY_ARRAY) {
                    break;
                }
                
                // Extract new cursor from reply[0]
                RedisModuleCallReply *cursorReply = RedisModule_CallReplyArrayElement(scanReply, 0);
                if (!cursorReply) break;
                
                size_t cursorStrLen;
                const char *cursorStr = RedisModule_CallReplyStringPtr(cursorReply, &cursorStrLen);
                if (!cursorStr) break;
                
                cursor = strtoull(cursorStr, NULL, 10);
                
                // Extract and delete keys from reply[1]
                RedisModuleCallReply *keysReply = RedisModule_CallReplyArrayElement(scanReply, 1);
                if (keysReply && RedisModule_CallReplyType(keysReply) == REDISMODULE_REPLY_ARRAY) {
                    size_t n = RedisModule_CallReplyLength(keysReply);
                    for (size_t i = 0; i < n; i++) {
                        RedisModuleCallReply *keyReply = RedisModule_CallReplyArrayElement(keysReply, i);
                        RedisModuleString *key = RedisModule_CreateStringFromCallReply(keyReply);
                        RedisModule_Call(ctx, "DEL", "s", key);
                    }
                }
            } while (cursor != 0);
            
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
        }
    }
    
    return RedisModule_ReplyWithError(ctx, "ERR syntax: ADD COLUMN col:type[:index] | ADD INDEX col | DROP INDEX col");
}

/* ================== TABLE.INSERT <namespace.table> <col>=<value> ... ================== */
static int TableInsertCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);

    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");

    RedisModuleString *idKey = fmt(ctx, "table:%s:id", argv[1]);
    RedisModuleCallReply *idReply = RedisModule_Call(ctx, "INCR", "s", idKey);
    long long idNum = idReply ? RedisModule_CallReplyInteger(idReply) : 0;
    RedisModuleString *rowId = RedisModule_CreateStringFromLongLong(ctx, idNum);

    RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], rowId);
    RedisModuleKey *row = RedisModule_OpenKey(ctx, rowKey, REDISMODULE_WRITE);
    RedisModuleString *rowsSet = fmt(ctx, "rows:%s", argv[1]);

    for (int i = 2; i < argc; i++) {
        RedisModuleString *col=NULL, *val=NULL;
        char op[3];
        if (split_condition(ctx, argv[i], &col, op, &val) != REDISMODULE_OK || strcmp(op, "=") != 0)
            return RedisModule_ReplyWithError(ctx, "ERR each field must be <col>=<value>");
        if (validate_and_typecheck(ctx, argv[1], col, val) != REDISMODULE_OK)
            return RedisModule_ReplyWithError(ctx, "ERR invalid column or type");
        RedisModule_HashSet(row, REDISMODULE_HASH_NONE, col, val, NULL);
        
        // Only create index if column is indexed
        if (is_column_indexed(ctx, argv[1], col)) {
            RedisModuleString *idxKey = fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val);
            RedisModule_Call(ctx, "SADD", "ss", idxKey, rowId);
        }
    }

    RedisModule_Call(ctx, "SADD", "ss", rowsSet, rowId);
    return RedisModule_ReplyWithString(ctx, rowId);
}

// Collect members of an index set into a dictionary
static void dict_add_set_members(RedisModuleCtx *ctx, RedisModuleDict *dict, RedisModuleString *setKey) {
    RedisModuleCallReply *r = RedisModule_Call(ctx, "SMEMBERS", "s", setKey);
    if (!r || RedisModule_CallReplyType(r) != REDISMODULE_REPLY_ARRAY) return;
    size_t n = RedisModule_CallReplyLength(r);
    for (size_t i = 0; i < n; i++) {
        RedisModuleCallReply *e = RedisModule_CallReplyArrayElement(r, i);
        RedisModuleString *id = RedisModule_CreateStringFromCallReply(e);
        RedisModule_DictSet(dict, id, NULL);
    }
}

// Filter dictionary based on comparison operator
// Returns 0 on success, -1 if scan limit exceeded
static int dict_filter_condition(RedisModuleCtx *ctx, RedisModuleDict *dict, RedisModuleString *table,
                                  RedisModuleString *col, const char *op, RedisModuleString *val) {
    // Get column type
    RedisModuleKey *schemaKey = RedisModule_OpenKey(ctx, fmt(ctx, "schema:%s", table), REDISMODULE_READ);
    RedisModuleString *typeStr = NULL;
    RedisModule_HashGet(schemaKey, REDISMODULE_HASH_NONE, col, &typeStr, NULL);
    const char *type = "string";
    if (typeStr) {
        size_t tlen; const char *t = RedisModule_StringPtrLen(typeStr, &tlen);
        if (tlen == 7 && strncasecmp(t, "integer", 7) == 0) type = "integer";
        else if (tlen == 5 && strncasecmp(t, "float", 5) == 0) type = "float";
        else if (tlen == 4 && strncasecmp(t, "date", 4) == 0) type = "date";
    }

    size_t vlen; const char *vstr = RedisModule_StringPtrLen(val, &vlen);

    // First pass: collect keys to remove (using dynamic allocation to avoid arbitrary limits)
    size_t toRemoveCapacity = INITIAL_FILTER_CAPACITY;
    size_t removeCount = 0;
    RedisModuleString **toRemove = RedisModule_Alloc(sizeof(RedisModuleString*) * toRemoveCapacity);
    if (toRemove == NULL) {
        // Out of memory - cannot filter, return without modifying dict
        return -1;
    }

    RedisModuleDictIter *it = RedisModule_DictIteratorStartC(dict, "^", NULL, 0);
    RedisModuleString *key; void *dummy;
    size_t rowsScanned = 0;
    while ((key = RedisModule_DictNext(ctx, it, &dummy)) != NULL) {
        // Check scan limit to prevent blocking Redis on large datasets
        if (++rowsScanned > (size_t)g_max_rows_scan_limit) {
            RedisModule_DictIteratorStop(it);
            RedisModule_Free(toRemove);
            return -1; // Scan limit exceeded
        }
        
        RedisModuleString *rowKey = fmt2(ctx, "%s:%s", table, key);
        RedisModuleCallReply *v = RedisModule_Call(ctx, "HGET", "ss", rowKey, col);
        int keep = 0;
        if (v && RedisModule_CallReplyType(v) == REDISMODULE_REPLY_STRING) {
            RedisModuleString *cur = RedisModule_CreateStringFromCallReply(v);
            size_t clen; const char *cstr = RedisModule_StringPtrLen(cur, &clen);
            keep = compare_values(cstr, vstr, op, type);
        }
        if (!keep) {
            // Resize array if needed (doubles capacity each time)
            if (removeCount >= toRemoveCapacity) {
                toRemoveCapacity *= 2;
                RedisModuleString **newToRemove = RedisModule_Realloc(toRemove, sizeof(RedisModuleString*) * toRemoveCapacity);
                if (newToRemove == NULL) {
                    // Out of memory during realloc - stop collecting, work with what we have
                    break;
                }
                toRemove = newToRemove;
            }
            toRemove[removeCount++] = key;
        }
    }
    RedisModule_DictIteratorStop(it);

    // Second pass: remove the collected keys
    for (size_t i = 0; i < removeCount; i++) {
        RedisModule_DictDel(dict, toRemove[i], NULL);
    }

    RedisModule_Free(toRemove);
    return 0; // Success
}

/* ================== TABLE.SELECT <namespace.table> [WHERE col op val (AND|OR col op val ...)] ================== */
static int TableSelectCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");

    int wherePos = -1;
    for (int i = 2; i < argc; i++) {
        size_t l; const char *w = RedisModule_StringPtrLen(argv[i], &l);
        if (l == 5 && strncasecmp(w, "WHERE", 5) == 0) { wherePos = i; break; }
    }

    RedisModuleDict *ids = RedisModule_CreateDict(ctx);
    if (wherePos == -1) {
        dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
    } else {
        int i = wherePos + 1;
        int haveSeed = 0;
        while (i < argc) {
            RedisModuleString *col=NULL, *val=NULL;
            char op[3];
            if (split_condition(ctx, argv[i], &col, op, &val) != REDISMODULE_OK)
                return RedisModule_ReplyWithError(ctx, "ERR condition must be <col><op><value>");
            
            // Check if column is indexed (only for = operator, others need full scan)
            if (strcmp(op, "=") != 0 || !is_column_indexed(ctx, argv[1], col)) {
                if (strcmp(op, "=") == 0) {
                    return RedisModule_ReplyWithError(ctx, "ERR search cannot be done on non-indexed column");
                }
                // For comparison operators, we need to scan all rows
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
                    haveSeed = 1;
                }
                if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                    return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                }
                i++;
            } else {
                // Indexed equality search
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                    haveSeed = 1; i++;
                } else {
                    size_t opl; const char *ops = RedisModule_StringPtrLen(argv[i-1], &opl);
                    if (opl==3 && strncasecmp(ops, "AND",3)==0) {
                        if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                            return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                        }
                        i++;
                    } else if (opl==2 && strncasecmp(ops, "OR",2)==0) {
                        dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                        i++;
                    } else {
                        return RedisModule_ReplyWithError(ctx, "ERR expected AND/OR between conditions");
                    }
                }
            }
            
            if (i < argc) {
                size_t tl; const char *ts = RedisModule_StringPtrLen(argv[i], &tl);
                if ((tl==3 && strncasecmp(ts, "AND",3)==0) || (tl==2 && strncasecmp(ts, "OR",2)==0)) {
                    i++;
                    if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR dangling operator");
                }
            }
        }
    }

    // Build reply
    RedisModuleDictIter *it = RedisModule_DictIteratorStartC(ids, "^", NULL, 0);
    RedisModuleString *id; void *dummy;
    size_t rowCount = 0;
    while (RedisModule_DictNext(ctx, it, &dummy)) rowCount++;
    RedisModule_DictIteratorStop(it);

    RedisModule_ReplyWithArray(ctx, rowCount);
    it = RedisModule_DictIteratorStartC(ids, "^", NULL, 0);
    while ((id = RedisModule_DictNext(ctx, it, &dummy)) != NULL) {
        RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], id);
        RedisModuleCallReply *all = RedisModule_Call(ctx, "HGETALL", "s", rowKey);
        if (!all || RedisModule_CallReplyType(all) != REDISMODULE_REPLY_ARRAY) {
            RedisModule_ReplyWithNull(ctx);
            continue;
        }
        size_t n = RedisModule_CallReplyLength(all);
        RedisModule_ReplyWithArray(ctx, n);
        for (size_t j = 0; j < n; j++) {
            RedisModuleCallReply *e = RedisModule_CallReplyArrayElement(all, j);
            RedisModule_ReplyWithString(ctx, RedisModule_CreateStringFromCallReply(e));
        }
    }
    RedisModule_DictIteratorStop(it);
    return REDISMODULE_OK;
}

// Update indices for a single column when value changes
static void update_index_for_change(RedisModuleCtx *ctx, RedisModuleString *table, RedisModuleString *col,
                                    RedisModuleString *oldv, RedisModuleString *newv, RedisModuleString *rowId) {
    if (!is_column_indexed(ctx, table, col)) return;
    if (oldv && RedisModule_StringCompare(oldv, newv) == 0) return;
    if (oldv) {
        RedisModuleString *oldIdx = fmt3(ctx, "idx:%s:%s:%s", table, col, oldv);
        RedisModule_Call(ctx, "SREM", "ss", oldIdx, rowId);
    }
    RedisModuleString *newIdx = fmt3(ctx, "idx:%s:%s:%s", table, col, newv);
    RedisModule_Call(ctx, "SADD", "ss", newIdx, rowId);
}

/* ================== TABLE.UPDATE <namespace.table> WHERE ... SET col=val ... ================== */
static int TableUpdateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 5) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");

    int setPos = -1;
    for (int i = 2; i < argc; i++) {
        size_t l; const char *w = RedisModule_StringPtrLen(argv[i], &l);
        if (l==3 && strncasecmp(w, "SET",3)==0) { setPos = i; break; }
    }
    if (setPos == -1) return RedisModule_ReplyWithError(ctx, "ERR missing SET");

    int whereStart = 2;
    if (setPos > 2) {
        size_t l; const char *w = RedisModule_StringPtrLen(argv[2], &l);
        if (l == 5 && strncasecmp(w, "WHERE", 5) == 0) whereStart = 3;
    }
    
    RedisModuleDict *ids = RedisModule_CreateDict(ctx);
    if (whereStart >= setPos) {
        dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
    } else {
        int i = whereStart;
        int haveSeed = 0;
        while (i < setPos) {
            RedisModuleString *col=NULL,*val=NULL;
            char op[3];
            if (split_condition(ctx, argv[i], &col, op, &val) != REDISMODULE_OK)
                return RedisModule_ReplyWithError(ctx, "ERR condition must be <col><op><value>");
            
            if (strcmp(op, "=") == 0 && is_column_indexed(ctx, argv[1], col)) {
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                    haveSeed = 1; i++;
                } else {
                    size_t opl; const char *ops = RedisModule_StringPtrLen(argv[i-1], &opl);
                    if (opl==3 && strncasecmp(ops, "AND",3)==0) {
                        if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                            return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                        }
                        i++;
                    } else if (opl==2 && strncasecmp(ops, "OR",2)==0) {
                        dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                        i++;
                    } else return RedisModule_ReplyWithError(ctx, "ERR expected AND/OR between conditions");
                }
            } else {
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
                    haveSeed = 1;
                }
                if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                    return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                }
                i++;
            }
            
            if (i < setPos) {
                size_t tl; const char *ts = RedisModule_StringPtrLen(argv[i], &tl);
                if ((tl==3 && strncasecmp(ts, "AND",3)==0) || (tl==2 && strncasecmp(ts, "OR",2)==0)) {
                    i++; if (i>=setPos) return RedisModule_ReplyWithError(ctx, "ERR dangling operator");
                }
            }
        }
    }

    long long updated = 0;
    RedisModuleDictIter *it = RedisModule_DictIteratorStartC(ids, "^", NULL, 0);
    RedisModuleString *id; void *dummy;
    while ((id = RedisModule_DictNext(ctx, it, &dummy)) != NULL) {
        RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], id);
        for (int j = setPos + 1; j < argc; j++) {
            RedisModuleString *col=NULL,*val=NULL;
            char op[3];
            if (split_condition(ctx, argv[j], &col, op, &val) != REDISMODULE_OK || strcmp(op, "=") != 0)
                return RedisModule_ReplyWithError(ctx, "ERR SET expects <col>=<value>");
            if (validate_and_typecheck(ctx, argv[1], col, val) != REDISMODULE_OK)
                return RedisModule_ReplyWithError(ctx, "ERR invalid column or type");
            
            RedisModuleCallReply *oldr = RedisModule_Call(ctx, "HGET", "ss", rowKey, col);
            RedisModuleString *oldv = NULL;
            if (oldr && RedisModule_CallReplyType(oldr) == REDISMODULE_REPLY_STRING)
                oldv = RedisModule_CreateStringFromCallReply(oldr);
            
            RedisModule_Call(ctx, "HSET", "sss", rowKey, col, val);
            update_index_for_change(ctx, argv[1], col, oldv, val, id);
        }
        updated++;
    }
    RedisModule_DictIteratorStop(it);
    return RedisModule_ReplyWithLongLong(ctx, updated);
}

/* ================== TABLE.DELETE <namespace.table> WHERE ... ================== */
static int TableDeleteCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");

    RedisModuleDict *ids = RedisModule_CreateDict(ctx);
    int hasWhere = 0;
    for (int i = 2; i < argc; i++) {
        size_t l; const char *w = RedisModule_StringPtrLen(argv[i], &l);
        if (l==5 && strncasecmp(w, "WHERE",5)==0) { hasWhere=1; break; }
    }

    if (!hasWhere) {
        dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
    } else {
        int i = 3;
        int haveSeed = 0;
        while (i < argc) {
            RedisModuleString *col=NULL,*val=NULL;
            char op[3];
            if (split_condition(ctx, argv[i], &col, op, &val) != REDISMODULE_OK)
                return RedisModule_ReplyWithError(ctx, "ERR condition must be <col><op><value>");
            
            if (strcmp(op, "=") == 0 && is_column_indexed(ctx, argv[1], col)) {
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                    haveSeed=1; i++;
                } else {
                    size_t opl; const char *ops = RedisModule_StringPtrLen(argv[i-1], &opl);
                    if (opl==3 && strncasecmp(ops, "AND",3)==0) {
                        if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                            return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                        }
                        i++;
                    } else if (opl==2 && strncasecmp(ops, "OR",2)==0) {
                        dict_add_set_members(ctx, ids, fmt3(ctx, "idx:%s:%s:%s", argv[1], col, val));
                        i++;
                    } else return RedisModule_ReplyWithError(ctx, "ERR expected AND/OR between conditions");
                }
            } else {
                if (!haveSeed) {
                    dict_add_set_members(ctx, ids, fmt(ctx, "rows:%s", argv[1]));
                    haveSeed = 1;
                }
                if (dict_filter_condition(ctx, ids, argv[1], col, op, val) != 0) {
                    return RedisModule_ReplyWithError(ctx, "ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.");
                }
                i++;
            }
            
            if (i < argc) {
                size_t tl; const char *ts = RedisModule_StringPtrLen(argv[i], &tl);
                if ((tl==3 && strncasecmp(ts, "AND",3)==0) || (tl==2 && strncasecmp(ts, "OR",2)==0)) {
                    i++; if (i>=argc) return RedisModule_ReplyWithError(ctx, "ERR dangling operator");
                }
            }
        }
    }

    long long deleted = 0;
    RedisModuleString *rowsSet = fmt(ctx, "rows:%s", argv[1]);

    RedisModuleDictIter *it = RedisModule_DictIteratorStartC(ids, "^", NULL, 0);
    RedisModuleString *id; void *dummy;
    while ((id = RedisModule_DictNext(ctx, it, &dummy)) != NULL) {
        RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], id);

        RedisModuleCallReply *fields = RedisModule_Call(ctx, "HKEYS", "s", fmt(ctx, "schema:%s", argv[1]));
        if (fields && RedisModule_CallReplyType(fields) == REDISMODULE_REPLY_ARRAY) {
            size_t n = RedisModule_CallReplyLength(fields);
            for (size_t i = 0; i < n; i++) {
                RedisModuleCallReply *fc = RedisModule_CallReplyArrayElement(fields, i);
                RedisModuleString *col = RedisModule_CreateStringFromCallReply(fc);
                
                if (is_column_indexed(ctx, argv[1], col)) {
                    RedisModuleCallReply *oldr = RedisModule_Call(ctx, "HGET", "ss", rowKey, col);
                    if (oldr && RedisModule_CallReplyType(oldr) == REDISMODULE_REPLY_STRING) {
                        RedisModuleString *oldv = RedisModule_CreateStringFromCallReply(oldr);
                        RedisModuleString *idxKey = fmt3(ctx, "idx:%s:%s:%s", argv[1], col, oldv);
                        RedisModule_Call(ctx, "SREM", "ss", idxKey, id);
                    }
                }
            }
        }

        RedisModule_Call(ctx, "DEL", "s", rowKey);
        RedisModule_Call(ctx, "SREM", "ss", rowsSet, id);
        deleted++;
    }
    RedisModule_DictIteratorStop(it);
    return RedisModule_ReplyWithLongLong(ctx, deleted);
}

/* ================== TABLE.DROP <namespace.table> [FORCE] ================== */
static int TableDropCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);
    RedisModule_AutoMemory(ctx);
    if (ensure_table_exists(ctx, argv[1]) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR table schema does not exist");
    
    // Check for FORCE parameter
    if (argc == 2) {
        return RedisModule_ReplyWithError(ctx, "ERR This operation is irreversible, use FORCE parameter to remove the table");
    }
    
    // Verify FORCE parameter
    const char *force_param = RedisModule_StringPtrLen(argv[2], NULL);
    if (strcasecmp(force_param, "FORCE") != 0) {
        return RedisModule_ReplyWithError(ctx, "ERR Invalid parameter. Use FORCE to confirm table removal");
    }

    RedisModuleString *rowsSet = fmt(ctx, "rows:%s", argv[1]);
    RedisModuleCallReply *rows = RedisModule_Call(ctx, "SMEMBERS", "s", rowsSet);
    if (rows && RedisModule_CallReplyType(rows) == REDISMODULE_REPLY_ARRAY) {
        size_t n = RedisModule_CallReplyLength(rows);
        for (size_t i = 0; i < n; i++) {
            RedisModuleCallReply *e = RedisModule_CallReplyArrayElement(rows, i);
            RedisModuleString *id = RedisModule_CreateStringFromCallReply(e);
            RedisModuleString *rowKey = fmt2(ctx, "%s:%s", argv[1], id);

            RedisModuleCallReply *fields = RedisModule_Call(ctx, "HKEYS", "s", fmt(ctx, "schema:%s", argv[1]));
            if (fields && RedisModule_CallReplyType(fields) == REDISMODULE_REPLY_ARRAY) {
                size_t fn = RedisModule_CallReplyLength(fields);
                for (size_t j = 0; j < fn; j++) {
                    RedisModuleCallReply *fc = RedisModule_CallReplyArrayElement(fields, j);
                    RedisModuleString *col = RedisModule_CreateStringFromCallReply(fc);
                    
                    if (is_column_indexed(ctx, argv[1], col)) {
                        RedisModuleCallReply *oldr = RedisModule_Call(ctx, "HGET", "ss", rowKey, col);
                        if (oldr && RedisModule_CallReplyType(oldr) == REDISMODULE_REPLY_STRING) {
                            RedisModuleString *oldv = RedisModule_CreateStringFromCallReply(oldr);
                            RedisModuleString *idxKey = fmt3(ctx, "idx:%s:%s:%s", argv[1], col, oldv);
                            RedisModule_Call(ctx, "SREM", "ss", idxKey, id);
                        }
                    }
                }
            }

            RedisModule_Call(ctx, "DEL", "s", rowKey);
            RedisModule_Call(ctx, "SREM", "ss", rowsSet, id);
        }
    }

    RedisModule_Call(ctx, "DEL", "s", fmt(ctx, "schema:%s", argv[1]));
    RedisModule_Call(ctx, "DEL", "s", fmt(ctx, "table:%s:id", argv[1]));
    RedisModule_Call(ctx, "DEL", "s", fmt(ctx, "idx:meta:%s", argv[1]));
    RedisModule_Call(ctx, "DEL", "s", rowsSet);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* ================== TABLE.HELP ================== */
static int TableHelpCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    (void)argv; (void)argc;
    const char *help[] = {
        "TABLE.NAMESPACE.CREATE <namespace>",
        "TABLE.NAMESPACE.VIEW [<namespace>] - Display all namespace:table pairs, optionally filtered by namespace",
        "TABLE.SCHEMA.VIEW <namespace.table> - Display columns, types, and index status",
        "TABLE.SCHEMA.CREATE <namespace.table> <col:type[:index]> [<col:type[:index]> ...]",
        "  Types: string, integer, float, date (YYYY-MM-DD)",
        "TABLE.SCHEMA.ALTER <namespace.table> ADD COLUMN <col:type[:index]> | ADD INDEX <col> | DROP INDEX <col>",
        "  ADD INDEX builds index for existing data",
        "TABLE.INSERT <namespace.table> <col>=<value> [<col>=<value> ...]",
        "TABLE.SELECT <namespace.table> [WHERE <col><op><value> (AND|OR <col><op><value> ...)]",
        "  Operators: = > < >= <=",
        "  Note: Only indexed columns can use = in WHERE",
        "TABLE.UPDATE <namespace.table> WHERE <cond> (AND|OR <cond> ...) SET <col>=<value> [<col>=<value> ...]",
        "TABLE.DELETE <namespace.table> [WHERE <cond> (AND|OR <cond> ...)]",
        "TABLE.DROP <namespace.table> FORCE",
        "  FORCE parameter is required to confirm irreversible deletion",
        "TABLE.HELP"
    };
    size_t n = sizeof(help)/sizeof(help[0]);
    RedisModule_ReplyWithArray(ctx, n);
    for (size_t i = 0; i < n; i++) RedisModule_ReplyWithSimpleString(ctx, help[i]);
    return REDISMODULE_OK;
}

/* ================== Module Init ================== */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "table", 2, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    // Parse module load-time arguments
    // Usage: --loadmodule redis_table.so max_scan_limit <value>
    // Example: --loadmodule redis_table.so max_scan_limit 200000
    if (argc >= 2) {
        size_t keyLen;
        const char *key = RedisModule_StringPtrLen(argv[0], &keyLen);
        
        if (strncmp(key, "max_scan_limit", keyLen) == 0 || strncmp(key, "max_scan_limit", 14) == 0) {
            long long value;
            if (RedisModule_StringToLongLong(argv[1], &value) == REDISMODULE_OK) {
                if (value >= 1000 && value <= 10000000) {  // Min 1K, Max 10M
                    g_max_rows_scan_limit = value;
                    RedisModule_Log(ctx, "notice", "Table module: max_scan_limit set to %lld", value);
                } else {
                    RedisModule_Log(ctx, "warning", "Table module: invalid max_scan_limit value %lld (must be between 1000 and 10000000), using default %lld", value, (long long)DEFAULT_MAX_ROWS_SCAN_LIMIT);
                }
            }
        }
    }

    if (RedisModule_CreateCommand(ctx, "TABLE.NAMESPACE.CREATE", TableNamespaceCreateCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.NAMESPACE.VIEW", TableNamespaceViewCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.SCHEMA.VIEW", TableSchemaViewCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.SCHEMA.CREATE", TableSchemaCreateCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.SCHEMA.ALTER", TableSchemaAlterCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.INSERT", TableInsertCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.SELECT", TableSelectCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.UPDATE", TableUpdateCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.DELETE", TableDeleteCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.DROP", TableDropCommand, "write", 1, 1, 1) == REDISMODULE_ERR) return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "TABLE.HELP", TableHelpCommand, "readonly", 0, 0, 0) == REDISMODULE_ERR) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
