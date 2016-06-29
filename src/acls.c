#include "server.h"
#include "zmalloc.h"

typedef struct aclGroupInfo {
    char *name;
    int flag;
} aclGroupInfo;

aclGroupInfo aclGroupInfos[] = {
    {"readonly" , CMD_READONLY},
    {"write"    , CMD_WRITE   },
    {"slow"     , CMD_FAST    },
    {"admin"    , CMD_ADMIN   },
    {"pubsub"   , CMD_PUBSUB  },
    {"scripting", CMD_NOSCRIPT},
    {"all"      , CMD_ALL     }
};

int NUMGROUPS = sizeof(aclGroupInfos)/sizeof(struct aclGroupInfo);

int getAllAclsIndex() {
    return NUMGROUPS - 1;
}

void addCommandToAclGroup(aclGroup *aclGroups, int flags, int index, acl_t value) {
    aclGroupInfo *info;
    for (int i = 0; i < NUMGROUPS - 1; i++) {
        info = aclGroupInfos + i;
        if (flags & info->flag) {
            aclGroups[i].acls[index] |= value;
        }
    }

    aclGroups[getAllAclsIndex()].acls[index] |= value;
}

static void initAcls(acl_t *acls) {
    memset(acls, 0, sizeof(acl_t) * server.acls_array_size);
}

static acl_t *createAcls() {
    acl_t *acls = (acl_t *)zmalloc(sizeof(acl_t) * server.acls_array_size);
    initAcls(acls);
    return acls;
}

static void setAcls(acl_t *tar, acl_t *src) {
    memcpy(tar, src, sizeof(acl_t) * server.acls_array_size);
}

static void addAcls(acl_t *src, acl_t *tar) {
    for (int i = 0; i < server.acls_array_size; i++) {
        src[i] |= tar[i];
    }
}

static void removeAcls(acl_t *src, acl_t *tar) {
    acl_t v = 0;

    for (int i = 0; i < server.acls_array_size; i++) {
        v = ~tar[i];
        src[i] &= v;
    }
}

static int getAclsFromCommand(char *cmdName, int *index, acl_t *value) {
    struct redisCommand *cmd;

    sds name = sdsnew(cmdName);
    cmd = lookupCommandOrOriginal(name);
    sdsfree(name);
    if (cmd == NULL) {
        return 0;
    }

    *index = cmd->aclindex;
    *value = cmd->aclvalue;
    return 1;
}

static int getAclsFromGroup(aclGroup *aclGroups, char *groupName, acl_t *acls) {
    for (int i = 0; i < NUMGROUPS; i++) {
        aclGroup *acl = aclGroups + i;
    
        if (strcmp(acl->name, groupName) == 0) {
            setAcls(acls, acl->acls);
            return i;
        }
    }

    return -1;
}

static int parseAcl(aclGroup *aclGroups, int argc, char *argv[], userAcl *user) {
    int ret;
    acl_t *acls = user->acls;
    aclGroupInfo *info;

    initAcls(acls);

    acl_t *tmpAcls = createAcls();
    for (int i = 0; i < argc; i++) {
        int op = 0;
        int group = 0;

        char *acl = argv[2+i];
        char *cmdName = &acl[1];

        initAcls(tmpAcls);

        if (acl[0] == '+') {
            op = 1;
        } else {
            op = 0;
        }

        if (acl[1] == '#') {
            group = 1;
            cmdName = &acl[2];
        }
    
        if (group) {
            ret = getAclsFromGroup(aclGroups, cmdName, tmpAcls);
            if (ret >= 0) {
                info = aclGroupInfos + ret;
                user->groupFlags |= info->flag;
                if (op) {
                    user->groupOpFlags |= info->flag;
                    addAcls(acls, tmpAcls);
                } else {
                    removeAcls(acls, tmpAcls);
                }
            }
        } else {
            int index;
            acl_t value;

            if (getAclsFromCommand(cmdName, &index, &value) == 1) {
                if (op) {
                    addAcl(acls, index, value);
                } else {
                    removeAcl(acls, index, value);
                }
            }
        }

    }

    zfree(tmpAcls);
    return 1;
}

static void addUserAcl(userAcl *user) {
    dictAdd(server.acls, sdsnew(user->name), user);
}

static userAcl *createUserAcls(char *name, char *passwd) {
    userAcl *user = zmalloc(sizeof(*user));
    user->name = zstrdup(name);
    user->passwd = zstrdup(passwd);
    user->acls = createAcls();
    user->groupFlags = 0;
    user->groupOpFlags = 0;
    return user;
}

static void parse(aclGroup *aclGroups, char *acls) {
    char *err = NULL;
    int linenum = 0, totlines, i;
    sds *lines;

    lines = sdssplitlen(acls,strlen(acls),"\n",1,&totlines);

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

        userAcl *user = createUserAcls(argv[0], argv[1]);
        parseAcl(aclGroups, argc-2, argv, user);
        addUserAcl(user);
    }

    return;

loaderr:
    fprintf(stderr, "\n*** FATAL ACLS FILE ERROR ***\n");
    fprintf(stderr, "Reading the acls file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", lines[i]);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

int loadAcls(aclGroup *aclGroups, const char *filename) {
    sds acls = sdsempty();
    char buf[CONFIG_MAX_LINE+1];
   
    server.default_acls = NULL;

    /* Load the file content */
    if (filename) {
        FILE *fp;

        if (filename[0] == '-' && filename[1] == '\0') {
            fp = stdin;
        } else {
            if ((fp = fopen(filename,"r")) == NULL) {
                serverLog(LL_WARNING,
                    "Fatal error, can't open acl file '%s'", filename);
                exit(1);
            }
        }
        while(fgets(buf,CONFIG_MAX_LINE+1,fp) != NULL)
            acls = sdscat(acls,buf);
        if (fp != stdin) fclose(fp);
    }

    parse(aclGroups, acls);
    userAcl *defaultUser = getUserAcl(ACL_DEFAULT_USER_NAME);
    if (defaultUser) {
        server.default_acls = defaultUser->acls;
    } else {
        server.default_acls = aclGroups[getAllAclsIndex()].acls;
    }

    return 0;
}

userAcl *getUserAcl(char *userName) {
    sds name = sdsnew(userName);
    userAcl *user = dictFetchValue(server.acls, name);
    sdsfree(name);
    return user;
}

int increaseAclCmdSize() {
    int i = server.acl_cmd_max_size;
    server.acl_cmd_max_size++;
    return i;
}

aclGroup *createAclGroups() {
    aclGroupInfo *info;
    aclGroup *aclGroups = (aclGroup *)zmalloc(sizeof(aclGroup) * NUMGROUPS);

    for (int i = 0; i < NUMGROUPS; i++) {
        aclGroup *group = aclGroups + i;
        info = aclGroupInfos + i;
        group->name = info->name;
        group->acls = createAcls();
    }

    return aclGroups;
}

void addAcl(acl_t *acls, int index, acl_t value) {
    acls[index] |= value; 
}

void removeAcl(acl_t *acls, int index, acl_t value) {
    acls[index] &= (~value);
}

static void reloadGroupAcls(aclGroup *aclGroups, struct userAcl *user) {
    aclGroupInfo *info;
    int op;

    acl_t *acls = user->acls;

    for (int i = 0; i < NUMGROUPS; i++) {
        info = aclGroupInfos + i;
        if (user->groupFlags & info->flag) {
            op = ((user->groupOpFlags & info->flag) != 0);
            aclGroup *acl = aclGroups + i;
            if (op == 1) {
                addAcls(acls, acl->acls);
            } else {
                removeAcls(acls, acl->acls);
            }        
        }
    }
}

void reloadUserAcls() {
    if (server.use_cmd_acls == 0) {
        return;
    }

    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(server.acls);
    while ((de = dictNext(di)) != NULL) {
        struct userAcl *user = (struct userAcl *)dictGetVal(de);
        reloadGroupAcls(server.aclGroups, user);
    }
    dictReleaseIterator(di);
}
