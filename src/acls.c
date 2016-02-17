#include "server.h"
#include "zmalloc.h"

aclGroup aclGroups[] = {
    {"readonly", {0, 0, 0, 0}},
    {"write", {0, 0, 0, 0}},
    {"slow", {0, 0, 0, 0}},
    {"admin", {0, 0, 0, 0}},
    {"pubsub", {0, 0, 0, 0}},
    {"scripting", {0, 0, 0, 0}},
    {"all", {0, 0, 0, 0}}
};

void initAcls(acl_t acls[ACL_ARRAY_NUM]) {
    memset(acls, 0, sizeof(acl_t) * ACL_ARRAY_NUM);
}

void setAcls(acl_t tar[ACL_ARRAY_NUM], acl_t src[ACL_ARRAY_NUM]) {
    memcpy(tar, src, sizeof(acl_t) * ACL_ARRAY_NUM);
}

static void addAcls(acl_t src[ACL_ARRAY_NUM], acl_t tar[ACL_ARRAY_NUM]) {
    for (int i = 0; i < ACL_ARRAY_NUM; i++) {
        src[i] |= tar[i];
    }
}

static void removeAcls(acl_t src[ACL_ARRAY_NUM], acl_t tar[ACL_ARRAY_NUM]) {
    acl_t v = 0;

    for (int i = 0; i < ACL_ARRAY_NUM; i++) {
        v = ~tar[i];
        src[i] &= v;
    }
}

static int getAclsFromCommand(char *cmdName, acl_t acls[ACL_ARRAY_NUM]) {
    struct redisCommand *cmd;

    sds name = sdsnew(cmdName);
    cmd = lookupCommandOrOriginal(name);
    sdsfree(name);
    if (cmd == NULL) {
        return 0;
    }

    acls[cmd->aclindex] = cmd->aclvalue;
    return 1;
}

static int getAclsFromGroup(char *groupName, acl_t acls[ACL_ARRAY_NUM]) {
    int numgroups = sizeof(aclGroups)/sizeof(struct aclGroup);

    for (int i = 0; i < numgroups; i++) {
        aclGroup *acl = aclGroups + i;
    
        if (strcmp(acl->name, groupName) == 0) {
            setAcls(acls, acl->acls);
            return 1;
        }
    }

    return 0;
}

static int parseAcl(int argc, char *argv[], acl_t acls[ACL_ARRAY_NUM]) {
    acls[0] = 0;
    acls[1] = 0;
    acls[2] = 0;
    acls[3] = 0;

    for (int i = 0; i < argc; i++) {
        int op = 0;
        int group = 0;

        acl_t tmpAcls[ACL_ARRAY_NUM];
        initAcls(tmpAcls);

        char *acl = argv[2+i];
        char *cmdName = &acl[1];

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
            getAclsFromGroup(cmdName, tmpAcls);
        } else {
            getAclsFromCommand(cmdName, tmpAcls);
        }

        if (op) {
            addAcls(acls, tmpAcls);
        } else {
            removeAcls(acls, tmpAcls);
        }
    }

    return 1;
}

static void addUserAcl(userAcl *user) {
    dictAdd(server.acls, sdsnew(user->name), user);
}

static void parse(char *acls) {
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

        userAcl *user = zmalloc(sizeof(*user));
        user->name = zstrdup(argv[0]);
        user->passwd = zstrdup(argv[1]);
        parseAcl(argc-2, argv, user->acls);
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

int loadAcls(const char *filename) {
    sds acls = sdsempty();
    char buf[CONFIG_MAX_LINE+1];

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

    parse(acls);
    userAcl *defaultUser = getUserAcl(ACL_DEFAULT_USER_NAME);
    if (defaultUser) {
        setAcls(server.default_acls, defaultUser->acls);
    }

    return 0;
}

userAcl *getUserAcl(char *userName) {
    sds name = sdsnew(userName);
    userAcl *user = dictFetchValue(server.acls, name);
    sdsfree(name);
    return user;
}
