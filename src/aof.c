/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>

off_t getBaseAppendOnlyFileSize(void);
void freeClientArgv(client *c);
off_t getAppendOnlyFileSize(const char *filename);

/* ----------------------------------------------------------------------------
 * AOF meta file implementation.
 *
 * The following code implements the read/write logic of AOF meta file, which 
 * is used to track and manage all AOF files.
 *
 * There are three types of AOF, they are: 
 * BASE: Every time aof rewrite success, a BASE type aof will be generated, which 
 *       represents the redis SNAPSHOT at the moment when the rewrite is executed.
 *       In the aof meta file, the BASE type AOF (if we have) is always at the 
 *       beginning of the file. there is at most one BASE AOF.
 * HIST: Each time the rewrite is successful, the previous BASE AOF and INCR 
 *       AOFs will become HISTORY. they will be cleaned regularly in cron. 
 * INCR: There may be more than one (after multiple rewrite failures), and they 
 *       together represent all the incremental commands executed by redis after
 *       the last aof rewrite.
 * 
 * The following is a possible aof meta file content:
 * 
 * fileName appendonly.aof_b_2 fileSeq 2 fileType b
 * fileName appendonly.aof_i_1 fileSeq 1 fileType h
 * fileName appendonly.aof_i_2 fileSeq 2 fileType h
 * fileName appendonly.aof_i_3 fileSeq 3 fileType h
 * fileName appendonly.aof_i_4 fileSeq 4 fileType i
 * fileName appendonly.aof_i_5 fileSeq 5 fileType i
 * ------------------------------------------------------------------------- */

/* Create an empty aofInfo. */
aofInfo *aofInfoCreate(void) {
    return zcalloc(sizeof(aofInfo));
}

/* Free the aofInfo structure (pointed to by ai)  and its embedded file_name. */
void aofInfoFree(aofInfo *ai) {
    serverAssert(ai != NULL);
    if (ai->file_name) sdsfree(ai->file_name);
    zfree(ai);
}

/* Deep copy an aofInfo. */
aofInfo *aofInfoDup(aofInfo *orig) {
    serverAssert(orig != NULL);
    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdsdup(orig->file_name);
    ai->file_seq = orig->file_seq;
    ai->file_type = orig->file_type;
    return ai;
}

/* Method to free AOF list elements. */
void aofListFree(void *item) {
    aofInfo *ai = (aofInfo *)item;
    aofInfoFree(ai);
}

/* Method to duplicate AOF list elements. */
void *aofListDup(void *item) {
    return aofInfoDup(item);;
}

/* Create an empty aofMeta, which will be called in `loadAofMetaFromDisk`. */
aofMeta *aofMetaCreate(void) {
    aofMeta *am = zcalloc(sizeof(aofMeta));
    am->incr_aof_list = listCreate();
    am->history_aof_list = listCreate();
    listSetFreeMethod(am->incr_aof_list,aofListFree);
    listSetDupMethod(am->incr_aof_list,aofListDup);
    listSetFreeMethod(am->history_aof_list,aofListFree);
    listSetDupMethod(am->history_aof_list,aofListDup);
    return am;
}

/* Free the aofMeta structure (pointed to by am) and its embedded members. */
void aofMetaFree(aofMeta *am) {
    if (am->base_aof_info) aofInfoFree(am->base_aof_info);
    if (am->incr_aof_list) listRelease(am->incr_aof_list);
    if (am->history_aof_list) listRelease(am->history_aof_list);
    zfree(am);
}

char *getAofMetaName() {
    return sdscatprintf(sdsempty(), "%s%s", server.aof_filename, META_NAME_SUFFIX);
}

char *getTempAofMetaName() {
    return sdscatprintf(sdsempty(), "%s%s%s", META_TEM_NAME_PREFIX, server.aof_filename, META_NAME_SUFFIX);;
}

/* Returns the string representation of aofMeta pointed to by am.
 * 
 * The string is multiple lines separated by'\n', and each line 
 * represents an AOF file.
 * 
 * Each line contains 6 fields (they are separated by spaces).Among 
 * them, the 0th, 2nd, and 4th field respectively represent the meta 
 * key, which meanings:
 * fileName: AOF file name
 * fileSeq: The serial number of the AOF file
 * fileType: Types of AOF file, There are three types: b(BASE)、h(HIST)、i(INCR)
 * 
 * A possible line:
 *    fileName appendonly.aof_b_5 fileSeq 5 fileType b
 * 
 * The BASE AOF information (if we have) will be placed on the first 
 * line, followed by history type AOFs and Finally is the INCR type.
 */
sds getAofMetaAsString(aofMeta *am) {
    serverAssert(am != NULL);

    sds buf = sdsempty();
    listNode *ln;
    listIter li;

    /* 1. Add base aof information, it is always at the beginning of the meta file. */
    if (am->base_aof_info) {
        buf = sdscatprintf(buf,"%s %s %s %" PRId64 " %s %c\n",
                AOF_META_KEY_FILE_NAME,am->base_aof_info->file_name,
                AOF_META_KEY_FILE_SEQ,am->base_aof_info->file_seq,
                AOF_META_KEY_FILE_TYPE,am->base_aof_info->file_type);
    }
    
    /* 2. Add history type aof information. */
    listRewind(am->history_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;

        buf = sdscatprintf(buf, "%s %s %s %" PRId64 " %s %c\n",
            AOF_META_KEY_FILE_NAME,ai->file_name,
            AOF_META_KEY_FILE_SEQ,ai->file_seq,
            AOF_META_KEY_FILE_TYPE,ai->file_type);
    }

    /* 3. Add incr type aof information. */
    listRewind(am->incr_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;

        buf = sdscatprintf(buf, "%s %s %s %" PRId64 " %s %c\n",
            AOF_META_KEY_FILE_NAME,ai->file_name,
            AOF_META_KEY_FILE_SEQ,ai->file_seq,
            AOF_META_KEY_FILE_TYPE,ai->file_type);
    }

    return buf;
}

/*  Load the meta information from the disk to `server.aof_meta` when 
 *  the redis server start.
 *  
 *  During the loading process, we will conduct strict error checking. 
 *  Once there are file opening error, format error, etc., we will 
 *  directly exit the redis process.
 * 
 *  Note: We will ignore the DOESN'T EXIST error, because this will 
 *  happen when we upgrade from an old version redis.
 */
void loadAofMetaFromDisk(void) {
    const char *err = NULL;
    struct redis_stat sb;
    int64_t maxseq = 0;
    server.aof_meta = aofMetaCreate();

    sds meta_name = getAofMetaName();
    FILE *fp = fopen(meta_name,"r");
    if (fp == NULL) {
        int en = errno;
        if (redis_stat(meta_name,&sb) == 0) {
            serverLog(LL_WARNING,
                     "Fatal error: can't open the aof meta file %s for reading: %s",
                     meta_name,strerror(en));
            exit(1);
        } else {
            serverLog(LL_WARNING,
                     "The aof meta file %s doesn't exist: %s",
                     meta_name,strerror(errno));
            sdsfree(meta_name);
            return;
        }
    }  

    sdsfree(meta_name);

    char buf[1024];
    sds config = sdsempty();

    while (fgets(buf,1024,fp) != NULL)
        config = sdscat(config,buf);

    fclose(fp);

    int linenum = 0, totlines, i;
    sds *lines = sdssplitlen(config,strlen(config),"\n",1,&totlines);
    serverAssert(totlines > 0);

    for (i = 0; i < totlines; i++) {
        sds *argv;
        int argc;

        linenum = i+1;

        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip comments and blank lines */
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;

        argv = sdssplitargs(lines[i],&argc);
        if (argv == NULL || argc != 6) {
            err = "The aof meta file is invalid format";
            goto loaderr;
        }

        aofInfo* ai = aofInfoCreate();
        if (strcmp(argv[0], AOF_META_KEY_FILE_NAME) == 0) {
            ai->file_name = sdsnew(argv[1]);
        } else {
            err = "Mismatched meta key";
            goto loaderr;
        }

        if (strcmp(argv[2], AOF_META_KEY_FILE_SEQ) == 0) {
            ai->file_seq = atol(argv[3]);
        } else {
            err = "Mismatched meta key";
            goto loaderr;
        }

        if (strcmp(argv[4], AOF_META_KEY_FILE_TYPE) == 0) {
            ai->file_type = (argv[5])[0];
        } else {
            err = "Mismatched meta key";
            goto loaderr;
        }

        sdsfreesplitres(argv, argc);

        if (ai->file_type == AOF_FILE_TYPE_BASE) {
            if (server.aof_meta->base_aof_info) {
                err = "Found duplicate base aof information";
                goto loaderr;
            }
            server.aof_meta->base_aof_info = ai;
            server.aof_meta->curr_base_aof_seq = ai->file_seq;
        } else if (ai->file_type == AOF_FILE_TYPE_HIST) {
            listAddNodeTail(server.aof_meta->history_aof_list,ai);
        } else if (ai->file_type == AOF_FILE_TYPE_INCR) {
            if (ai->file_seq <= maxseq) {
                err = "Found Non-increasing sequence number";
                goto loaderr;
            }
            listAddNodeTail(server.aof_meta->incr_aof_list,ai);
            server.aof_meta->curr_incr_aof_seq = ai->file_seq;
            maxseq = ai->file_seq;
        } else {
            err = "Unknown aof file type";
            goto loaderr;
        }
    }

    sdsfreesplitres(lines, totlines);
    sdsfree(config);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL AOF META FILE ERROR ***\n");
    if (i < totlines) {
        fprintf(stderr, "Reading the meta file, at line %d\n", linenum);
        fprintf(stderr, ">>> '%s'\n", lines[i]);
    }
    fprintf(stderr, "%s\n", err);
    exit(1);
}

/* Deep copy an aofMeta from orig.
 * 
 * In `backgroundRewriteDoneHandler`, we will first deep copy a temporary aof_meta 
 * from the `server.aof_meta`, and try to modify it. Once everything is modified, we 
 * will atomically make the `server.aof_meta` point to this temporary aof_meta.
 */
aofMeta *aofMetaDup(aofMeta *orig) {
    serverAssert(orig != NULL);
    aofMeta *am = zcalloc(sizeof(aofMeta));
    
    am->curr_base_aof_seq = orig->curr_base_aof_seq;
    am->curr_incr_aof_seq = orig->curr_incr_aof_seq;
    am->dirty = orig->dirty;

    if (orig->base_aof_info) am->base_aof_info = aofInfoDup(orig->base_aof_info);
    
    am->incr_aof_list = listDup(orig->incr_aof_list);
    am->history_aof_list = listDup(orig->history_aof_list);
    if (am->incr_aof_list == NULL || am->history_aof_list == NULL) {
        aofMetaFree(am);
        am = NULL;
    }
    return am;
}

/* Called in `backgroundRewriteDoneHandler`. Get a new BASE type AOF name, and 
 * mark the previous (if we have) BASE AOF as the HIST type.
 * 
 * The format of BASE type AOF name is:
 *   server.aof_filename_b_seq
 * 
 * It consists of three parts:
 * server.aof_filename: Configured by the user as the base part of the aof name
 * _b_: BASE type AOF-specific suffix
 * seq: An incremental value to ensure that a redis process will not open the same BASE AOF file
 */
const char *getNewBaseAofNameAndMarkPreAsHistory(aofMeta *am) {
    serverAssert(am != NULL);
    if (am->base_aof_info) {
        am->base_aof_info->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeTail(am->history_aof_list,am->base_aof_info);
    }

    aofInfo *ai = aofInfoCreate();
    ai->file_name = sdscatprintf(sdsempty(),"%s%s%" PRId64 "",server.aof_filename, 
                        BASE_AOF_SUFFIX,++am->curr_base_aof_seq);
    ai->file_seq = am->curr_base_aof_seq;
    ai->file_type = AOF_FILE_TYPE_BASE;
    am->base_aof_info = ai;
    am->dirty = 1;
    return am->base_aof_info->file_name;
}

/* Get a new INCR type AOF name and add it to the meta structure.
 * 
 * The format of INCR type AOF name is:
 *   server.aof_filename_i_seq
 * 
 * It consists of three parts:
 * server.aof_filename: Configured by the user as the base part of the aof name
 * _i_: INCR type AOF-specific suffix
 * seq: An incremental value to ensure that a redis process will not open the same INCR AOF file
 */
const char* getNewIncrAofNameAndAddIt(aofMeta *am) {
    aofInfo *ai = aofInfoCreate();
    ai->file_type = AOF_FILE_TYPE_INCR;
    ai->file_name = sdscatprintf(sdsempty(),"%s%s%" PRId64 "",server.aof_filename, 
                        INCR_AOF_SUFFIX,++am->curr_incr_aof_seq);
    ai->file_seq = am->curr_incr_aof_seq;
    listAddNodeTail(am->incr_aof_list,ai);

    am->dirty = 1;
    return ai->file_name;
}

/* Get the last INCR AOF name or create a new one. */
const char *getLastIncrAofName(aofMeta *am) {
    serverAssert(am != NULL);

    if (listLength(am->incr_aof_list) == 0) {
        return getNewIncrAofNameAndAddIt(am);
    }

    listNode *lastnode = listIndex(am->incr_aof_list,-1);
    aofInfo *ai = listNodeValue(lastnode);
    return ai->file_name;
}

/* Called in `backgroundRewriteDoneHandler`. when aof rewrite success, This
 * function will change the AOF file type in `incr_aof_list` from AOF_FILE_TYPE_INCR 
 * to AOF_FILE_TYPE_HIST, and move them to the `history_aof_list`.
 */
void markRewrittenIncrAofAsHistory(aofMeta *am) {
    serverAssert(am != NULL);

    if (listLength(am->incr_aof_list) == 0) {
        return;
    }
    
    listNode *ln;
    listIter li;

    listRewindTail(am->incr_aof_list,&li);

    /* server.aof_fd != -1 means that AOF is open, then we must skip the last 
     * AOF, because this one is our currently writing. */
    if (server.aof_fd != -1) {
        ln = listNext(&li);
        serverAssert(ln != NULL);
    }

    /* move aofInfo from incr_aof_list to history_aof_list. */
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);

        aofInfo *hai = aofInfoDup(ai);
        hai->file_type = AOF_FILE_TYPE_HIST;
        listAddNodeTail(am->history_aof_list,hai);
        listDelNode(am->incr_aof_list,ln);
    }

    am->dirty = 1;
}

/* Write the formatted meta string to disk. */
int writeAofMetaFile(sds buf) {
    int ret = C_OK;
    ssize_t nwritten;
    int len;

    sds meta_name = getAofMetaName();
    sds temp_meta_name = getTempAofMetaName();
    int fd = open(temp_meta_name,O_WRONLY|O_TRUNC|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
                "Can't open the aof meta file %s: %s",
                 temp_meta_name, strerror(errno));
        ret = C_ERR;
        goto cleanup;
    }
    
    len = sdslen(buf);
    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) continue;
            serverLog(LL_WARNING,
                "Error trying to write the temporary AOF meta file %s: %s",
                temp_meta_name,
                strerror(errno));
            ret = C_ERR;
            goto cleanup;
        }

        len -= nwritten;
        buf += nwritten;
    }

    if (redis_fsync(fd) == -1) {
        serverLog(LL_WARNING,"Fail to fsync the temp AOF file %s: %s.", 
                              temp_meta_name,
                               strerror(errno));
        ret = C_ERR;
        goto cleanup;
    }

    if (rename(temp_meta_name, meta_name) != 0) {
        serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF meta file %s into %s: %s",
                temp_meta_name,
                meta_name,
                strerror(errno));
        ret = C_ERR;
    }
    
cleanup:
    close(fd);
    sdsfree(meta_name);
    sdsfree(temp_meta_name);
    return ret;
}

int writeAofMetaAndFree(sds aof_meta_str) {
    serverAssert(aof_meta_str != NULL);
    int ret = writeAofMetaFile(aof_meta_str);
    sdsfree(aof_meta_str);
    return ret;
}

/* Persist the aofMeta information pointed to by am to disk. */
int persistAofMeta(aofMeta *am) {
    if (am->dirty == 0) {
        return C_OK;
    }

    am->dirty = 0;
    sds aof_meta_str = getAofMetaAsString(am);
    return writeAofMetaAndFree(aof_meta_str);
}

/* AOF garbage collection processing function.
 * 
 * When aof rewrite succuss, the previous BASE and INCR AOFs will 
 * become HIST type and be moved into history_aof_list.
 * 
 * The function will traverse the history_aof_list and submit the 
 * delete task to the bio thread.
 */
void delHistoryAofFilesCron(aofMeta *am) {
    serverAssert(am != NULL);
    if (server.aof_enabled_auto_gc == 0 || 
         listLength(am->history_aof_list) == 0) {
        return;
    }

    listNode *ln;
    listIter li;

    listRewind(am->history_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_HIST);
        serverLog(LL_DEBUG, "Delete the history aof file %s in background",ai->file_name);
        bioCreateDelJob(ai->file_name);
        listDelNode(am->history_aof_list,ln);
    }
    am->dirty = 1;
    int ret = persistAofMeta(server.aof_meta);
    if (ret != C_OK) {
        exit(1);
    }
}

/* Called after `loadDataFromDisk` when redis start. If `server.aof_state` is 
 * AOF_ON, It will do two things:
 * 1. Open the last opened INCR type AOF for writing, If not, create a new one
 * 2. synchronously update the meta file to the disk
 * 
 * if any of the above two steps fails, the redis process will exit.
 */
void openAofIfNeeded(void) {
    if (server.aof_state != AOF_ON) {
        return;
    }

    serverAssert(server.aof_meta != NULL);
    serverAssert(server.aof_fd == -1);

    const char *aof_name = getLastIncrAofName(server.aof_meta);
    server.aof_fd = open(aof_name,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (server.aof_fd == -1) {
        serverLog(LL_WARNING,"Can't open the append-only file %s: %s",aof_name,strerror(errno));
        exit(1);
    }

    int ret = persistAofMeta(server.aof_meta);
    if (ret != C_OK) {
        exit(1);
    }
}

/* Called when an aof rewrite is executed. If `server.aof_state` is AOF_ON, It will 
 * do two things:
 * 1. open a new INCR type AOF for writing
 * 2. synchronously update the meta file to the disk
 * 
 * If any of the above two steps fails, the redis process will exit.
 * */
void openNewIncrAofForAppend(void) {
    serverAssert(server.aof_meta != NULL);

    const char *new_aof_name = getNewIncrAofNameAndAddIt(server.aof_meta);

    /* Close old aof_fd if needed. */
    if (server.aof_fd != -1) bioCreateCloseJob(server.aof_fd);

    server.aof_fd = open(new_aof_name,O_WRONLY|O_TRUNC|O_CREAT,0644);
    if (server.aof_fd == -1) {
        serverLog(LL_WARNING,"Can't open the append-only file %s: %s",
                    new_aof_name,strerror(errno));
        exit(1);
    }
    /* Reset the aof_newfile_size. */
    server.aof_newfile_size = 0;
    int ret = persistAofMeta(server.aof_meta);
    if (ret == C_ERR) {
        exit(1);
    }
}

/*  */
int openLastOrCreateIncrAofForAppend(void) {
    serverAssert(server.aof_meta != NULL);
    
    const char *incr_aof_name = getLastIncrAofName(server.aof_meta);
    int fd = open(incr_aof_name,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,"Can't open the append-only file %s: %s",
                    incr_aof_name,strerror(errno));
        return C_ERR;
    }

    int ret = persistAofMeta(server.aof_meta);
    if (ret == C_ERR) {
        close(fd);
        return ret;
    }

    if (server.aof_fd != -1) bioCreateCloseJob(server.aof_fd);

    server.aof_newfile_size = getAppendOnlyFileSize(incr_aof_name);
    server.aof_fd = fd;
    return C_OK;
}
/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

/* Return true if an AOf fsync is currently already in progress in a
 * BIO thread. */
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
void aof_background_fsync(int fd) {
    bioCreateFsyncJob(fd);
}

/* Kills an AOFRW child process if exists */
void killAppendOnlyChild(void) {
    int statloc;
    /* No AOFRW child? return. */
    if (server.child_type != CHILD_TYPE_AOF) return;
    /* Kill AOFRW child, wait for child exit. */
    serverLog(LL_NOTICE,"Killing running AOF rewrite child: %ld",
        (long) server.child_pid);
    if (kill(server.child_pid,SIGUSR1) != -1) {
        while(waitpid(-1, &statloc, 0) != server.child_pid);
    }
    aofRemoveTempFile(server.child_pid);
    resetChildState();
    server.aof_rewrite_time_start = -1;
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
void stopAppendOnly(void) {
    serverAssert(server.aof_state != AOF_OFF);
    flushAppendOnlyFile(1);
    if (redis_fsync(server.aof_fd) == -1) {
        serverLog(LL_WARNING,"Fail to fsync the AOF file: %s",strerror(errno));
    } else {
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    }
    close(server.aof_fd);

    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_scheduled = 0;
    server.aof_newfile_size = 0;
    killAppendOnlyChild();
    sdsfree(server.aof_buf);
    server.aof_buf = sdsempty();
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
int startAppendOnly(void) {
    serverAssert(server.aof_state == AOF_OFF);
    if (hasActiveChildProcess() && server.child_type != CHILD_TYPE_AOF) {
        if (openLastOrCreateIncrAofForAppend() != C_OK) {
            return C_ERR;
        }
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled but there is already another background operation. An AOF background was scheduled to start when possible.");
    } else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. */
        if (server.child_type == CHILD_TYPE_AOF) {
            serverLog(LL_WARNING,"AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }

        if (rewriteAppendOnlyFileBackground(1) == C_ERR) {
            serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. */
    server.aof_state = AOF_WAIT_REWRITE;
    server.aof_last_fsync = server.unixtime;
    
    /* If AOF fsync error in bio job, we just ignore it and log the event. */
    int aof_bio_fsync_status;
    atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status);
    if (aof_bio_fsync_status == C_ERR) {
        serverLog(LL_WARNING,
            "AOF reopen, just ignore the AOF fsync error in bio job");
        atomicSet(server.aof_bio_fsync_status,C_OK);
    }

    /* If AOF was in error state, we just ignore it and log the event. */
    if (server.aof_last_write_status == C_ERR) {
        serverLog(LL_WARNING,"AOF reopen, just ignore the last error.");
        server.aof_last_write_status = C_OK;
    }
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. */
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;

    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) continue;
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * About the 'force' argument:
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync. */
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    if (sdslen(server.aof_buf) == 0) {
        /* Check if we need to do fsync even the aof buffer is empty,
         * because previously in AOF_FSYNC_EVERYSEC mode, fsync is
         * called only when aof buffer is not empty, so if users
         * stop write commands before fsync called in one second,
         * the data in page cache cannot be flushed in time. */
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&
            server.aof_fsync_offset != server.aof_current_size &&
            server.unixtime > server.aof_last_fsync &&
            !(sync_in_progress = aofFsyncInProgress())) {
            goto try_fsync;
        } else {
            return;
        }
    }

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress();

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        if (sync_in_progress) {
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                return;
            }
            /* Otherwise fall through, and go write since we can't wait
             * over two seconds. */
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */

    if (server.aof_flush_sleep && sdslen(server.aof_buf)) {
        usleep(server.aof_flush_sleep);
    }

    latencyStartMonitor(latency);
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    if (sync_in_progress) {
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    } else if (hasActiveChildProcess()) {
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    server.aof_flush_postponed_start = 0;

    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
                server.aof_last_write_errno = errno;
            }
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }

            if (ftruncate(server.aof_fd, server.aof_newfile_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. */
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the reply
             * for the client is already in the output buffers (both writes and
             * reads), and the changes to the db can't be rolled back. Since we
             * have a contract with the user that on acknowledged or observed
             * writes are is synced on disk, we must exit. */
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                server.aof_newfile_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... */
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    server.aof_current_size += nwritten;
    server.aof_newfile_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        sdsclear(server.aof_buf);
    } else {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    if (server.aof_no_fsync_on_rewrite && hasActiveChildProcess())
        return;

    /* Perform the fsync if needed. */
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        latencyStartMonitor(latency);
        /* Let's try to get this data on the disk. To guarantee data safe when
         * the AOF fsync policy is 'always', we should exit if failed to fsync
         * AOF (see comment next to the exit(1) after write error above). */
        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING,"Can't persist AOF for fsync error when the "
              "AOF fsync policy is 'always': %s. Exiting...", strerror(errno));
            exit(1);
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        server.aof_last_fsync = server.unixtime;
    }
}

sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

/* Generate a piece of timestamp annotation for AOF if current record timestamp
 * in AOF is not equal server unix time. If we specify 'force' argument to 1,
 * we would generate one without check, currently, it is useful in AOF rewriting
 * child process which always needs to record one timestamp at the beginning of
 * rewriting AOF.
 *
 * Timestamp annotation format is "#TS:${timestamp}\r\n". "TS" is short of
 * timestamp and this method could save extra bytes in AOF. */
sds genAofTimestampAnnotationIfNeeded(int force) {
    sds ts = NULL;

    if (force || server.aof_cur_timestamp < server.unixtime) {
        server.aof_cur_timestamp = force ? time(NULL) : server.unixtime;
        ts = sdscatfmt(sdsempty(), "#TS:%I\r\n", server.aof_cur_timestamp);
        serverAssert(sdslen(ts) <= AOF_ANNOTATION_LINE_MAX_LEN);
    }
    return ts;
}

void feedAppendOnlyFile(int dictid, robj **argv, int argc) {
    sds buf = sdsempty();

    /* Feed timestamp if needed */
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(0);
        if (ts != NULL) {
            buf = sdscatsds(buf, ts);
            sdsfree(ts);
        }
    }

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.aof_selected_db = dictid;
    }

    /* All commands should be propagated the same way in AOF as in replication.
     * No need for AOF-specific translation. */
    buf = catAppendOnlyGenericCommand(buf,argc,argv);

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    if (server.aof_state == AOF_ON ||
        (server.aof_state == AOF_WAIT_REWRITE && server.child_pid != -1)) {
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));
    }

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
struct client *createAOFClient(void) {
    struct client *c = createClient(NULL);

    c->id = CLIENT_ID_AOF; /* So modules can identify it's the AOF client. */

    /*
     * The AOF client should never be blocked (unlike master
     * replication connection).
     * This is because blocking the AOF client might cause
     * deadlock (because potentially no one will unblock it).
     * Also, if the AOF client will be blocked just for
     * background processing there is a chance that the
     * command execution order will be violated.
     */
    c->flags = CLIENT_DENY_BLOCKING;

    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    return c;
}

/* Replay the append log file. On success AOF_OK is returned,
 * otherwise, one of the following is returned:
 * AOF_OPEN_ERR: Failed to open the AOF file.
 * AOF_NOT_EXIST: AOF file doesn't exist.
 * AOF_EMPTY: The AOF file is empty (nothing to load).
 * AOF_FAILED: Failed to load the AOF file. */
int loadSingleAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;
    off_t valid_up_to = 0; /* Offset of latest well-formed command loaded. */
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. */
    int ret;

    if (fp == NULL) {
        int en = errno;
        if (redis_stat(filename, &sb) == 0) {
            serverLog(LL_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(en));
            return AOF_OPEN_ERR;
        } else {
            serverLog(LL_WARNING,"The append log file %s doesn't exist: %s",filename,strerror(errno));
            return AOF_NOT_EXIST;
        }
    }

    /* Handle a zero-length AOF file as a special case. An empty AOF file
     * is a valid AOF because an empty server with AOF enabled will create
     * a zero length file at startup, that will remain like that if no write
     * operation is received. */
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        server.aof_fsync_offset = server.aof_current_size;
        fclose(fp);
        return AOF_EMPTY;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.aof_state = AOF_OFF;

    fakeClient = createAOFClient();

    /* Check if this AOF file has an RDB preamble. In that case we need to
     * load the RDB file and later continue loading the AOF tail. */
    char sig[5]; /* "REDIS" */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* No RDB preamble, seek back at 0 offset. */
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB preamble. Pass loading the RDB functions. */
        rio rdb;

        serverLog(LL_NOTICE,"Reading RDB preamble from AOF file...");
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,RDBFLAGS_AOF_PREAMBLE,NULL,server.db) != C_OK) {
            serverLog(LL_WARNING,"Error reading the RDB preamble of the AOF file, AOF loading aborted");
            goto readerr;
        } else {
            serverLog(LL_NOTICE,"Reading the remaining AOF tail...");
        }
    }

    /* Read the actual AOF file, in REPL format, command by command. */
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[AOF_ANNOTATION_LINE_MAX_LEN];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            processEventsWhileBlocked();
            processModuleLoadingProgressEvent(1);
        }

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] == '#') continue; /* Skip annotations */
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;
        if ((size_t)argc > SIZE_MAX / sizeof(robj*)) goto fmterr;

        /* Load the next command in the AOF as our fake client
         * argv. */
        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        fakeClient->argv_len = argc;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. */
            char *readres = fgets(buf,sizeof(buf),fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. */
                freeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf+1,NULL,10);

            /* Read it into a string object. */
            argsds = sdsnewlen(SDS_NOINIT,len);
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeClientArgv(fakeClient);
                goto readerr;
            }
            argv[j] = createObject(OBJ_STRING,argsds);

            /* Discard CRLF. */
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeClientArgv(fakeClient);
                goto readerr;
            }
        }

        /* Command lookup */
        cmd = lookupCommand(argv,argc);
        if (!cmd) {
            serverLog(LL_WARNING,
                "Unknown command '%s' reading the append only file",
                (char*)argv[0]->ptr);
            freeClientArgv(fakeClient);
            ret = AOF_FAILED;
            goto cleanup;
        }

        if (cmd->proc == multiCommand) valid_before_multi = valid_up_to;

        /* Run the command in the context of a fake client */
        fakeClient->cmd = fakeClient->lastcmd = cmd;
        if (fakeClient->flags & CLIENT_MULTI &&
            fakeClient->cmd->proc != execCommand)
        {
            queueMultiCommand(fakeClient);
        } else {
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply */
        serverAssert(fakeClient->bufpos == 0 &&
                     listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked */
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        freeClientArgv(fakeClient);
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. */
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING,
            "Revert incomplete MULTI/EXEC transaction in AOF file");
        valid_up_to = valid_before_multi;
        goto uxeof;
    }

loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. */
    server.aof_state = old_aof_state;
    ret = AOF_OK;
    goto cleanup;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    if (!feof(fp)) {
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        ret = AOF_FAILED;
        goto cleanup;
    }

uxeof: /* Unexpected AOF end of file. */
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        serverLog(LL_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (unsigned long long) valid_up_to);
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file: %s",
                    strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file: %s",
                    strerror(errno));
            } else {
                serverLog(LL_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok;
            }
        }
    }
    serverLog(LL_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    ret = AOF_FAILED;
    goto cleanup;

fmterr: /* Format error. */
    serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    ret = AOF_FAILED;
    /* fall through to cleanup. */

cleanup:
    if (fakeClient) freeClient(fakeClient);
    fclose(fp);
    return ret;
}

/* Load the AOF files according the aofMeta pointed by am. */
int loadAppendOnlyFiles(aofMeta *am) {
    serverAssert(am != NULL);
    int ret = C_OK;
    long long start;
    off_t base_aof_size = 0, incr_aof_size = 0, size;
    char *aof_name;

    /* If there is no information about BASE AOF and INCR AOF in aofMeta, then 
     * there is only one possibility: the aof meta file does not exist, we may 
     * be starting from an old redis version. so we must fall back to the previous 
     * loading mode.
     */
    if (am->base_aof_info == NULL && listLength(am->incr_aof_list) == 0) {
        struct redis_stat sb;
        /* If the `server.aof_filename` file does not exist, we will directly return 
         * AOF_NOT_EXIST, and redis will ignore this error. */
        if (redis_stat(server.aof_filename, &sb) != 0) {
            return AOF_NOT_EXIST;
        }

        /* If the server.aof_filename file exists, we manually construct a BASE 
         * type aofInfo and add it to aofMeta. In this way, we can reuse the 
         * following code to load this AOF file. */
        aofInfo *ai = aofInfoCreate();
        ai->file_name = sdsnew(server.aof_filename);
        ai->file_seq = 1;
        ai->file_type = AOF_FILE_TYPE_BASE;
        am->base_aof_info = ai;
        am->dirty = 1;

        int ret = persistAofMeta(am);
        if (ret == C_ERR) {
            return AOF_FAILED;
        }
    }

    /* Load BASE AOF. */
    if (am->base_aof_info) {
        serverAssert(am->base_aof_info->file_type == AOF_FILE_TYPE_BASE);
        aof_name = (char*)am->base_aof_info->file_name;
        size = getAppendOnlyFileSize(aof_name);
        start = ustime();
        startLoading(size, RDBFLAGS_AOF_PREAMBLE, 0);
        ret = loadSingleAppendOnlyFile(aof_name);
        stopLoading(ret == AOF_OK);
        if (ret == AOF_OK) {
            base_aof_size = size;
            serverLog(LL_NOTICE,"DB loaded from append only file %s: %.3f seconds",aof_name,(float)(ustime()-start)/1000000);
        }

        /* If an AOF file exists in the meta but not on the disk, we consider this to be a fatal error. */
        if (ret == AOF_NOT_EXIST) ret = AOF_FAILED;

        if (ret != AOF_OK && ret != AOF_EMPTY) {
            return ret;
        }
    }

    /* Load INCR AOFs. */
    listNode *ln;
    listIter li;

    listRewind(am->incr_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = (aofInfo*)ln->value;
        serverAssert(ai->file_type == AOF_FILE_TYPE_INCR);
        aof_name = (char*)ai->file_name;
        size = getAppendOnlyFileSize(aof_name);
        start = ustime();
        startLoading(size, RDBFLAGS_AOF_PREAMBLE, 0);
        ret = loadSingleAppendOnlyFile(aof_name);
        stopLoading(ret == AOF_OK);
        if (ret == AOF_OK) {
            incr_aof_size += size;
            serverLog(LL_NOTICE,"DB loaded from append only file %s: %.3f seconds",aof_name,(float)(ustime()-start)/1000000);
        }

        if (ret == AOF_NOT_EXIST) ret = AOF_FAILED;

        if (ret != AOF_OK && ret != AOF_EMPTY) {
            return ret;
        }
    }

    server.aof_current_size = base_aof_size + incr_aof_size;
    server.aof_rewrite_base_size = server.aof_current_size;
    server.aof_fsync_offset = server.aof_current_size;
    return ret;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;

        while (quicklistNext(li,&entry)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"RPUSH",5) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }

            if (entry.value) {
                if (!rioWriteBulkString(r,(char*)entry.value,entry.sz)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            } else {
                if (!rioWriteBulkLongLong(r,entry.longval)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkLongLong(r,llval)) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkString(r,ele,sdslen(ele))) {
                dictReleaseIterator(di);
                return 0;          
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = lpSeek(zl,0);
        serverAssert(eptr != NULL);
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            vstr = lpGetValue(eptr,&vlen,&vll);
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,score)) return 0;
            if (vstr != NULL) {
                if (!rioWriteBulkString(r,(char*)vstr,vlen)) return 0;
            } else {
                if (!rioWriteBulkLongLong(r,vll)) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,*score) ||
                !rioWriteBulkString(r,ele,sdslen(ele)))
            {
                dictReleaseIterator(di);
                return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either OBJ_HASH_KEY or OBJ_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return rioWriteBulkString(r, (char*)vstr, vlen);
        else
            return rioWriteBulkLongLong(r, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return rioWriteBulkString(r, value, sdslen(value));
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                AOF_REWRITE_ITEMS_PER_CMD : items;

            if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                !rioWriteBulkString(r,"HMSET",5) ||
                !rioWriteBulkObject(r,key)) 
            {
                hashTypeReleaseIterator(hi);
                return 0;
            }
        }

        if (!rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) ||
            !rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE))
        {
            hashTypeReleaseIterator(hi);
            return 0;           
        }
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. */
int rioWriteBulkStreamID(rio *r,streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    retval = rioWriteBulkString(r,replyid,sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. */
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
     /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
               RETRYCOUNT <count> JUSTID FORCE. */
    streamID id;
    streamDecodeID(rawid,&id);
    if (rioWriteBulkCount(r,'*',12) == 0) return 0;
    if (rioWriteBulkString(r,"XCLAIM",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    if (rioWriteBulkString(r,"0",1) == 0) return 0;
    if (rioWriteBulkStreamID(r,&id) == 0) return 0;
    if (rioWriteBulkString(r,"TIME",4) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_time) == 0) return 0;
    if (rioWriteBulkString(r,"RETRYCOUNT",10) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_count) == 0) return 0;
    if (rioWriteBulkString(r,"JUSTID",6) == 0) return 0;
    if (rioWriteBulkString(r,"FORCE",5) == 0) return 0;
    return 1;
}

/* Helper for rewriteStreamObject(): emit the XGROUP CREATECONSUMER is
 * needed in order to create consumers that do not have any pending entries.
 * All this in the context of the specified key and group. */
int rioWriteStreamEmptyConsumer(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer) {
    /* XGROUP CREATECONSUMER <key> <group> <consumer> */
    if (rioWriteBulkCount(r,'*',5) == 0) return 0;
    if (rioWriteBulkString(r,"XGROUP",6) == 0) return 0;
    if (rioWriteBulkString(r,"CREATECONSUMER",14) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. */
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. */
        while(streamIteratorGetID(&si,&id,&numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs. */

            /* Emit the XADD <key> <id> ...fields... command. */
            if (!rioWriteBulkCount(r,'*',3+numfields*2) || 
                !rioWriteBulkString(r,"XADD",4) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkStreamID(r,&id)) 
            {
                streamIteratorStop(&si);
                return 0;
            }
            while(numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si,&field,&value,&field_len,&value_len);
                if (!rioWriteBulkString(r,(char*)field,field_len) ||
                    !rioWriteBulkString(r,(char*)value,value_len)) 
                {
                    streamIteratorStop(&si);
                    return 0;                  
                }
            }
        }
    } else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. */
        id.ms = 0; id.seq = 1; 
        if (!rioWriteBulkCount(r,'*',7) ||
            !rioWriteBulkString(r,"XADD",4) ||
            !rioWriteBulkObject(r,key) ||
            !rioWriteBulkString(r,"MAXLEN",6) ||
            !rioWriteBulkString(r,"0",1) ||
            !rioWriteBulkStreamID(r,&id) ||
            !rioWriteBulkString(r,"x",1) ||
            !rioWriteBulkString(r,"y",1))
        {
            streamIteratorStop(&si);
            return 0;     
        }
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. */
    if (!rioWriteBulkCount(r,'*',3) ||
        !rioWriteBulkString(r,"XSETID",6) ||
        !rioWriteBulkObject(r,key) ||
        !rioWriteBulkStreamID(r,&s->last_id)) 
    {
        streamIteratorStop(&si);
        return 0; 
    }


    /* Create all the stream consumer groups. */
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. */
            if (!rioWriteBulkCount(r,'*',5) ||
                !rioWriteBulkString(r,"XGROUP",6) ||
                !rioWriteBulkString(r,"CREATE",6) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkString(r,(char*)ri.key,ri.key_len) ||
                !rioWriteBulkStreamID(r,&group->last_id)) 
            {
                raxStop(&ri);
                streamIteratorStop(&si);
                return 0;
            }

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers would be generated with
             * XGROUP CREATECONSUMER. */
            raxIterator ri_cons;
            raxStart(&ri_cons,group->consumers);
            raxSeek(&ri_cons,"^",NULL,0);
            while(raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* If there are no pending entries, just emit XGROUP CREATECONSUMER */
                if (raxSize(consumer->pel) == 0) {
                    if (rioWriteStreamEmptyConsumer(r,key,(char*)ri.key,
                                                    ri.key_len,consumer) == 0)
                    {
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                    continue;
                }
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. */
                raxIterator ri_pel;
                raxStart(&ri_pel,consumer->pel);
                raxSeek(&ri_pel,"^",NULL,0);
                while(raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r,key,(char*)ri.key,
                                                   ri.key_len,consumer,
                                                   ri_pel.key,nack) == 0)
                    {
                        raxStop(&ri_pel);
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. */
int rewriteModuleObject(rio *r, robj *key, robj *o, int dbid) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io,mt,r,key,dbid);
    mt->aof_rewrite(&io,key,mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    int j;
    long key_count = 0;
    long long updated_time = 0;

    /* Record timestamp at the beginning of rewriting AOF. */
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(1);
        if (rioWrite(aof,ts,sdslen(ts)) == 0) { sdsfree(ts); goto werr; }
        sdsfree(ts);
    }

    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* SELECT the new DB */
        if (rioWrite(aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;
            size_t aof_bytes_before_key = aof->processed_bytes;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value */
            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(aof,o) == 0) goto werr;
            } else if (o->type == OBJ_LIST) {
                if (rewriteListObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_SET) {
                if (rewriteSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_ZSET) {
                if (rewriteSortedSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_HASH) {
                if (rewriteHashObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_STREAM) {
                if (rewriteStreamObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_MODULE) {
                if (rewriteModuleObject(aof,&key,o,j) == 0) goto werr;
            } else {
                serverPanic("Unknown object type");
            }

            /* In fork child process, we can try to release memory back to the
             * OS and possibly avoid or decrease COW. We guve the dismiss
             * mechanism a hint about an estimated size of the object we stored. */
            size_t dump_size = aof->processed_bytes - aof_bytes_before_key;
            if (server.in_fork_child) dismissObject(o, dump_size);

            /* Save the expire time */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(aof,expiretime) == 0) goto werr;
            }

            /* Update info every 1 second (approximately).
             * in order to avoid calling mstime() on each iteration, we will
             * check the diff every 1024 keys */
            if ((key_count++ & 1023) == 0) {
                long long now = mstime();
                if (now - updated_time >= 1000) {
                    sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, "AOF rewrite");
                    updated_time = now;
                }
            }
        }
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp = NULL;
    char tmpfile[256];

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground(int opennew) function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }

    rioInitWithFile(&aof,fp);

    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);

    startSaving(RDBFLAGS_AOF_PREAMBLE);

    if (server.aof_use_rdb_preamble) {
        int error;
        if (rdbSaveRio(&aof,&error,RDBFLAGS_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp)) goto werr;
    if (fsync(fileno(fp))) goto werr;
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    serverLog(LL_NOTICE,"SYNC append only file rewrite performed");
    stopSaving(1);

    /* Delay return if required (for testing) */
    if (server.aof_child_rewrite_delay)
        debugDelay(server.aof_child_rewrite_delay);

    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}
/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * ------------------------------------------------------------------------- */

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
int rewriteAppendOnlyFileBackground(int opennew) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;
    if ((childpid = redisFork(CHILD_TYPE_AOF)) == 0) {
        char tmpfile[256];

        /* Child */
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %ld",(long) childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);

        /* We set aof_selected_db to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        server.aof_selected_db = -1;
        replicationScriptCacheFlush();
        flushAppendOnlyFile(1);
        if (opennew) openNewIncrAofForAppend();
        return C_OK;
    }
    return C_OK; /* unreached */
}

void bgrewriteaofCommand(client *c) {
    if (server.child_type == CHILD_TYPE_AOF) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (hasActiveChildProcess()) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground(server.aof_state == AOF_ON) == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReplyError(c,"Can't execute an AOF background rewriting. "
                        "Please check the server logs for more information.");
    }
}

void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    bg_unlink(tmpfile);

    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) childpid);
    bg_unlink(tmpfile);
}

off_t getAppendOnlyFileSize(const char *filename) {
    struct redis_stat sb;
    off_t size;
    mstime_t latency;

    latencyStartMonitor(latency);
    if (redis_stat(filename,&sb) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the AOF file %s length. stat: %s",
            filename,strerror(errno));
        size = 0;
    } else {
        size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat",latency);
    return size;
}

off_t getBaseAppendOnlyFileSize(void) {
    serverAssert(server.aof_meta != NULL);
    char *base_aof_name;

    if (server.aof_meta->base_aof_info == NULL) {
        return 0;
    } 

    base_aof_name = server.aof_meta->base_aof_info->file_name;
    return getAppendOnlyFileSize(base_aof_name);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        char tmpfile[256];
        long long now = ustime();
        const char *new_base_aof_name;
        aofMeta *tmpmeta;
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.child_pid);

        serverAssert(server.aof_meta != NULL);

        /* Dup a temporary aof_meta for subsequent modifications. */
        tmpmeta = aofMetaDup(server.aof_meta);
        if (tmpmeta == NULL) {
            goto cleanup;
        }

        /* Get a new BASE type AOF name, and mark the previous (if we have) BASE AOF as the HIST type. */
        new_base_aof_name = getNewBaseAofNameAndMarkPreAsHistory(tmpmeta);
        serverAssert(new_base_aof_name != NULL);
        /* Rename the temporary aof file to new_base_aof_name. */
        latencyStartMonitor(latency);
        if (rename(tmpfile,new_base_aof_name) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile,
                new_base_aof_name,
                strerror(errno));
            aofMetaFree(tmpmeta);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename",latency);

        /* Change the AOF file type in `incr_aof_list` from AOF_FILE_TYPE_INCR 
         * to AOF_FILE_TYPE_HIST, and move them to the `history_aof_list`. */
        markRewrittenIncrAofAsHistory(tmpmeta);
        if (persistAofMeta(tmpmeta) == C_ERR) {
            aofMetaFree(tmpmeta);
            goto cleanup;
        }
    
        if (server.aof_fd != -1) {
            /* AOF enabled. */
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            server.aof_current_size = getBaseAppendOnlyFileSize() + server.aof_newfile_size;
            server.aof_rewrite_base_size = server.aof_current_size;
            server.aof_fsync_offset = server.aof_current_size;
            server.aof_last_fsync = server.unixtime;
        }

        /* We can safely let server.aof_meta point to tmpmeta and free the previous aof_meta. */
        aofMetaFree(server.aof_meta);
        server.aof_meta = tmpmeta;

        server.aof_lastbgrewrite_status = C_OK;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1)
            server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofRemoveTempFile(server.child_pid);
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
