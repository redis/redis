/*
 * User Access Control List
 *
 * Copyright (c) 2018, DaeMyung Kang <charsyam at naver dot com>
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

/*
 * redisCommmands has aclid
 * and client has acls to allow using each command.
 * it consists of four 8 bytes. if the bit is 1, client can use it
 * if it is 0, client can't use it
 * for ex, aclid of module command is 0, so 0th bit of acls is set.
 * client can use it
 */

struct ACLGroup aclGroups[] = {
    {"readonly", CMD_READONLY, 0, 0, {0, 0, 0, 0}},
    {"write", CMD_WRITE, 0, 0, {0, 0, 0, 0}},
    {"slow", CMD_READONLY|CMD_WRITE, CMD_FAST, 0, {0, 0, 0, 0}},
    {"admin", CMD_ADMIN, 0, 0, {0, 0, 0, 0}},
    {"string", 0, 0, CMD_TYPE_STRING, {0, 0, 0, 0}},
    {"list", 0, 0, CMD_TYPE_LIST, {0, 0, 0, 0}},
    {"set", 0, 0, CMD_TYPE_SET, {0, 0, 0, 0}},
    {"zset", 0, 0, CMD_TYPE_ZSET, {0, 0, 0, 0}},
    {"hash", 0, 0, CMD_TYPE_HASH, {0, 0, 0, 0}},
    {"hyperloglog", 0, 0, CMD_TYPE_HYPERLOGLOG, {0, 0, 0, 0}},
    {"scan", 0, 0, CMD_TYPE_SCAN, {0, 0, 0, 0}},
    {"pubsub", 0, 0, CMD_TYPE_PUBSUB, {0, 0, 0, 0}},
    {"transaction", 0, 0, CMD_TYPE_TRANSACTION, {0, 0, 0, 0}},
    {"scripting", 0, 0, CMD_TYPE_SCRIPTING, {0, 0, 0, 0}},
    {"geo", 0, 0, CMD_TYPE_GEO, {0, 0, 0, 0}},
    {"stream", 0, 0, CMD_TYPE_STREAM, {0, 0, 0, 0}},
    {"all", 0, 0, 0, {0, 0, 0, 0}}
};

static void userACLDestructure(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    userACL *user = (userACL *)val;

    sdsfree(user->name);
    sdsfree(user->passwd);
    zfree(user);
}

/* user acl table, sds string -> userACL struct pointer. */
dictType userACLDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    NULL,                       /* key destructor */
    userACLDestructure          /* val destructor */
};

int getNumberOfACLGroups() {
    return (sizeof(aclGroups)/sizeof(struct ACLGroup));
}

void initACLs(ACLPermission acls) {
    memset(acls, 0, sizeof(ACLPermission));
}

void setACLs(ACLPermission tar, ACLPermission src) {
    memcpy(tar, src, sizeof(ACLPermission));
}

static void addACLs(ACLPermission src, ACLPermission tar) {
    for (int i = 0; i < ACL_ARRAY_NUM; i++) {
        src[i] |= tar[i];
    }
}

static void removeACLs(ACLPermission src, ACLPermission tar) {
    acl_t v = 0;
    for (int i = 0; i < ACL_ARRAY_NUM; i++) {
        v = ~tar[i];
        src[i] &= v;
    }
}

static int getACLsFromCommand(char *cmdName, ACLPermission acls) {
    struct redisCommand *cmd;

    sds name = sdsnew(cmdName);
    cmd = lookupCommandOrOriginal(name);
    sdsfree(name);
    if (cmd == NULL) {
        return 0;
    }

    acls[CMD_ACL_INDEX(cmd->aclid)] = CMD_ACL_VALUE(cmd->aclid);
    return 1;
}

static int getACLsFromGroup(char *groupName, ACLPermission acls) {
    int numgroups = getNumberOfACLGroups();

    for (int i = 0; i < numgroups; i++) {
        ACLGroup *acl = aclGroups + i;

        if (strcmp(acl->name, groupName) == 0) {
            setACLs(acls, acl->acls);
            return 1;
        }
    }

    return 0;
}

static int parseACL(int argc, char *argv[], ACLPermission acls, int dryrun) {
    int op;
    int group;

    if (dryrun == 0) {
        memset(acls, 0, sizeof(ACLPermission));
    }

    char *cmdName;
    char *acl;

    for (int i = 0; i < argc; i++) {
        ACLPermission tmpACLs;
        initACLs(tmpACLs);

        acl = argv[2+i];

        if (acl[0] != '+' && acl[0] != '-') {
            serverLog(LL_WARNING, "Fatal error, acl rule has to start + or - %s", acl);
            return -1;
        }

        if (acl[0] == '+') {
            op = 1;
        } else {
            op = 0;
        }

        if (acl[1] == '#') {
            group = 1;
            cmdName = &acl[2];
        } else {
            group = 0;
            cmdName = &acl[1];
        }

        if (group) {
            if (getACLsFromGroup(cmdName, tmpACLs) == -1) {
                serverLog(LL_WARNING, "Fatal error, group is not exist: %s", cmdName);
                return -1;
            }
        } else {
            if (getACLsFromCommand(cmdName, tmpACLs) == -1) {
                serverLog(LL_WARNING, "Fatal error, command is not exist: %s", cmdName);
                return -1;
            }
        }

        if (dryrun == 0) {
            if (op) {
                addACLs(acls, tmpACLs);
            } else {
                removeACLs(acls, tmpACLs);
            }
        }
    }

    return 1;
}

static void addUserACL(char *name, char *passwd, ACLPermission acls, dict *acls_dict) {
    sds key = sdsnew(name);
    dictEntry *de = dictFind(acls_dict,key);
    if (de != NULL) {
        sdsfree(key);
        return;
    }

    userACL *user = zmalloc(sizeof(*user));
    user->name = key;
    user->passwd = sdsnew(passwd);
    setACLs(user->acls, acls);
    dictAdd(acls_dict, user->name, user);
}

static void reloadACLs(ACLPermission default_acls, dict *acls_dict) {
    dict *old = server.acls;

    setACLs(server.default_acls, default_acls);
    server.acls = acls_dict;

    dictRelease(old);
}

static int parse(char *acls, int dryrun) {
    char *err = NULL;
    int linenum = 0, totlines, i;
    sds *lines;

    lines = sdssplitlen(acls,strlen(acls),"\n",1,&totlines);
    dict *tmpACLsDict = dictCreate(&userACLDictType,NULL);
    ACLPermission default_acls;

    for (i = 0; i < totlines; i++) {
        sds *argv;
        int argc;

        linenum = i+1;
        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip comments and blank lines */
        if (lines[i][0] == '#' || lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitargs(lines[i],&argc);
        if (argv == NULL) {
            err = "Unbalanced quotes in configuration line";
            goto loaderr;
        }

        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        sdstolower(argv[0]);

        if (argc < 2) {
            err = "params count is more than 2 in configuration line";
            goto loaderr;
        }

        if (parseACL(argc-2, argv, NULL, 1) == -1) {
            goto loaderr;
        }

        if (dryrun == 0) {
            ACLPermission acls;
            parseACL(argc-2, argv, acls, 0);

            if (strcmp(argv[0], ACL_DEFAULT_USER_NAME) == 0) {
                if (strlen(argv[1]) != 0) {
                    serverLog(LL_WARNING, "%s passwd setting will be ignored", ACL_DEFAULT_USER_NAME);
                }
                setACLs(default_acls, acls);
            } else {
                addUserACL(argv[0], argv[1], acls, tmpACLsDict);
            }
        }
        sdsfreesplitres(argv,argc);
    }

    sdsfreesplitres(lines, totlines);
    reloadACLs(default_acls, tmpACLsDict);
    return 0;

loaderr:
    if (tmpACLsDict) {
        dictRelease(tmpACLsDict);
    }

    serverLog(LL_WARNING, "Fatal error, user-file is NULL\n"
            "Reading the acls file, at line %d\n"
            ">>> '%s'\n%s\n"
            ,linenum, lines[i], err);
    return -1;
}

int loadACLs(const char *filename) {
    if (filename == NULL) {
        serverLog(LL_WARNING, "Fatal error, user-file is NULL");
        return -1;
    }

    sds acls = sdsempty();
    char buf[CONFIG_MAX_LINE+1];

    /* Load the file content */
    if (filename) {
        FILE *fp;

        if ((fp = fopen(filename,"r")) == NULL) {
            serverLog(LL_WARNING,
                    "Fatal error, can't open config file '%s'", filename);
                return -1;
        }

        while(fgets(buf,CONFIG_MAX_LINE+1,fp) != NULL) {
            acls = sdscat(acls,buf);
        }

        fclose(fp);
    }

    //dryrun to check user-file
    if (parse(acls, 1) == -1) {
        return -1;
    }

    parse(acls, 0);
    sdsfree(acls);
    return 0;
}

userACL *getUserACL(char *userName) {
    sds name = sdsnew(userName);
    userACL *user = dictFetchValue(server.acls, name);
    sdsfree(name);
    return user;
}
