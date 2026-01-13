/*
 * Redis objects overview
 *
 * robj (struct redisObject) is the fundamental in-memory container that can hold
 * values of different logical types (strings, lists, sets, hashes, sorted sets,
 * streams, modules, ...). It contains:
 *   - type: one of OBJ_STRING, OBJ_LIST, OBJ_SET, OBJ_ZSET, OBJ_HASH, OBJ_STREAM,
 *           OBJ_MODULE, ...
 *   - encoding: an implementation detail of how the value is represented in
 *           memory for the given type (see OBJ_ENCODING_* below). For example,
 *           strings may be RAW/EMBSTR/INT, sets may be INTSET or HT, etc.
 *   - lru: either LRU information (relative to the global LRU clock) or LFU data
 *           when LFU is enabled.
 *   - refcount: reference counting for object lifetime management.
 *   - ptr: a pointer to the underlying value payload (SDS, dict, quicklist, ...).
 *
 * Object encodings
 * -----------------
 * Some kinds of objects (strings, hashes, sets, zsets, lists) have multiple
 * possible in-memory encodings optimized for memory footprint and speed. The
 * 'encoding' field indicates which representation is currently used (see
 * OBJ_ENCODING_* defines below).
 *
 * kvobj (key-value object)
 * ------------------------
 * kvobj is a specific use of robj that additionally embeds the key (and optional
 * metadata) alongside the value. It is identified by the iskvobj flag in robj.
 * The distinction is mostly declarative for clarity and may be enforced later with
 * explicit casting in places. Conceptually, robj and kvobj are in a relation 
 * similar to that of a parent-child class hierarchy.
 * 
 * When iskvobj is set, it also contains:
 *   - metabits: bitmap of additional metadata attached to the object.
 *   - lru: LRU time (relative to global lru_clock) or LFU data (see robj above).
 *   - embedded key: the key string is stored inline after the struct.
 *   - embedded value: for small strings, the value is stored inline after the key.
 *
 * Example layout with key and embedded value "myvalue":
 *    +--------------+--------------+--------------------+----------------------+
 *    | serverObject | key-hdr-size | sdshdr5 "mykey" \0 | sdshdr8 "myvalue" \0 |
 *    | 16 bytes     | 1 byte       | 1      +   5   + 1 | 3    +      7    + 1 |
 *    +--------------+--------------+--------------------+----------------------+
 * 
 * kvobj with metadata (+expiration)
 * ---------------------------------
 * Up to 8 metadata classes are supported, each storing one 8-byte field.
 * Class 0 is reserved for expiration time. Metadata blocks are placed before
 * the kvobj itself, in reverse class order.
 * 
 * Example of a key with expiration time (metabits=0b00000001):
 *     +--------------+--------------+--------------+--------------------+
 *     | Expiry Time  | serverObject | key-hdr-size | sdshdr5 "mykey" \0 |
 *     | 8 byte       | 16 bytes     | 1 byte       | 1      +   5   + 1 |
 *     +--------------+--------------+--------------+--------------------+
 *                    ^
 *                    +---- kvobjCreate() returns pointer here
 * 
 * Example with metadata of class1 and class3 attached (metabits=0b00001010):
 * +--------------+--------------+--------------+--------------+--------------------+
 * | meta (class3)| meta (class1)| serverObject | key-hdr-size | sdshdr5 "mykey" \0 |
 * | 8 byte       | 8 byte       | 16 bytes     | 1 byte       | 1      +   5   + 1 |
 * +--------------+--------------+--------------+--------------+--------------------+
 *                               ^
 *                               +---- kvobjCreate() returns pointer here
 * 
 */
#ifndef __OBJECT_H
#define __OBJECT_H

/* forward declarations */
struct client;
struct RedisModuleType;

/* Object encodings (see header comment below for details). */
#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3  /* No longer used: old hash encoding. */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* No longer used: old list/hash/zset encoding. */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of listpacks */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */
#define OBJ_ENCODING_LISTPACK 11 /* Encoded as a listpack */
#define OBJ_ENCODING_LISTPACK_EX 12 /* Encoded as listpack, extended with metadata */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */

#define OBJ_NUM_KVMETA_BITS 8
#define OBJ_REFCOUNT_BITS 23
#define OBJ_SHARED_REFCOUNT ((1 << OBJ_REFCOUNT_BITS) - 1) /* Global object never destroyed. */
#define OBJ_STATIC_REFCOUNT ((1 << OBJ_REFCOUNT_BITS) - 2) /* Object allocated in the stack. */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT

struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned refcount : OBJ_REFCOUNT_BITS;
    unsigned iskvobj : 1;   /* 1 if this struct serves as a kvobj base */
    
    /* metabits and lru are Relevant only when iskvobj is set: */     
    unsigned metabits :8;  /* Bitmap of metadata (+expiry) attached to this kvobj */
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    void *ptr;
};

/* robj - General purpose redis object */
typedef struct redisObject robj;

/* kvobj: see header comment above for definition and memory layout. */
typedef struct redisObject kvobj;

kvobj *kvobjCreate(int type, const sds key, void *ptr, uint32_t keyMetaBits);
kvobj *kvobjSet(sds key, robj *val, uint32_t keyMetaBits);
kvobj *kvobjSetExpire(kvobj *kv, long long expire);
sds kvobjGetKey(const kvobj *kv);
long long kvobjGetExpire(const kvobj *val);
uint64_t *kvobjMetaRef(kvobj *kv, int metaId);

/* Redis object implementation */
void decrRefCount(robj *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
void dismissObject(robj *o, size_t dump_size);
robj *createObject(int type, void *ptr);
void initObjectLRUOrLFU(robj *o);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *tryCreateRawStringObject(const char *ptr, size_t len);
robj *tryCreateStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *tryObjectEncodingEx(robj *o, int try_trim);
size_t getObjectLength(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
size_t stringObjectAllocSize(const robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongLongWithSds(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(int fill, int compress);
robj *createListListpackObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createSetListpackObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetListpackObject(void);
robj *createStreamObject(void);
robj *createModuleObject(struct RedisModuleType *mt, void *value);
int getLongFromObjectOrReply(struct client *c, robj *o, long *target, const char *msg);
int getPositiveLongFromObjectOrReply(struct client *c, robj *o, long *target, const char *msg);
int getRangeLongFromObjectOrReply(struct client *c, robj *o, long min, long max, long *target, const char *msg);
int checkType(struct client *c, robj *o, int type);
int getLongLongFromObjectOrReply(struct client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(struct client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(struct client *c, robj *o, long double *target, const char *msg);
int getIntFromObjectOrReply(struct client *c, robj *o, int *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(const robj *a, const robj *b);
int collateStringObjects(const robj *a, const robj *b);
int equalStringObjects(robj *a, robj *b);
void trimStringObjectIfNeeded(robj *o, int trim_small_values);
size_t kvobjAllocSize(kvobj *o);

int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                      long long lru_clock, int lru_multiplier);
void objectCommand(struct client *c);
void memoryCommand(struct client *c);

static inline void *kvobjGetAllocPtr(const kvobj *kv) {
    /* Return the base allocation pointer (start of the metadata prefix). */
    uint32_t numMetaBytes = __builtin_popcount(kv->metabits) * sizeof(uint64_t);
    return (char *)kv - numMetaBytes;
}

#endif /* __OBJECT_H */
