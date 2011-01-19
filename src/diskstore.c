/* diskstore.c implements a very simple disk backed key-value store used
 * by Redis for the "disk" backend. This implementation uses the filesystem
 * to store key/value pairs. Every file represents a given key.
 *
 * The key path is calculated using the SHA1 of the key name. For instance
 * the key "foo" is stored as a file name called:
 *
 *  /0b/ee/0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 * The couples of characters from the hex output of SHA1 are also used
 * to locate two two levels of directories to store the file (as most
 * filesystems are not able to handle too many files in a single dir).
 *
 * In the end there are 65536 final directories (256 directories inside
 * every 256 top level directories), so that with 1 billion of files every
 * directory will contain in the average 15258 entires, that is ok with
 * most filesystems implementation.
 *
 * Note that since Redis supports multiple databases, the actual key name
 * is:
 *
 *  /0b/ee/<dbid>_0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 *  so for instance if the key is inside DB 0:
 *
 *  /0b/ee/0_0beec7b5ea3f0fdbc95d0dd47f3c5bc275da8a33
 *
 * The actaul implementation of this disk store is highly dependant to the
 * filesystem implementation itself. This implementation may be replaced by
 * a B+TREE implementation in future implementations.
 *
 * Data ok every key is serialized using the same format used for .rdb
 * serialization. Everything is serialized on every entry: key name,
 * ttl information in case of keys with an associated expire time, and the
 * serialized value itself.
 *
 * Because the format is the same of the .rdb files it is trivial to create
 * an .rdb file starting from this format just by mean of scanning the
 * directories and concatenating entries, with the sole addition of an
 * .rdb header at the start and the end-of-db opcode at the end.
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2011, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "redis.h"
#include "sha1.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

int create256dir(char *prefix) {
    char buf[1024];
    int j;

    for (j = 0; j < 256; j++) {
        snprintf(buf,sizeof(buf),"%s%02x",prefix,j);
        if (mkdir(buf,0755) == -1) {
            redisLog(REDIS_WARNING,"Error creating dir %s for diskstore: %s",
                buf,strerror(errno));
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

int dsOpen(void) {
    struct stat sb;
    int retval, j;
    char *path = server.ds_path;
    char buf[1024];

    if ((retval = stat(path,&sb) == -1) && errno != ENOENT) {
        redisLog(REDIS_WARNING, "Error opening disk store at %s: %s",
                path, strerror(errno));
        return REDIS_ERR;
    }

    /* Directory already in place. Assume everything is ok. */
    if (retval == 0 && S_ISDIR(sb.st_mode)) {
        redisLog(REDIS_NOTICE,"Disk store %s exists", path);
        return REDIS_OK;
    }

    /* File exists but it's not a directory */
    if (retval == 0 && !S_ISDIR(sb.st_mode)) {
        redisLog(REDIS_WARNING,"Disk store at %s is not a directory", path);
        return REDIS_ERR;
    }

    /* New disk store, create the directory structure now, as creating
     * them in a lazy way is not a good idea, after very few insertions
     * we'll need most of the 65536 directories anyway. */
    redisLog(REDIS_NOTICE,"Disk store %s does not exist: creating", path);
    if (mkdir(path,0755) == -1) {
        redisLog(REDIS_WARNING,"Disk store init failed creating dir %s: %s",
            path, strerror(errno));
        return REDIS_ERR;
    }
    /* Create the top level 256 directories */
    snprintf(buf,sizeof(buf),"%s/",path);
    if (create256dir(buf) == REDIS_ERR) return REDIS_ERR;

    /* For every 256 top level dir, create 256 nested dirs */
    for (j = 0; j < 256; j++) {
        snprintf(buf,sizeof(buf),"%s/%02x/",path,j);
        if (create256dir(buf) == REDIS_ERR) return REDIS_ERR;
    }
    return REDIS_OK;
}

int dsClose(void) {
    return REDIS_OK;
}

/* Convert key into full path for this object. Dirty but hopefully
 * is fast enough. Returns the length of the returned path. */
int dsKeyToPath(redisDb *db, char *buf, robj *key) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char hex[40], digits[] = "0123456789abcdef";
    int j, l;
    char *origbuf = buf;

    SHA1Init(&ctx);
    SHA1Update(&ctx,key->ptr,sdslen(key->ptr));
    SHA1Final(hash,&ctx);

    /* Convert the hash into hex format */
    for (j = 0; j < 20; j++) {
        hex[j*2] = digits[(hash[j]&0xF0)>>4];
        hex[(j*2)+1] = digits[hash[j]&0x0F];
    }

    /* Create the object path. Start with server.ds_path that's the root dir */
    l = sdslen(server.ds_path);
    memcpy(buf,server.ds_path,l);
    buf += l;
    *buf++ = '/';

    /* Then add xx/yy/ that is the two level directories */
    buf[0] = hex[0];
    buf[1] = hex[1];
    buf[2] = '/';
    buf[3] = hex[2];
    buf[4] = hex[3];
    buf[5] = '/';
    buf += 6;

    /* Add the database number followed by _ and finall the SHA1 hex */
    l = ll2string(buf,64,db->id);
    buf += l;
    buf[0] = '_';
    memcpy(buf+1,hex,40);
    buf[41] = '\0';
    return (buf-origbuf)+41;
}

int dsSet(redisDb *db, robj *key, robj *val) {
    char buf[1024], buf2[1024];
    FILE *fp;
    int retval, len;

    len = dsKeyToPath(db,buf,key);
    memcpy(buf2,buf,len);
    snprintf(buf2+len,sizeof(buf2)-len,"-%ld-%ld",(long)time(NULL),(long)val);
    while ((fp = fopen(buf2,"w")) == NULL) {
        if (errno == ENOSPC) {
            redisLog(REDIS_WARNING,"Diskstore: No space left on device. Please make room and wait 30 seconds for Redis to continue.");
            sleep(30);
        } else {
            redisLog(REDIS_WARNING,"diskstore error opening %s: %s",
                buf2, strerror(errno));
            redisPanic("Unrecoverable diskstore error. Exiting.");
        }
    }
    if ((retval = rdbSaveKeyValuePair(fp,db,key,val,time(NULL))) == -1)
        return REDIS_ERR;
    fclose(fp);
    if (retval == 0) {
        /* Expired key. Unlink failing not critical */
        unlink(buf);
        unlink(buf2);
    } else {
        /* Use rename for atomic updadte of value */
        if (rename(buf2,buf) == -1) {
            redisLog(REDIS_WARNING,"rename(2) returned an error: %s",
                strerror(errno));
            redisPanic("Unrecoverable diskstore error. Exiting.");
        }
    }
    return REDIS_OK;
}

robj *dsGet(redisDb *db, robj *key, time_t *expire) {
    char buf[1024];
    int type;
    time_t expiretime = -1; /* -1 means: no expire */
    robj *dskey; /* Key as loaded from disk. */
    robj *val;
    FILE *fp;

    dsKeyToPath(db,buf,key);
    fp = fopen(buf,"r");
    if (fp == NULL && errno == ENOENT) return NULL; /* No such key */
    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Disk store failed opening %s: %s",
            buf, strerror(errno));
        goto readerr;
    }

    if ((type = rdbLoadType(fp)) == -1) goto readerr;
    if (type == REDIS_EXPIRETIME) {
        if ((expiretime = rdbLoadTime(fp)) == -1) goto readerr;
        /* We read the time so we need to read the object type again */
        if ((type = rdbLoadType(fp)) == -1) goto readerr;
    }
    /* Read key */
    if ((dskey = rdbLoadStringObject(fp)) == NULL) goto readerr;
    /* Read value */
    if ((val = rdbLoadObject(type,fp)) == NULL) goto readerr;
    fclose(fp);

    /* The key we asked, and the key returned, must be the same */
    redisAssert(equalStringObjects(key,dskey));

    /* Check if the key already expired */
    decrRefCount(dskey);
    if (expiretime != -1 && expiretime < time(NULL)) {
        decrRefCount(val);
        unlink(buf); /* This failing is non critical here */
        return NULL;
    }

    /* Everything ok... */
    *expire = expiretime;
    return val;

readerr:
    redisLog(REDIS_WARNING,"Read error reading reading %s. Corrupted key?",
        buf);
    redisPanic("Unrecoverable error reading from disk store");
    return NULL; /* unreached */
}

int dsDel(redisDb *db, robj *key) {
    char buf[1024];

    dsKeyToPath(db,buf,key);
    if (unlink(buf) == -1) {
        if (errno == ENOENT) {
            return REDIS_ERR;
        } else {
            redisLog(REDIS_WARNING,"Disk store can't remove %s: %s",
                buf, strerror(errno));
            redisPanic("Unrecoverable Disk store errore. Existing.");
            return REDIS_ERR; /* unreached */
        }
    } else {
        return REDIS_OK;
    }
}

int dsExists(redisDb *db, robj *key) {
    char buf[1024];

    dsKeyToPath(db,buf,key);
    return access(buf,R_OK) == 0;
}

int dsGetDbidFromFilename(char *path) {
    char id[64];
    char *p = strchr(path,'_');
    int len = (p - path);

    redisAssert(p != NULL && len < 64);
    memcpy(id,path,len);
    id[len] = '\0';
    return atoi(id);
}

void dsFlushOneDir(char *path, int dbid) {
    DIR *dir;
    struct dirent *dp, de;

    dir = opendir(path);
    if (dir == NULL) {
        redisLog(REDIS_WARNING,"Disk store can't open dir %s: %s",
            path, strerror(errno));
        redisPanic("Unrecoverable Disk store errore. Existing.");
    }
    while(1) {
        char buf[1024];

        readdir_r(dir,&de,&dp);
        if (dp == NULL) break;
        if (dp->d_name[0] == '.') continue;

        /* Check if we need to remove this entry accordingly to the
         * DB number. */
        if (dbid != -1 && dsGetDbidFromFilename(dp->d_name)) continue;
        
        /* Finally unlink the file */
        snprintf(buf,1024,"%s/%s",path,dp->d_name);
        if (unlink(buf) == -1) {
            redisLog(REDIS_WARNING,
                "Can't unlink %s: %s", buf, strerror(errno));
            redisPanic("Unrecoverable Disk store errore. Existing.");
        }
    }
    closedir(dir);
}

void dsFlushDb(int dbid) {
    char buf[1024];
    int j, i;

    redisLog(REDIS_NOTICE,"Flushing diskstore DB (%d)",dbid);
    for (j = 0; j < 256; j++) {
        for (i = 0; i < 256; i++) {
            snprintf(buf,1024,"%s/%02x/%02x",server.ds_path,j,i);
            dsFlushOneDir(buf,dbid);
        }
    }
}

void dsRdbSaveSetState(int state) {
    pthread_mutex_lock(&server.bgsavethread_mutex);
    server.bgsavethread_state = state;
    pthread_mutex_unlock(&server.bgsavethread_mutex);
}

void *dsRdbSave_thread(void *arg) {
    char tmpfile[256], *filename = (char*)arg;
    struct dirent *dp, de;
    int j, i, last_dbid = -1;
    FILE *fp;

    /* Change state to ACTIVE, to signal there is a saving thead working. */
    redisLog(REDIS_NOTICE,"Diskstore BGSAVE thread started");
    dsRdbSaveSetState(REDIS_BGSAVE_THREAD_ACTIVE);

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed opening .rdb for saving: %s",
            strerror(errno));
        dsRdbSaveSetState(REDIS_BGSAVE_THREAD_DONE_ERR);
        return NULL;
    }
    if (fwrite("REDIS0001",9,1,fp) == 0) goto werr;

    sleep(5);

    /* Scan all diskstore dirs looking for keys */
    for (j = 0; j < 256; j++) {
        for (i = 0; i < 256; i++) {
            DIR *dir;
            char buf[1024];

            /* For every directory, collect all the keys */
            snprintf(buf,sizeof(buf),"%s/%02x/%02x",server.ds_path,j,i);
            if ((dir = opendir(buf)) == NULL) {
                redisLog(REDIS_WARNING,"Disk store can't open dir %s: %s",
                    buf, strerror(errno));
                goto werr;
            }

            while(1) {
                char buf[1024];
                int dbid;
                FILE *entryfp;

                readdir_r(dir,&de,&dp);
                if (dp == NULL) break;
                if (dp->d_name[0] == '.') continue;
                /* If there is a '-' char in the file name, it's a temp file */
                if (strchr(dp->d_name,'-') != NULL) continue;

                /* Emit the SELECT DB opcode if needed. */
                dbid = dsGetDbidFromFilename(dp->d_name);
                if (dbid != last_dbid) {
                    last_dbid = dbid;
                    if (rdbSaveType(fp,REDIS_SELECTDB) == -1) goto werr;
                    if (rdbSaveLen(fp,dbid) == -1) goto werr;
                }

                /* Let's copy this file into the target .rdb */
                snprintf(buf,sizeof(buf),"%s/%02x/%02x/%s",
                    server.ds_path,j,i,dp->d_name);
                if ((entryfp = fopen(buf,"r")) == NULL) {
                    redisLog(REDIS_WARNING,"Can't open %s: %s",
                        buf,strerror(errno));
                    closedir(dir);
                    goto werr;
                }
                while(1) {
                    int nread = fread(buf,1,sizeof(buf),entryfp);

                    if (nread == 0) {
                        if (ferror(entryfp)) {
                            redisLog(REDIS_WARNING,"Error reading from file entry while performing BGSAVE for diskstore: %s", strerror(errno));
                            closedir(dir);
                            goto werr;
                        } else {
                            break;
                        }
                    }
                    if (fwrite(buf,1,nread,fp) != (unsigned)nread) {
                        closedir(dir);
                        goto werr;
                    }
                }
                fclose(entryfp);
            }
            closedir(dir);
        }
    }
    
    /* Output the end of file opcode */
    if (rdbSaveType(fp,REDIS_EOF) == -1) goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    zfree(filename);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s (diskstore)", strerror(errno));
        unlink(tmpfile);
        dsRdbSaveSetState(REDIS_BGSAVE_THREAD_DONE_ERR);
        return NULL;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk by diskstore thread");
    dsRdbSaveSetState(REDIS_BGSAVE_THREAD_DONE_OK);
    return NULL;

werr:
    zfree(filename);
    fclose(fp);
    unlink(tmpfile);
    dsRdbSaveSetState(REDIS_BGSAVE_THREAD_DONE_ERR);
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    return NULL;
}

int dsRdbSaveBackground(char *filename) {
    pthread_t thread;

    if (pthread_create(&thread,NULL,dsRdbSave_thread,zstrdup(filename)) != 0) {
        redisLog(REDIS_WARNING,"Can't create diskstore BGSAVE thread: %s",
            strerror(errno));
        return REDIS_ERR;
    } else {
        server.bgsavethread = thread;
        return REDIS_OK;
    }
}

int dsRdbSave(char *filename) {
    /* A blocking save is actually a non blocking save... just we wait
     * for it to terminate in a non-busy loop. */

    redisLog(REDIS_NOTICE,"Starting a blocking SAVE (BGSAVE + blocking wait)");
    server.dirty_before_bgsave = server.dirty;
    if (dsRdbSaveBackground(filename) == REDIS_ERR) return REDIS_ERR;
    while(1) {
        usleep(1000);
        int state;

        pthread_mutex_lock(&server.bgsavethread_mutex);
        state = server.bgsavethread_state;
        pthread_mutex_unlock(&server.bgsavethread_mutex);

        if (state == REDIS_BGSAVE_THREAD_DONE_OK ||
            state == REDIS_BGSAVE_THREAD_DONE_ERR) break;
    }
    return REDIS_OK;
}
