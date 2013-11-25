#include "server.h"
#include "murmur3.h"

unsigned int murmurHash(char* key) {
    unsigned int hash = 0;
    MurmurHash3_x86_32(key,sdslen(key),0,&hash);
    return hash; 
}

unsigned int SuperFastHash(const char* data, int len);

unsigned int superfastHash(char* key) {
    return SuperFastHash(key,sdslen(key));
}

typedef struct {
    unsigned int magic;
    long m;
    int k;
} __attribute__((packed)) bfHeader;

void bfcreateCommand(client *c){
    robj *o;
    long m,k;
    int byte;
    bfHeader* header;

    if ((C_OK != getLongFromObjectOrReply(c, c->argv[2],&m,
                    "filter bit is not an positive integer or out of rage")) || (m < 0))
        return;

    if ((C_OK != getLongFromObjectOrReply(c, c->argv[3],&k,
                    "hash times is not an positive integer or out of rage")) || (k < 0))
        return;

    byte = (m >> 3) + sizeof(bfHeader) + 1;
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
    header->m=m;
    header->k=(int)k;

    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

void bfaddCommand(client *c) {
    robj *o;
    char *err = "invalid filter format", *buf;
    size_t len, byte;
    int i, j;
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
    byte = (header->m >> 3) + sizeof(bfHeader) + 1;
    buf = (char*)(header + 1);
    if ((len != byte) || (header->magic != 0xDEADBEEF) 
            || (header->m < 0) || (header->k < 0)) {
        addReplyError(c,err);
        return;
    }


    for (i = 2; i < c->argc; i++) {
        unsigned int hash1 = superfastHash(c->argv[i]->ptr),
            hash2 = murmurHash(c->argv[i]->ptr);
        for (j = 0; j < header->k; j++) {
            long bit = (hash1 + j * hash2) % header->m;
            long byteoffset = bit >> 3;
            long bitoffset = 7 - (bit & 0x7);
            buf[byteoffset] |= (1 << bitoffset);             
        }
    }

    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
} 

void bfmatchCommand(client *c) {
    robj *o;
    char *err = "invalid filter format", *buf;
    size_t len, byte;
    bfHeader *header;
    int i,j;

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
    byte = (header->m >> 3) + sizeof(bfHeader) + 1;
    buf = (char*)(header + 1);
    if ((len != byte) || (header->magic != 0xDEADBEEF) 
            || (header->m < 0) || (header->k < 0)) {
        addReplyError(c,err);
        return;
    }

    addReplyMultiBulkLen(c,c->argc-2);
    for (i = 2; i < c->argc; i++) {
        unsigned int hash1 = superfastHash(c->argv[i]->ptr),
            hash2 = murmurHash(c->argv[i]->ptr);
        for (j = 0; j < header->k; j++) {
            long bit = (hash1 + j * hash2) % header->m;
            long byteoffset = bit >> 3;
            long bitoffset = 7 - (bit & 0x7);
            if (!(buf[byteoffset] & (1 << bitoffset))) break;
        }

        if (j < header->k) {
            addReply(c,shared.czero);
        } else {
            addReply(c,shared.cone);
        }
    }
}
