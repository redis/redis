#include "server.h"
#include "crc64.h"
extern uint64_t MurmurHash64A (const void * key, int len, unsigned int seed);
static uint64_t crc64_hash(const sds key) {
    return crc64(0, (const unsigned char*)key, sdslen(key));
}

static uint64_t murmurHash(const sds key) {
    return MurmurHash64A(key, sdslen(key), 0xadc83b19ULL);
}

typedef struct {
    uint32_t magic;
    uint64_t m;
    uint32_t k;
} __attribute__((packed)) bfHeader;

static inline size_t bflen(uint64_t m) {
    return sizeof(bfHeader) + (m >> 3) + ((m & 0x7) > 0);
}

void bfcreateCommand(client *c){
    robj *o;
    long m,k;
    size_t byte;
    bfHeader* header;

    if (C_OK != getLongFromObjectOrReply(c, c->argv[2],&m,
                    "filter bits is not an integer or out of range"))
        return;
    if (m <= 0) {
        addReplyError(c,"filter bits is not an positive integer");
        return;
    }

    if (C_OK != getLongFromObjectOrReply(c, c->argv[3],&k,
                    "hash times is not an integer or out of range"))
        return;
    if (k <= 0) {
        addReplyError(c,"hash times is not an positive integer");
        return;
    }

    byte = bflen(m);
    if (byte > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return;
    }

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        o = createObject(OBJ_STRING,sdsnewlen(NULL,byte));
        dbAdd(c->db,c->argv[1],o);
    } else {
        addReplyError(c,"filter object is already exist");
        return;
    }

    header = (bfHeader*) o->ptr;
    header->magic=0xDEADBEEF;
    header->m=(uint64_t)m;
    header->k=(uint32_t)k;

    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"bfcreate",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.ok);
}

void bfaddCommand(client *c) {
    robj *o;
    char *err = "invalid filter format", *buf;
    size_t len, byte;
    int i, updated = 0;;
    uint32_t j;
    bfHeader *header;

    if (c->argc > 2002) {
        addReplyError(c,"too many arguments");
        return;
    }

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL
         || (checkType(c,o,OBJ_STRING))) return;
    if (o->encoding != OBJ_ENCODING_RAW) {
        addReplyError(c,err);
        return;
    }

    len = sdslen(o->ptr);
    if (len < sizeof(bfHeader)) {
        addReplyError(c,err);
        return;
    }

    header = (bfHeader*)o->ptr;
    byte = bflen(header->m);
    buf = (char*)(header + 1);
    if ((len != byte) || (header->magic != 0xDEADBEEF)) {
        addReplyError(c,err);
        return;
    }

    for (i = 2; i < c->argc; i++) {
        uint64_t hash1 = crc64_hash(c->argv[i]->ptr),
            hash2 = murmurHash(c->argv[i]->ptr);
        for (j = 0; j < header->k; j++) {
            uint64_t bit = (hash1 + j * hash2) % header->m,
                     byteoffset = bit >> 3,
                     bitoffset = 7 - (bit & 0x7);
            updated |= !(buf[byteoffset] & (1 << bitoffset));
            buf[byteoffset] |= (1 << bitoffset);
        }
    }

    if (updated) {
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"bfadd",c->argv[1],c->db->id);
        server.dirty++;
    }
    addReply(c,shared.ok);
}

void bfmatchCommand(client *c) {
    robj *o;
    char *err = "invalid filter format", *buf;
    size_t len, byte;
    bfHeader *header;
    int i;
    uint32_t j;

    if (c->argc > 2002) {
        addReplyError(c,"too many arguments");
        return;
    }

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nokeyerr)) == NULL
        || (checkType(c,o,OBJ_STRING))) return;
    if (o->encoding != OBJ_ENCODING_RAW) {
        addReplyError(c,err);
        return;
    }

    len = sdslen(o->ptr);
    if (len < sizeof(bfHeader)) {
        addReplyError(c,err);
        return;
    }

    header = (bfHeader*)o->ptr;
    byte = bflen(header->m);
    buf = (char*)(header + 1);
    if ((len != byte) || (header->magic != 0xDEADBEEF)) {
        addReplyError(c,err);
        return;
    }

    addReplyMultiBulkLen(c,c->argc-2);
    for (i = 2; i < c->argc; i++) {
        uint64_t hash1 = crc64_hash(c->argv[i]->ptr),
            hash2 = murmurHash(c->argv[i]->ptr);
        for (j = 0; j < header->k; j++) {
            uint64_t bit = (hash1 + j * hash2) % header->m,
                     byteoffset = bit >> 3,
                     bitoffset = 7 - (bit & 0x7);
            if (!(buf[byteoffset] & (1 << bitoffset)))
                break;
        }

        if (j < header->k) {
            addReply(c,shared.czero);
        } else {
            addReply(c,shared.cone);
        }
    }
}
