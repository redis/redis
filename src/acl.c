/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* =============================================================================
 * Global state for ACLs
 * ==========================================================================*/

rax *Users; /* Table mapping usernames to user structures. */
user *DefaultUser;   /* Global reference to the default user.
                        Every new connection is associated to it, if no
                        AUTH or HELLO is used to authenticate with a
                        different user. */

struct ACLCategoryItem {
    const char *name;
    uint64_t flag;
} ACLCommandCategories[] = {
    {"keyspace", CMD_CATEGORY_KEYSPACE},
    {"read", CMD_CATEGORY_READ},
    {"write", CMD_CATEGORY_WRITE},
    {"set", CMD_CATEGORY_SET},
    {"sortedset", CMD_CATEGORY_SORTEDSET},
    {"list", CMD_CATEGORY_LIST},
    {"hash", CMD_CATEGORY_HASH},
    {"string", CMD_CATEGORY_STRING},
    {"bitmap", CMD_CATEGORY_BITMAP},
    {"hyperloglog", CMD_CATEGORY_HYPERLOGLOG},
    {"geo", CMD_CATEGORY_GEO},
    {"stream", CMD_CATEGORY_STREAM},
    {"pubsub", CMD_CATEGORY_PUBSUB},
    {"admin", CMD_CATEGORY_ADMIN},
    {"fast", CMD_CATEGORY_FAST},
    {"slow", CMD_CATEGORY_SLOW},
    {"blocking", CMD_CATEGORY_BLOCKING},
    {"dangerous", CMD_CATEGORY_DANGEROUS},
    {"connection", CMD_CATEGORY_CONNECTION},
    {"transaction", CMD_CATEGORY_TRANSACTION},
    {"scripting", CMD_CATEGORY_SCRIPTING},
    {"",0} /* Terminator. */
};

/* =============================================================================
 * Helper functions for the rest of the ACL implementation
 * ==========================================================================*/

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function.
 *
 * Note that limiting the comparison length to strings up to 512 bytes we
 * can avoid leaking any information about the password length and any
 * possible branch misprediction related leak.
 */
int time_independent_strcmp(char *a, char *b) {
    char bufa[CONFIG_AUTHPASS_MAX_LEN], bufb[CONFIG_AUTHPASS_MAX_LEN];
    /* The above two strlen perform len(a) + len(b) operations where either
     * a or b are fixed (our password) length, and the difference is only
     * relative to the length of the user provided string, so no information
     * leak is possible in the following two lines of code. */
    unsigned int alen = strlen(a);
    unsigned int blen = strlen(b);
    unsigned int j;
    int diff = 0;

    /* We can't compare strings longer than our static buffers.
     * Note that this will never pass the first test in practical circumstances
     * so there is no info leak. */
    if (alen > sizeof(bufa) || blen > sizeof(bufb)) return 1;

    memset(bufa,0,sizeof(bufa));        /* Constant time. */
    memset(bufb,0,sizeof(bufb));        /* Constant time. */
    /* Again the time of the following two copies is proportional to
     * len(a) + len(b) so no info is leaked. */
    memcpy(bufa,a,alen);
    memcpy(bufb,b,blen);

    /* Always compare all the chars in the two buffers without
     * conditional expressions. */
    for (j = 0; j < sizeof(bufa); j++) {
        diff |= (bufa[j] ^ bufb[j]);
    }
    /* Length must be equal as well. */
    diff |= alen ^ blen;
    return diff; /* If zero strings are the same. */
}

/* =============================================================================
 * Low level ACL API
 * ==========================================================================*/

/* Given the category name the command returns the corresponding flag, or
 * zero if there is no match. */
uint64_t ACLGetCommandCategoryFlagByName(const char *name) {
    for (int j = 0; ACLCommandCategories[j].flag != 0; j++) {
        if (!strcasecmp(name,ACLCommandCategories[j].name)) {
            return ACLCommandCategories[j].flag;
        }
    }
    return 0; /* No match. */
}

/* Method for passwords/pattern comparison used for the user->passwords list
 * so that we can search for items with listSearchKey(). */
int ACLListMatchSds(void *a, void *b) {
    return sdscmp(a,b) == 0;
}

/* Create a new user with the specified name, store it in the list
 * of users (the Users global radix tree), and returns a reference to
 * the structure representing the user.
 *
 * If the user with such name already exists NULL is returned. */
user *ACLCreateUser(const char *name, size_t namelen) {
    if (raxFind(Users,(unsigned char*)name,namelen) != raxNotFound) return NULL;
    user *u = zmalloc(sizeof(*u));
    u->name = sdsnewlen(name,namelen);
    u->flags = 0;
    u->allowed_subcommands = NULL;
    u->passwords = listCreate();
    u->patterns = listCreate();
    listSetMatchMethod(u->passwords,ACLListMatchSds);
    listSetMatchMethod(u->patterns,ACLListMatchSds);
    memset(u->allowed_commands,0,sizeof(u->allowed_commands));
    raxInsert(Users,(unsigned char*)name,namelen,u,NULL);
    return u;
}

/* Given a command ID, this function set by reference 'word' and 'bit'
 * so that user->allowed_commands[word] will address the right word
 * where the corresponding bit for the provided ID is stored, and
 * so that user->allowed_commands[word]&bit will identify that specific
 * bit. The function returns C_ERR in case the specified ID overflows
 * the bitmap in the user representation. */
int ACLGetCommandBitCoordinates(unsigned long id, uint64_t *word, uint64_t *bit) {
    if (id >= USER_COMMAND_BITS_COUNT) return C_ERR;
    *word = id / sizeof(uint64_t) / 8;
    *bit = 1 << (id % (sizeof(uint64_t) * 8));
    return C_OK;
}

/* Check if the specified command bit is set for the specified user.
 * The function returns 1 is the bit is set or 0 if it is not.
 * Note that this function does not check the ALLCOMMANDS flag of the user
 * but just the lowlevel bitmask.
 *
 * If the bit overflows the user internal represetation, zero is returned
 * in order to disallow the execution of the command in such edge case. */
int ACLGetUserCommandBit(user *u, unsigned long id) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return 0;
    return u->allowed_commands[word] & bit;
}

/* When +@all or allcommands is given, we set a reserved bit as well that we
 * can later test, to see if the user has the right to execute "future commands",
 * that is, commands loaded later via modules. */
int ACLUserCanExecuteFutureCommands(user *u) {
    return ACLGetUserCommandBit(u,USER_COMMAND_BITS_COUNT-1);
}

/* Set the specified command bit for the specified user to 'value' (0 or 1).
 * If the bit overflows the user internal represetation, no operation
 * is performed. */
void ACLSetUserCommandBit(user *u, unsigned long id, int value) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return;
    if (value)
        u->allowed_commands[word] |= bit;
    else
        u->allowed_commands[word] &= ~bit;
}

/* Get a command from the original command table, that is not affected
 * by the command renaming operations: we base all the ACL work from that
 * table, so that ACLs are valid regardless of command renaming. */
struct redisCommand *ACLLookupCommand(const char *name) {
    struct redisCommand *cmd;
    sds sdsname = sdsnew(name);
    cmd = dictFetchValue(server.orig_commands, sdsname);
    sdsfree(sdsname);
    return cmd;
}

/* Flush the array of allowed subcommands for the specified user
 * and command ID. */
void ACLResetSubcommandsForCommand(user *u, unsigned long id) {
    if (u->allowed_subcommands && u->allowed_subcommands[id]) {
        zfree(u->allowed_subcommands[id]);
        u->allowed_subcommands[id] = NULL;
    }
}

/* Set user properties according to the string "op". The following
 * is a description of what different strings will do:
 *
 * on           Enable the user: it is possible to authenticate as this user.
 * off          Disable the user: it's no longer possible to authenticate
 *              with this user, however the already authenticated connections
 *              will still work.
 * +<command>   Allow the execution of that command
 * -<command>   Disallow the execution of that command
 * +@<category> Allow the execution of all the commands in such category
 *              with valid categories are like @admin, @set, @sortedset, ...
 *              and so forth, see the full list in the server.c file where
 *              the Redis command table is described and defined.
 *              The special category @all means all the commands, but currently
 *              present in the server, and that will be loaded in the future
 *              via modules.
 * +<command>|subcommand    Allow a specific subcommand of an otherwise
 *                          disabled command. Note that this form is not
 *                          allowed as negative like -DEBUG|SEGFAULT, but
 *                          only additive starting with "+".
 * allcommands  Alias for +@all. Note that it implies the ability to execute
 *              all the future commands loaded via the modules system.
 * nocommands   Alias for -@all.
 * ~<pattern>   Add a pattern of keys that can be mentioned as part of
 *              commands. For instance ~* allows all the keys. The pattern
 *              is a glob-style pattern like the one of KEYS.
 *              It is possible to specify multiple patterns.
 * allkeys      Alias for ~*
 * resetkeys    Flush the list of allowed keys patterns.
 * ><password>  Add this passowrd to the list of valid password for the user.
 *              For example >mypass will add "mypass" to the list.
 *              This directive clears the "nopass" flag (see later).
 * <<password>  Remove this password from the list of valid passwords.
 * nopass       All the set passwords of the user are removed, and the user
 *              is flagged as requiring no password: it means that every
 *              password will work against this user. If this directive is
 *              used for the default user, every new connection will be
 *              immediately authenticated with the default user without
 *              any explicit AUTH command required. Note that the "resetpass"
 *              directive will clear this condition.
 * resetpass    Flush the list of allowed passwords. Moreover removes the
 *              "nopass" status. After "resetpass" the user has no associated
 *              passwords and there is no way to authenticate without adding
 *              some password (or setting it as "nopass" later).
 * reset        Performs the following actions: resetpass, resetkeys, off,
 *              -@all. The user returns to the same state it has immediately
 *              after its creation.
 *
 * The 'op' string must be null terminated. The 'oplen' argument should
 * specify the length of the 'op' string in case the caller requires to pass
 * binary data (for instance the >password form may use a binary password).
 * Otherwise the field can be set to -1 and the function will use strlen()
 * to determine the length.
 *
 * The function returns C_OK if the action to perform was understood because
 * the 'op' string made sense. Otherwise C_ERR is returned if the operation
 * is unknown or has some syntax error.
 *
 * When an error is returned, errno is set to the following values:
 *
 * EINVAL: The specified opcode is not understood.
 * ENOENT: The command name provided with + or - is not known.
 */
int ACLSetUser(user *u, const char *op, ssize_t oplen) {
    if (oplen == -1) oplen = strlen(op);
    if (!strcasecmp(op,"on")) {
        u->flags |= USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"off")) {
        u->flags &= ~USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"allkeys") ||
               !strcasecmp(op,"~*"))
    {
        u->flags |= USER_FLAG_ALLKEYS;
        listEmpty(u->patterns);
    } else if (!strcasecmp(op,"resetkeys")) {
        u->flags &= ~USER_FLAG_ALLKEYS;
        listEmpty(u->patterns);
    } else if (!strcasecmp(op,"allcommands") ||
               !strcasecmp(op,"+@all"))
    {
        memset(u->allowed_commands,255,sizeof(u->allowed_commands));
        u->flags |= USER_FLAG_ALLCOMMANDS;
    } else if (!strcasecmp(op,"nocommands") ||
               !strcasecmp(op,"-@all"))
    {
        memset(u->allowed_commands,0,sizeof(u->allowed_commands));
        u->flags &= ~USER_FLAG_ALLCOMMANDS;
    } else if (!strcasecmp(op,"nopass")) {
        u->flags |= USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (!strcasecmp(op,"resetpass")) {
        u->flags &= ~USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (op[0] == '>') {
        sds newpass = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->passwords,newpass);
        /* Avoid re-adding the same password multiple times. */
        if (ln == NULL) listAddNodeTail(u->passwords,newpass);
        u->flags &= ~USER_FLAG_NOPASS;
    } else if (op[0] == '<') {
        sds delpass = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->passwords,delpass);
        if (ln) listDelNode(u->passwords,ln);
        sdsfree(delpass);
    } else if (op[0] == '~') {
        sds newpat = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->patterns,newpat);
        /* Avoid re-adding the same pattern multiple times. */
        if (ln == NULL) listAddNodeTail(u->patterns,newpat);
        u->flags &= ~USER_FLAG_ALLKEYS;
    } else if (op[0] == '+' && op[1] != '@') {
        if (strchr(op,'|') == NULL) {
            if (ACLLookupCommand(op+1) == NULL) {
                errno = ENOENT;
                return C_ERR;
            }
            unsigned long id = ACLGetCommandID(op+1);
            ACLSetUserCommandBit(u,id,1);
            ACLResetSubcommandsForCommand(u,id);
        } else {
            /* Split the command and subcommand parts. */
            char *copy = zstrdup(op+1);
            char *sub = strchr(copy,'|');
            sub[0] = '\0';
            sub++;

            /* Check if the command exists. We can't check the
             * subcommand to see if it is valid. */
            if (ACLLookupCommand(copy) == NULL) {
                errno = ENOENT;
                return C_ERR;
            }
            unsigned long id = ACLGetCommandID(copy);

            /* The subcommand cannot be empty, so things like DEBUG|
             * are syntax errors of course. */
            if (strlen(sub) == 0) {
                zfree(copy);
                errno = EINVAL;
                return C_ERR;
            }

            /* If this is the first subcommand to be configured for
             * this user, we have to allocate the subcommands array. */
            if (u->allowed_subcommands == NULL) {
                u->allowed_subcommands = zcalloc(USER_COMMAND_BITS_COUNT *
                                         sizeof(sds*));
            }

            /* We also need to enlarge the allocation pointing to the
             * null terminated SDS array, to make space for this one. */
            long items = 0;
            if (u->allowed_subcommands[id]) {
                while(u->allowed_subcommands[items]) items++;
            }

            items += 2; /* Make space for the new item and the null term. */
            u->allowed_subcommands[id] = zrealloc(u->allowed_subcommands[id],
                                         sizeof(sds)*items);
            u->allowed_subcommands[id][items-2] = sdsnew(sub);
            u->allowed_subcommands[id][items-1] = NULL;

            /* We have to clear the command bit so that we force the
             * subcommand check. */
            ACLSetUserCommandBit(u,id,0);
            zfree(copy);
        }
    } else if (op[0] == '-' && op[1] != '@') {
        if (ACLLookupCommand(op+1) == NULL) {
            errno = ENOENT;
            return C_ERR;
        }
        unsigned long id = ACLGetCommandID(op+1);
        ACLSetUserCommandBit(u,id,0);
        u->flags &= ~USER_FLAG_ALLCOMMANDS;
        ACLResetSubcommandsForCommand(u,id);
    } else if (!strcasecmp(op,"reset")) {
        serverAssert(ACLSetUser(u,"resetpass",-1) == C_OK);
        serverAssert(ACLSetUser(u,"resetkeys",-1) == C_OK);
        serverAssert(ACLSetUser(u,"off",-1) == C_OK);
        serverAssert(ACLSetUser(u,"-@all",-1) == C_OK);
    } else {
        errno = EINVAL;
        return C_ERR;
    }
    return C_OK;
}

/* Return the first password of the default user or NULL.
 * This function is needed for backward compatibility with the old
 * directive "requirepass" when Redis supported a single global
 * password. */
sds ACLDefaultUserFirstPassword(void) {
    if (listLength(DefaultUser->passwords) == 0) return NULL;
    listNode *first = listFirst(DefaultUser->passwords);
    return listNodeValue(first);
}

/* Initialization of the ACL subsystem. */
void ACLInit(void) {
    Users = raxNew();
    DefaultUser = ACLCreateUser("default",7);
    ACLSetUser(DefaultUser,"+@all",-1);
    ACLSetUser(DefaultUser,"~*",-1);
    ACLSetUser(DefaultUser,"on",-1);
    ACLSetUser(DefaultUser,"nopass",-1);
}

/* Check the username and password pair and return C_OK if they are valid,
 * otherwise C_ERR is returned and errno is set to:
 *
 *  EINVAL: if the username-password do not match.
 *  ENONENT: if the specified user does not exist at all.
 */
int ACLCheckUserCredentials(robj *username, robj *password) {
    user *u = ACLGetUserByName(username->ptr,sdslen(username->ptr));
    if (u == NULL) {
        errno = ENOENT;
        return C_ERR;
    }

    /* Disabled users can't login. */
    if ((u->flags & USER_FLAG_ENABLED) == 0) {
        errno = EINVAL;
        return C_ERR;
    }

    /* If the user is configured to don't require any password, we
     * are already fine here. */
    if (u->flags & USER_FLAG_NOPASS) return C_OK;

    /* Check all the user passwords for at least one to match. */
    listIter li;
    listNode *ln;
    listRewind(u->passwords,&li);
    while((ln = listNext(&li))) {
        sds thispass = listNodeValue(ln);
        if (!time_independent_strcmp(password->ptr, thispass))
            return C_OK;
    }

    /* If we reached this point, no password matched. */
    errno = EINVAL;
    return C_ERR;
}

/* For ACL purposes, every user has a bitmap with the commands that such
 * user is allowed to execute. In order to populate the bitmap, every command
 * should have an assigned ID (that is used to index the bitmap). This function
 * creates such an ID: it uses sequential IDs, reusing the same ID for the same
 * command name, so that a command retains the same ID in case of modules that
 * are unloaded and later reloaded. */
unsigned long ACLGetCommandID(const char *cmdname) {
    static rax *map = NULL;
    static unsigned long nextid = 0;

    sds lowername = sdsnew(cmdname);
    sdstolower(lowername);
    if (map == NULL) map = raxNew();
    void *id = raxFind(map,(unsigned char*)lowername,sdslen(lowername));
    if (id != raxNotFound) {
        sdsfree(lowername);
        return (unsigned long)id;
    }
    raxInsert(map,(unsigned char*)lowername,strlen(lowername),
              (void*)nextid,NULL);
    sdsfree(lowername);
    nextid++;

    /* We never assign the last bit in the user commands bitmap structure,
     * this way we can later check if this bit is set, understanding if the
     * current ACL for the user was created starting with a +@all to add all
     * the possible commands and just subtracting other single commands or
     * categories, or if, instead, the ACL was created just adding commands
     * and command categories from scratch, not allowing future commands by
     * default (loaded via modules). This is useful when rewriting the ACLs
     * with ACL SAVE. */
    if (nextid == USER_COMMAND_BITS_COUNT-1) nextid++;
    return nextid;
}

/* Return an username by its name, or NULL if the user does not exist. */
user *ACLGetUserByName(const char *name, size_t namelen) {
    void *myuser = raxFind(Users,(unsigned char*)name,namelen);
    if (myuser == raxNotFound) return NULL;
    return myuser;
}

/* Check if the command ready to be excuted in the client 'c', and already
 * referenced by c->cmd, can be executed by this client according to the
 * ACls associated to the client user c->user.
 *
 * If the user can execute the command ACL_OK is returned, otherwise
 * ACL_DENIED_CMD or ACL_DENIED_KEY is returned: the first in case the
 * command cannot be executed because the user is not allowed to run such
 * command, the second if the command is denied because the user is trying
 * to access keys that are not among the specified patterns. */
int ACLCheckCommandPerm(client *c) {
    user *u = c->user;
    uint64_t id = c->cmd->id;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* Check if the user can execute this command. */
    if (!(u->flags & USER_FLAG_ALLCOMMANDS) &&
        c->cmd->proc != authCommand)
    {
        /* If the bit is not set we have to check further, in case the
         * command is allowed just with that specific subcommand. */
        if (ACLGetUserCommandBit(u,id) == 0) {
            /* Check if the subcommand matches. */
            if (u->allowed_subcommands == NULL || c->argc < 2)
                return ACL_DENIED_CMD;
            long subid = 0;
            while (1) {
                if (u->allowed_subcommands[id][subid] == NULL)
                    return ACL_DENIED_CMD;
                if (!strcasecmp(c->argv[1]->ptr,
                                u->allowed_subcommands[id][subid]))
                    break; /* Subcommand match found. Stop here. */
                subid++;
            }
        }
    }

    /* Check if the user can execute commands explicitly touching the keys
     * mentioned in the command arguments. */
    if (!(c->user->flags & USER_FLAG_ALLKEYS) &&
        (c->cmd->getkeys_proc || c->cmd->firstkey))
    {
        int numkeys;
        int *keyidx = getKeysFromCommand(c->cmd,c->argv,c->argc,&numkeys);
        for (int j = 0; j < numkeys; j++) {
            listIter li;
            listNode *ln;
            listRewind(u->passwords,&li);

            /* Test this key against every pattern. */
            int match = 0;
            while((ln = listNext(&li))) {
                sds pattern = listNodeValue(ln);
                size_t plen = sdslen(pattern);
                int idx = keyidx[j];
                if (stringmatchlen(pattern,plen,c->argv[idx]->ptr,
                                   sdslen(c->argv[idx]->ptr),0))
                {
                    match = 1;
                    break;
                }
            }
            if (!match) return ACL_DENIED_KEY;
        }
        getKeysFreeResult(keyidx);
    }

    /* If we survived all the above checks, the user can execute the
     * command. */
    return ACL_OK;
}

/* =============================================================================
 * ACL related commands
 * ==========================================================================*/

/* ACL -- show and modify the configuration of ACL users.
 * ACL HELP
 * ACL LIST
 * ACL SETUSER <username> ... user attribs ...
 * ACL DELUSER <username>
 * ACL GETUSER <username>
 */
void aclCommand(client *c) {
    char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub,"setuser") && c->argc >= 3) {
        sds username = c->argv[2]->ptr;
        user *u = ACLGetUserByName(username,sdslen(username));
        if (!u) u = ACLCreateUser(username,sdslen(username));
        serverAssert(u != NULL);
        for (int j = 3; j < c->argc; j++) {
            if (ACLSetUser(u,c->argv[j]->ptr,sdslen(c->argv[j]->ptr)) != C_OK) {
                char *errmsg = "wrong format";
                if (errno == ENOENT) errmsg = "unknown command name in ACL";
                if (errno == EINVAL) errmsg = "syntax error";
                addReplyErrorFormat(c,
                    "Error in ACL SETUSER modifier '%s': %s",
                    (char*)c->argv[j]->ptr, errmsg);
                return;
            }
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(sub,"whoami")) {
        if (c->user != NULL) {
            addReplyBulkCBuffer(c,c->user->name,sdslen(c->user->name));
        } else {
            addReplyNull(c);
        }
    } else if (!strcasecmp(sub,"getuser") && c->argc == 3) {
        user *u = ACLGetUserByName(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        addReplyMapLen(c,2);

        /* Flags */
        addReplyBulkCString(c,"flags");
        void *deflen = addReplyDeferredLen(c);
        int numflags = 0;
        if (u->flags & USER_FLAG_ENABLED) {
            addReplyBulkCString(c,"on");
            numflags++;
        } else {
            addReplyBulkCString(c,"off");
            numflags++;
        }
        if (u->flags & USER_FLAG_ALLKEYS) {
            addReplyBulkCString(c,"allkeys");
            numflags++;
        }
        if (u->flags & USER_FLAG_ALLCOMMANDS) {
            addReplyBulkCString(c,"allcommands");
            numflags++;
        }
        if (u->flags & USER_FLAG_NOPASS) {
            addReplyBulkCString(c,"nopass");
            numflags++;
        }
        setDeferredSetLen(c,deflen,numflags);

        /* Passwords */
        addReplyBulkCString(c,"passwords");
        addReplyArrayLen(c,listLength(u->passwords));
        listIter li;
        listNode *ln;
        listRewind(u->passwords,&li);
        while((ln = listNext(&li))) {
            sds thispass = listNodeValue(ln);
            addReplyBulkCBuffer(c,thispass,sdslen(thispass));
        }
    } else if (!strcasecmp(sub,"help")) {
        const char *help[] = {
"LIST                              -- List all the registered users.",
"SETUSER <username> [attribs ...]  -- Create or modify a user.",
"DELUSER <username>                -- Delete a user.",
"GETUSER <username>                -- Get the user details.",
"WHOAMI                            -- Return the current username.",
NULL
        };
        addReplyHelp(c,help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
