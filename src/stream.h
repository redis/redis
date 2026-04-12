#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"
#include "dict.h"
#include "xxhash.h"
#include "flax.h"
#include <stdint.h>

/* Two-level PEL key-length convention:
 *
 * Single-entry ("direct") buckets use a 16-byte rax key (the complete
 * big-endian streamID) and store the data pointer directly as the rax
 * value.  Multi-entry ("flax") buckets use a 15-byte rax key (ms + upper
 * 7 bytes of seq) and store a flax* mapping the low byte of seq to data
 * pointers.  The key length in rax (16 vs 15) distinguishes the two. */
#define PEL_RAX_DIRECT_KEYLEN  sizeof(streamID)   /* 16 */
#define PEL_RAX_FLAX_KEYLEN    15

/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
typedef struct streamID {
    uint64_t ms;        /* Unix time in milliseconds. */
    uint64_t seq;       /* Sequence number. */
} streamID;

/* Structure to hold IID and stream ID for IDMP deduplication */
typedef struct idmpEntry {
    struct idmpEntry *next;  /* Pointer to next entry in insertion order (linked list) */
    streamID id;             /* Associated stream ID */
    size_t iid_len;          /* Length of the IID */
    char iid[];              /* Flexible array member for inline IID storage */
} idmpEntry;

/* IDMP Producer structure for per-producer deduplication tracking */
typedef struct idmpProducer {
    dict *idmp_dict;       /* IDMP IID tracking tree. */
    idmpEntry *idmp_head;  /* Head of the IDMP entries linked list. */
    idmpEntry *idmp_tail;  /* Tail of the IDMP entries linked list. */
} idmpProducer;

/* Dictionary type for IDMP entries - uses IID as key */
extern dictType idmpDictType;

typedef struct stream {
    rax *rax;               /* The radix tree holding the stream. */
    uint64_t length;        /* Current number of elements inside this stream. */
    streamID last_id;       /* Zero if there are yet no items. */
    streamID first_id;      /* The first non-tombstone entry, zero if empty. */
    streamID max_deleted_entry_id;  /* The maximal ID that was deleted. */
    uint64_t entries_added; /* All time count of elements added. */
    size_t alloc_size;      /* Total allocated memory (in bytes) by this stream. */
    rax *cgroups;           /* Consumer groups dictionary: name -> streamCG */
    rax *cgroups_ref;       /* Two-level index mapping message IDs to their
                               consumer groups.  Same key scheme as PEL:
                               outer rax -> flax(low byte of seq -> list*
                               of streamCG pointers).  Direct / flax key
                               lengths follow the PEL convention (16/15). */
    streamID min_cgroup_last_id;  /* The minimum ID of consume group. */
    unsigned int min_cgroup_last_id_valid: 1;
    uint64_t idmp_duration; /* IDMP duration in seconds. */
    uint64_t idmp_max_entries; /* Max number of IID for tracking. */
    rax *idmp_producers;   /* IDMP producers radix tree: pid -> idmpProducer */
    uint64_t iids_added;   /* All time count of entries with IID added. */
    uint64_t iids_duplicates; /* All time count of duplicate IIDs detected. */
} stream;

/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
typedef struct streamIterator {
    stream *stream;         /* The stream we are iterating. */
    streamID master_id;     /* ID of the master entry at listpack head. */
    uint64_t master_fields_count;       /* Master entries # of fields. */
    unsigned char *master_fields_start; /* Master entries start in listpack. */
    unsigned char *master_fields_ptr;   /* Master field to emit next. */
    int entry_flags;                    /* Flags of entry we are emitting. */
    int rev;                /* True if iterating end to start (reverse). */
    int skip_tombstones;    /* True if not emitting tombstone entries. */
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */
    uint64_t end_key[2];    /* End key as 128 bit big endian. */
    /* Decoded native-endian fields for fast numeric comparison */
    uint64_t start_ms;
    uint64_t start_seq;
    uint64_t end_ms;
    uint64_t end_seq;
    raxIterator ri;         /* Rax iterator. */
    unsigned char *lp;      /* Current listpack. */
    unsigned char *lp_last_ele; /* Previous listpack element position for corruption detection. */
    unsigned char *lp_ele;  /* Current listpack cursor. */
    unsigned char *lp_flags; /* Current entry flags pointer. */
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    unsigned char field_buf[LP_INTBUF_SIZE];
    unsigned char value_buf[LP_INTBUF_SIZE];
} streamIterator;

/* Forward declarations */
typedef struct streamNACK streamNACK;

/* Consumer group. */
typedef struct streamCG {
    streamID last_id;       /* Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this. */
    long long entries_read; /* In a perfect world (CG starts at 0-0, no dels, no
                               XGROUP SETID, ...), this is the total number of
                               group reads. In the real world, the reasoning behind
                               this value is detailed at the top comment of
                               streamEstimateDistanceFromFirstEverEntry(). */
    rax *pel;               /* Two-level pending entries list.  Single-entry
                               buckets (direct) use a 16-byte rax key (full
                               big-endian streamID).  Multi-entry buckets use
                               a 15-byte key (ms + upper 7 bytes of seq) and
                               store a flax* mapping the low byte of seq to
                               streamNACK pointers.  Max 256 per bucket. */
    uint64_t pel_count;     /* Total number of NACK entries across all flax buckets. */
    streamNACK *pel_time_head; /* Head of time-ordered doubly-linked list of pending
                                  entries (oldest delivery_time). Used for efficient
                                  CLAIM operations. O(1) access to oldest entries. */
    streamNACK *pel_time_tail; /* Tail of time-ordered doubly-linked list of pending
                                  entries (newest delivery_time). O(1) append for
                                  updates that set delivery_time to current time. */
    streamNACK *pel_nack_tail; /* Tail of the NACK zone at the head of the
                                  PEL time-ordered list. NACKed entries occupy
                                  positions from pel_time_head to pel_nack_tail.
                                  NULL if no NACKed entries exist. */
    rax *consumers;         /* A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures. */
} streamCG;

/* A specific consumer in a consumer group.  */
typedef struct streamConsumer {
    mstime_t seen_time;         /* Last time this consumer tried to perform an action (attempted reading/claiming). */
    mstime_t active_time;       /* Last time this consumer was active (successful reading/claiming). */
    sds name;                   /* Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive. */
    rax *pel;                   /* Two-level consumer PEL: same structure as
                                   streamCG.pel — 16-byte key for direct,
                                   15-byte key for flax buckets.  NACKs are
                                   shared with group PEL. */
    uint64_t pel_count;         /* Total NACK count for this consumer. */
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
struct streamNACK {
    mstime_t delivery_time;     /* Last time this message was delivered. */
    uint64_t delivery_count;    /* Number of times this message was delivered.*/
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery. */
    listNode *cgroup_ref_node; /* Reference to this NACK in the cgroups_ref list. */
    streamID id;                /* Stream ID for this pending entry. */
    struct streamNACK *pel_prev; /* Previous NACK in time-ordered doubly-linked list. */
    struct streamNACK *pel_next; /* Next NACK in time-ordered doubly-linked list. */
};

/* Stream propagation information, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct streamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

/* Flags for streamCreateConsumer */
#define SCC_DEFAULT       0
#define SCC_NO_NOTIFY     (1<<0) /* Do not notify key space if consumer created */
#define SCC_NO_DIRTIFY    (1<<1) /* Do not dirty++ if consumer created */

#define SCG_INVALID_ENTRIES_READ -1

stream *streamNew(void);
void freeStream(stream *s);
unsigned long streamLength(const robj *subject);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, long long min_idle_time, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi, unsigned long *propCount);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorRemoveEntry(streamIterator *si, streamID *current);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name);
streamConsumer *streamCreateConsumer(stream *s, streamCG *cg, sds name, robj *key, int dbid, int flags);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id, long long entries_read);
streamNACK *streamCreateNACK(stream *s, streamConsumer *consumer, streamID *id);
void streamEncodeID(void *buf, streamID *id);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamFreeNACK(stream *s, streamNACK *na);
void streamDestroyNACK(stream *s, streamNACK *na);
int streamIncrID(streamID *id);
int streamDecrID(streamID *id);
void streamPropagateConsumerCreation(client *c, robj *key, robj *groupname, sds consumername);
robj *streamDup(robj *o);
int streamValidateListpackIntegrity(unsigned char *lp, size_t size, int deep);
int streamParseID(const robj *o, streamID *id);
robj *createObjectFromStreamID(streamID *id);
int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id, int seq_given);
int streamDeleteItem(stream *s, streamID *id);
void streamGetEdgeID(stream *s, int first, int skip_tombstones, streamID *edge_id);
long long streamEstimateDistanceFromFirstEverEntry(stream *s, streamID *id);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
int64_t streamTrimByID(stream *s, streamID minid, int approx);
int streamEntryExists(stream *s, streamID *id);
void streamKeyLoaded(redisDb *db, robj *key, robj *val);
void streamKeyRemoved(redisDb *db, robj *key, robj *val);

listNode *streamLinkCGroupToEntry(stream *s, streamCG *cg, streamID *id);

/* Two-level PEL iterator: walks outer rax and inner flax.
 * Direct entries (16-byte rax key) have no flax allocation;
 * is_direct tracks whether the current entry is direct. */
typedef struct pelIterator {
    raxIterator ri;
    flaxIterator fi;
    int valid;
    int just_seeked;
    int is_direct;
    streamID id;
    void *data;
    unsigned char key[sizeof(streamID)];
} pelIterator;

/* Two-level PEL operations. */
void pelCacheInvalidate(rax *pel);
rax *pelNew(size_t *alloc_size);
void pelFree(rax *pel, void (*nack_free)(void *, void *), void *ctx);
void pelFreeShallow(rax *pel);
int pelInsert(rax *pel, streamID *id, void *data, uint64_t *count);
int pelTryInsert(rax *pel, streamID *id, void *data, uint64_t *count);
void pelReplace(rax *pel, streamID *id, void *data);
int pelFind(rax *pel, streamID *id, void **data);
void *pelRemove(rax *pel, streamID *id, uint64_t *count);

void pelIterStart(pelIterator *pi, rax *pel);
int pelIterSeek(pelIterator *pi, const char *op, streamID *id);
int pelIterNext(pelIterator *pi);
void pelIterStop(pelIterator *pi);

/* PEL time list management (used by RDB loading) */
void pelListInsertSorted(streamCG *cg, streamNACK *nack);
void pelListUnlink(streamCG *cg, streamNACK *nack);
void pelListInsertNacked(streamCG *cg, streamNACK *nack);
uint64_t pelListNackedCount(streamCG *cg);

/* IDMP functions */
idmpEntry *idmpEntryCreate(const char *iid, size_t iid_len, size_t *alloc_size);
void idmpEntryFree(idmpEntry *entry, size_t *alloc_size);
idmpProducer *idmpProducerCreate(size_t *alloc_size);
void idmpProducerFree(idmpProducer *producer, size_t *alloc_size);
void streamFreeIdmpProducerGeneric(void *producer, void *strm);

#endif
