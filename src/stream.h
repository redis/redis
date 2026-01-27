#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"
#include "dict.h"
#include "xxhash.h"

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
    rax *cgroups_ref;       /* Index mapping message IDs to their consumer groups. */
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
    rax *pel;               /* Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.*/
    rax *pel_by_time;       /* A radix tree mapping delivery time to pending
                               entries, so that we can query faster PEL entries
                               by time. The key is a pelTimeKey structure containing
                               both delivery_time and stream ID. All information is
                               in the key; no value is stored. */
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
    rax *pel;                   /* Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the consumer group structure
                                   itself, so the value is shared. */
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
typedef struct streamNACK {
    mstime_t delivery_time;     /* Last time this message was delivered. */
    uint64_t delivery_count;    /* Number of times this message was delivered.*/
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery. */
    listNode *cgroup_ref_node; /* Reference to this NACK in the cgroups_ref list. */
} streamNACK;

/* Stream propagation information, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct streamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Pending entry in the consumer group's PEL, indexed by delivery time. */
typedef struct pelTimeKey {
    uint64_t delivery_time;
    streamID id;
} pelTimeKey;

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
streamNACK *streamCreateNACK(stream *s, streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamFreeNACK(stream *s, streamNACK *na);
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

listNode *streamLinkCGroupToEntry(stream *s, streamCG *cg, unsigned char *key);

void encodePelTimeKey(void* buf, pelTimeKey *timeKey);
void decodePelTimeKey(void *buf, pelTimeKey *timeKey);
void raxInsertPelByTime(rax *pel_by_time, uint64_t delivery_time, streamID *id);
void raxRemovePelByTime(rax *pel_by_time, uint64_t delivery_time, streamID *id);

/* IDMP functions */
idmpEntry *idmpEntryCreate(const char *iid, size_t iid_len, size_t *alloc_size);
void idmpEntryFree(idmpEntry *entry, size_t *alloc_size);
idmpProducer *idmpProducerCreate(size_t *alloc_size);
void idmpProducerFree(idmpProducer *producer, size_t *alloc_size);
void streamFreeIdmpProducerGeneric(void *producer, void *strm);

#endif
