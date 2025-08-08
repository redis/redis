/*
 * Copyright (c) 2018-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "cluster.h"
#include "sha256.h"
#include <fcntl.h>
#include <ctype.h>

/* =============================================================================
 * Global state for ACLs
 * ==========================================================================*/

rax *Users; /* Table mapping usernames to user structures. */

user *DefaultUser;  /* Global reference to the default user.
                       Every new connection is associated to it, if no
                       AUTH or HELLO is used to authenticate with a
                       different user. */

list *UsersToLoad;  /* This is a list of users found in the configuration file
                       that we'll need to load in the final stage of Redis
                       initialization, after all the modules are already
                       loaded. Every list element is a NULL terminated
                       array of SDS pointers: the first is the user name,
                       all the remaining pointers are ACL rules in the same
                       format as ACLSetUser(). */
list *ACLLog;       /* Our security log, the user is able to inspect that
                       using the ACL LOG command .*/

long long ACLLogEntryCount = 0; /* Number of ACL log entries created */

static rax *commandId = NULL; /* Command name to id mapping */

static unsigned long nextid = 0; /* Next command id that has not been assigned */

#define ACL_MAX_CATEGORIES 64 /* Maximum number of command categories  */

struct ACLCategoryItem {
    char *name;
    uint64_t flag;
} ACLDefaultCommandCategories[] = { /* See redis.conf for details on each category. */
    {"keyspace", ACL_CATEGORY_KEYSPACE},
    {"read", ACL_CATEGORY_READ},
    {"write", ACL_CATEGORY_WRITE},
    {"set", ACL_CATEGORY_SET},
    {"sortedset", ACL_CATEGORY_SORTEDSET},
    {"list", ACL_CATEGORY_LIST},
    {"hash", ACL_CATEGORY_HASH},
    {"string", ACL_CATEGORY_STRING},
    {"bitmap", ACL_CATEGORY_BITMAP},
    {"hyperloglog", ACL_CATEGORY_HYPERLOGLOG},
    {"geo", ACL_CATEGORY_GEO},
    {"stream", ACL_CATEGORY_STREAM},
    {"pubsub", ACL_CATEGORY_PUBSUB},
    {"admin", ACL_CATEGORY_ADMIN},
    {"fast", ACL_CATEGORY_FAST},
    {"slow", ACL_CATEGORY_SLOW},
    {"blocking", ACL_CATEGORY_BLOCKING},
    {"dangerous", ACL_CATEGORY_DANGEROUS},
    {"connection", ACL_CATEGORY_CONNECTION},
    {"transaction", ACL_CATEGORY_TRANSACTION},
    {"scripting", ACL_CATEGORY_SCRIPTING},
    {NULL,0} /* Terminator. */
};

static struct ACLCategoryItem *ACLCommandCategories = NULL;
static size_t nextCommandCategory = 0; /* Index of the next command category to be added */

/* Implements the ability to add to the list of ACL categories at runtime. Since each ACL category
 * also requires a bit in the acl_categories flag, there is a limit to the number that can be added.
 * The new ACL categories occupy the remaining bits of acl_categories flag, other than the bits
 * occupied by the default ACL command categories.
 * 
 * The optional `flag` argument allows the assignment of the `acl_categories` flag bit to the ACL category.
 * When adding a new category, except for the default ACL command categories, this arguments should be `0`
 * to allow the function to assign the next available `acl_categories` flag bit to the new ACL category.
 *
 * returns 1 -> Added, 0 -> Failed (out of space)
 *
 * This function is present here to gain access to the ACLCommandCategories array and add a new ACL category.
 */
int ACLAddCommandCategory(const char *name, uint64_t flag) {
    if (nextCommandCategory >= ACL_MAX_CATEGORIES) return 0;
    ACLCommandCategories[nextCommandCategory].name = zstrdup(name);
    ACLCommandCategories[nextCommandCategory].flag = flag != 0 ? flag : (1ULL<<nextCommandCategory);
    nextCommandCategory++;
    return 1;
}

/* Initializes ACLCommandCategories with default ACL categories and allocates space for 
 * new ACL categories.
 */
void ACLInitCommandCategories(void) {
    ACLCommandCategories = zcalloc(sizeof(struct ACLCategoryItem) * (ACL_MAX_CATEGORIES + 1));
    for (int j = 0; ACLDefaultCommandCategories[j].flag; j++) {
        serverAssert(ACLAddCommandCategory(ACLDefaultCommandCategories[j].name, ACLDefaultCommandCategories[j].flag));
    }
}

/* This function removes the specified number of categories from the trailing end of
 * the `ACLCommandCategories` array.
 * The purpose of this is to remove the categories added by modules that fail
 * during the onload function.
 */
void ACLCleanupCategoriesOnFailure(size_t num_acl_categories_added) {
    for (size_t j = nextCommandCategory - num_acl_categories_added; j < nextCommandCategory; j++) {
        zfree(ACLCommandCategories[j].name);
        ACLCommandCategories[j].name = NULL;
        ACLCommandCategories[j].flag = 0;
    }
    nextCommandCategory -= num_acl_categories_added;
}

struct ACLUserFlag {
    const char *name;
    uint64_t flag;
} ACLUserFlags[] = {
    /* Note: the order here dictates the emitted order at ACLDescribeUser */
    {"on", USER_FLAG_ENABLED},
    {"off", USER_FLAG_DISABLED},
    {"nopass", USER_FLAG_NOPASS},
    {"skip-sanitize-payload", USER_FLAG_SANITIZE_PAYLOAD_SKIP},
    {"sanitize-payload", USER_FLAG_SANITIZE_PAYLOAD},
    {NULL,0} /* Terminator. */
};

struct ACLSelectorFlags {
    const char *name;
    uint64_t flag;
} ACLSelectorFlags[] = {
    /* Note: the order here dictates the emitted order at ACLDescribeUser */
    {"allkeys", SELECTOR_FLAG_ALLKEYS},
    {"allchannels", SELECTOR_FLAG_ALLCHANNELS},
    {"allcommands", SELECTOR_FLAG_ALLCOMMANDS},
    {NULL,0} /* Terminator. */
};

/* ACL selectors are private and not exposed outside of acl.c. */
typedef struct {
    uint32_t flags; /* See SELECTOR_FLAG_* */
    /* The bit in allowed_commands is set if this user has the right to
     * execute this command.
     *
     * If the bit for a given command is NOT set and the command has
     * allowed first-args, Redis will also check allowed_firstargs in order to
     * understand if the command can be executed. */
    uint64_t allowed_commands[USER_COMMAND_BITS_COUNT/64];
    /* allowed_firstargs is used by ACL rules to block access to a command unless a
     * specific argv[1] is given.
     *
     * For each command ID (corresponding to the command bit set in allowed_commands),
     * This array points to an array of SDS strings, terminated by a NULL pointer,
     * with all the first-args that are allowed for this command. When no first-arg
     * matching is used, the field is just set to NULL to avoid allocating
     * USER_COMMAND_BITS_COUNT pointers. */
    sds **allowed_firstargs;
    list *patterns;  /* A list of allowed key patterns. If this field is NULL
                        the user cannot mention any key in a command, unless
                        the flag ALLKEYS is set in the user. */
    list *channels;  /* A list of allowed Pub/Sub channel patterns. If this
                        field is NULL the user cannot mention any channel in a
                        `PUBLISH` or [P][UNSUBSCRIBE] command, unless the flag
                        ALLCHANNELS is set in the user. */
    sds command_rules; /* A string representation of the ordered categories and commands, this
                        * is used to regenerate the original ACL string for display. */
} aclSelector;

void ACLResetFirstArgsForCommand(aclSelector *selector, unsigned long id);
void ACLResetFirstArgs(aclSelector *selector);
void ACLAddAllowedFirstArg(aclSelector *selector, unsigned long id, const char *sub);
void ACLFreeLogEntry(void *le);
int ACLSetSelector(aclSelector *selector, const char *op, size_t oplen);

/* The length of the string representation of a hashed password. */
#define HASH_PASSWORD_LEN (SHA256_BLOCK_SIZE*2)

/* =============================================================================
 * Helper functions for the rest of the ACL implementation
 * ==========================================================================*/

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function. Note: The two strings must be the same length.
 */
int time_independent_strcmp(char *a, char *b, int len) {
    int diff = 0;
    for (int j = 0; j < len; j++) {
        diff |= (a[j] ^ b[j]);
    }
    return diff; /* If zero strings are the same. */
}

/* Given an SDS string, returns the SHA256 hex representation as a
 * new SDS string. */
sds ACLHashPassword(unsigned char *cleartext, size_t len) {
    SHA256_CTX ctx;
    unsigned char hash[SHA256_BLOCK_SIZE];
    char hex[HASH_PASSWORD_LEN];
    char *cset = "0123456789abcdef";

    sha256_init(&ctx);
    sha256_update(&ctx,(unsigned char*)cleartext,len);
    sha256_final(&ctx,hash);

    for (int j = 0; j < SHA256_BLOCK_SIZE; j++) {
        hex[j*2] = cset[((hash[j]&0xF0)>>4)];
        hex[j*2+1] = cset[(hash[j]&0xF)];
    }
    return sdsnewlen(hex,HASH_PASSWORD_LEN);
}

/* Given a hash and the hash length, returns C_OK if it is a valid password
 * hash, or C_ERR otherwise. */
int ACLCheckPasswordHash(unsigned char *hash, int hashlen) {
    if (hashlen != HASH_PASSWORD_LEN) {
        return C_ERR;
    }

    /* Password hashes can only be characters that represent
     * hexadecimal values, which are numbers and lowercase
     * characters 'a' through 'f'. */
    for(int i = 0; i < HASH_PASSWORD_LEN; i++) {
        char c = hash[i];
        if ((c < 'a' || c > 'f') && (c < '0' || c > '9')) {
            return C_ERR;
        }
    }
    return C_OK;
}

/* =============================================================================
 * Low level ACL API
 * ==========================================================================*/

/* Return 1 if the specified string contains spaces or null characters.
 * We do this for usernames and key patterns for simpler rewriting of
 * ACL rules, presentation on ACL list, and to avoid subtle security bugs
 * that may arise from parsing the rules in presence of escapes.
 * The function returns 0 if the string has no spaces. */
int ACLStringHasSpaces(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (isspace(s[i]) || s[i] == 0) return 1;
    }
    return 0;
}

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

/* Method for searching for a user within a list of user definitions. The
 * list contains an array of user arguments, and we are only
 * searching the first argument, the username, for a match. */
int ACLListMatchLoadedUser(void *definition, void *user) {
    sds *user_definition = definition;
    return sdscmp(user_definition[0], user) == 0;
}

/* Method for passwords/pattern comparison used for the user->passwords list
 * so that we can search for items with listSearchKey(). */
int ACLListMatchSds(void *a, void *b) {
    return sdscmp(a,b) == 0;
}

/* Method to free list elements from ACL users password/patterns lists. */
void ACLListFreeSds(void *item) {
    sdsfreegeneric(item);
}

/* Method to duplicate list elements from ACL users password/patterns lists. */
void *ACLListDupSds(void *item) {
    return sdsdup(item);
}

/* Structure used for handling key patterns with different key
 * based permissions. */
typedef struct {
    int flags; /* The ACL key permission types for this key pattern */
    sds pattern; /* The pattern to match keys against */
} keyPattern;

/* Create a new key pattern. */
keyPattern *ACLKeyPatternCreate(sds pattern, int flags) {
    keyPattern *new = (keyPattern *) zmalloc(sizeof(keyPattern));
    new->pattern = pattern;
    new->flags = flags;
    return new;
}

/* Free a key pattern and internal structures. */
void ACLKeyPatternFree(keyPattern *pattern) {
    sdsfree(pattern->pattern);
    zfree(pattern);
}

/* Method for passwords/pattern comparison used for the user->passwords list
 * so that we can search for items with listSearchKey(). */
int ACLListMatchKeyPattern(void *a, void *b) {
    return sdscmp(((keyPattern *) a)->pattern,((keyPattern *) b)->pattern) == 0;
}

/* Method to free list elements from ACL users password/patterns lists. */
void ACLListFreeKeyPattern(void *item) {
    ACLKeyPatternFree(item);
}

/* Method to duplicate list elements from ACL users password/patterns lists. */
void *ACLListDupKeyPattern(void *item) {
    keyPattern *old = (keyPattern *) item;
    return ACLKeyPatternCreate(sdsdup(old->pattern), old->flags);
}

/* Append the string representation of a key pattern onto the
 * provided base string. */
sds sdsCatPatternString(sds base, keyPattern *pat) {
    if (pat->flags == ACL_ALL_PERMISSION) {
        base = sdscatlen(base,"~",1);
    } else if (pat->flags == ACL_READ_PERMISSION) {
        base = sdscatlen(base,"%R~",3);
    } else if (pat->flags == ACL_WRITE_PERMISSION) {
        base = sdscatlen(base,"%W~",3);
    } else {
        serverPanic("Invalid key pattern flag detected");
    }
    return sdscatsds(base, pat->pattern);
}

/* Create an empty selector with the provided set of initial
 * flags. The selector will be default have no permissions. */
aclSelector *ACLCreateSelector(int flags) {
    aclSelector *selector = zmalloc(sizeof(aclSelector));
    selector->flags = flags | server.acl_pubsub_default;
    selector->patterns = listCreate();
    selector->channels = listCreate();
    selector->allowed_firstargs = NULL;
    selector->command_rules = sdsempty();

    listSetMatchMethod(selector->patterns,ACLListMatchKeyPattern);
    listSetFreeMethod(selector->patterns,ACLListFreeKeyPattern);
    listSetDupMethod(selector->patterns,ACLListDupKeyPattern);
    listSetMatchMethod(selector->channels,ACLListMatchSds);
    listSetFreeMethod(selector->channels,ACLListFreeSds);
    listSetDupMethod(selector->channels,ACLListDupSds);
    memset(selector->allowed_commands,0,sizeof(selector->allowed_commands));

    return selector;
}

/* Cleanup the provided selector, including all interior structures. */
void ACLFreeSelector(aclSelector *selector) {
    listRelease(selector->patterns);
    listRelease(selector->channels);
    sdsfree(selector->command_rules);
    ACLResetFirstArgs(selector);
    zfree(selector);
}

/* Create an exact copy of the provided selector. */
aclSelector *ACLCopySelector(aclSelector *src) {
    aclSelector *dst = zmalloc(sizeof(aclSelector));
    dst->flags = src->flags;
    dst->patterns = listDup(src->patterns);
    dst->channels = listDup(src->channels);
    dst->command_rules = sdsdup(src->command_rules);
    memcpy(dst->allowed_commands,src->allowed_commands,
           sizeof(dst->allowed_commands));
    dst->allowed_firstargs = NULL;
    /* Copy the allowed first-args array of array of SDS strings. */
    if (src->allowed_firstargs) {
        for (int j = 0; j < USER_COMMAND_BITS_COUNT; j++) {
            if (!(src->allowed_firstargs[j])) continue;
            for (int i = 0; src->allowed_firstargs[j][i]; i++) {
                ACLAddAllowedFirstArg(dst, j, src->allowed_firstargs[j][i]);
            }
        }
    }
    return dst;
}

/* List method for freeing a selector */
void ACLListFreeSelector(void *a) {
    ACLFreeSelector((aclSelector *) a);
}

/* List method for duplicating a selector */
void *ACLListDuplicateSelector(void *src) {
    return ACLCopySelector((aclSelector *)src);
}

/* All users have an implicit root selector which
 * provides backwards compatibility to the old ACLs-
 * permissions. */
aclSelector *ACLUserGetRootSelector(user *u) {
    serverAssert(listLength(u->selectors));
    aclSelector *s = (aclSelector *) listNodeValue(listFirst(u->selectors));
    serverAssert(s->flags & SELECTOR_FLAG_ROOT);
    return s;
}

/* Create a new user with the specified name, store it in the list
 * of users (the Users global radix tree), and returns a reference to
 * the structure representing the user.
 *
 * If the user with such name already exists NULL is returned. */
user *ACLCreateUser(const char *name, size_t namelen) {
    if (raxFind(Users,(unsigned char*)name,namelen,NULL)) return NULL;
    user *u = zmalloc(sizeof(*u));
    u->name = sdsnewlen(name,namelen);
    u->flags = USER_FLAG_DISABLED;
    u->flags |= USER_FLAG_SANITIZE_PAYLOAD;
    u->passwords = listCreate();
    u->acl_string = NULL;
    listSetMatchMethod(u->passwords,ACLListMatchSds);
    listSetFreeMethod(u->passwords,ACLListFreeSds);
    listSetDupMethod(u->passwords,ACLListDupSds);

    u->selectors = listCreate();
    listSetFreeMethod(u->selectors,ACLListFreeSelector);
    listSetDupMethod(u->selectors,ACLListDuplicateSelector);

    /* Add the initial root selector */
    aclSelector *s = ACLCreateSelector(SELECTOR_FLAG_ROOT);
    listAddNodeHead(u->selectors, s);

    raxInsert(Users,(unsigned char*)name,namelen,u,NULL);
    return u;
}

/* This function should be called when we need an unlinked "fake" user
 * we can use in order to validate ACL rules or for other similar reasons.
 * The user will not get linked to the Users radix tree. The returned
 * user should be released with ACLFreeUser() as usually. */
user *ACLCreateUnlinkedUser(void) {
    char username[64];
    for (int j = 0; ; j++) {
        snprintf(username,sizeof(username),"__fakeuser:%d__",j);
        user *fakeuser = ACLCreateUser(username,strlen(username));
        if (fakeuser == NULL) continue;
        int retval = raxRemove(Users,(unsigned char*) username,
                               strlen(username),NULL);
        serverAssert(retval != 0);
        return fakeuser;
    }
}

/* Release the memory used by the user structure. Note that this function
 * will not remove the user from the Users global radix tree. */
void ACLFreeUser(user *u) {
    sdsfree(u->name);
    if (u->acl_string) {
        decrRefCount(u->acl_string);
        u->acl_string = NULL;
    }
    listRelease(u->passwords);
    listRelease(u->selectors);
    zfree(u);
}

/* Generic version of ACLFreeUser. */
void ACLFreeUserGeneric(void *u) {
    ACLFreeUser((user *)u);
}

/* When a user is deleted we need to cycle the active
 * connections in order to kill all the pending ones that
 * are authenticated with such user. */
void ACLFreeUserAndKillClients(user *u) {
    listIter li;
    listNode *ln;
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->user == u) {
            /* We'll free the connection asynchronously, so
             * in theory to set a different user is not needed.
             * However if there are bugs in Redis, soon or later
             * this may result in some security hole: it's much
             * more defensive to set the default user and put
             * it in non authenticated mode. */
            deauthenticateAndCloseClient(c);
        }
    }
    ACLFreeUser(u);
}

/* Copy the user ACL rules from the source user 'src' to the destination
 * user 'dst' so that at the end of the process they'll have exactly the
 * same rules (but the names will continue to be the original ones). */
void ACLCopyUser(user *dst, user *src) {
    listRelease(dst->passwords);
    listRelease(dst->selectors);
    dst->passwords = listDup(src->passwords);
    dst->selectors = listDup(src->selectors);
    dst->flags = src->flags;
    if (dst->acl_string) {
        decrRefCount(dst->acl_string);
    }
    dst->acl_string = src->acl_string;
    if (dst->acl_string) {
        /* if src is NULL, we set it to NULL, if not, need to increment reference count */
        incrRefCount(dst->acl_string);
    }
}

/* Given a command ID, this function set by reference 'word' and 'bit'
 * so that user->allowed_commands[word] will address the right word
 * where the corresponding bit for the provided ID is stored, and
 * so that user->allowed_commands[word]&bit will identify that specific
 * bit. The function returns C_ERR in case the specified ID overflows
 * the bitmap in the user representation. */
int ACLGetCommandBitCoordinates(uint64_t id, uint64_t *word, uint64_t *bit) {
    if (id >= USER_COMMAND_BITS_COUNT) return C_ERR;
    *word = id / sizeof(uint64_t) / 8;
    *bit = 1ULL << (id % (sizeof(uint64_t) * 8));
    return C_OK;
}

/* Check if the specified command bit is set for the specified user.
 * The function returns 1 is the bit is set or 0 if it is not.
 * Note that this function does not check the ALLCOMMANDS flag of the user
 * but just the lowlevel bitmask.
 *
 * If the bit overflows the user internal representation, zero is returned
 * in order to disallow the execution of the command in such edge case. */
int ACLGetSelectorCommandBit(const aclSelector *selector, unsigned long id) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return 0;
    return (selector->allowed_commands[word] & bit) != 0;
}

/* When +@all or allcommands is given, we set a reserved bit as well that we
 * can later test, to see if the user has the right to execute "future commands",
 * that is, commands loaded later via modules. */
int ACLSelectorCanExecuteFutureCommands(aclSelector *selector) {
    return ACLGetSelectorCommandBit(selector,USER_COMMAND_BITS_COUNT-1);
}

/* Set the specified command bit for the specified user to 'value' (0 or 1).
 * If the bit overflows the user internal representation, no operation
 * is performed. As a side effect of calling this function with a value of
 * zero, the user flag ALLCOMMANDS is cleared since it is no longer possible
 * to skip the command bit explicit test. */
void ACLSetSelectorCommandBit(aclSelector *selector, unsigned long id, int value) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return;
    if (value) {
        selector->allowed_commands[word] |= bit;
    } else {
        selector->allowed_commands[word] &= ~bit;
        selector->flags &= ~SELECTOR_FLAG_ALLCOMMANDS;
    }
}

/* Remove a rule from the retained command rules. Always match rules
 * verbatim, but also remove subcommand rules if we are adding or removing the 
 * entire command. */
void ACLSelectorRemoveCommandRule(aclSelector *selector, sds new_rule) {
    size_t new_len = sdslen(new_rule);
    char *existing_rule = selector->command_rules;

    /* Loop over the existing rules, trying to find a rule that "matches"
     * the new rule. If we find a match, then remove the command from the string by
     * copying the later rules over it. */
    while(existing_rule[0]) {
        /* The first character of the rule is +/-, which we don't need to compare. */
        char *copy_position = existing_rule;
        existing_rule += 1;

        /* Assume a trailing space after a command is part of the command, like '+get ', so trim it
         * as well if the command is removed. */
        char *rule_end = strchr(existing_rule, ' ');
        if (!rule_end) {
            /* This is the last rule, so move it to the end of the string. */
            rule_end = existing_rule + strlen(existing_rule);

            /* This approach can leave a trailing space if the last rule is removed,
             * but only if it's not the first rule, so handle that case. */
            if (copy_position != selector->command_rules) copy_position -= 1;
        }
        char *copy_end = rule_end;
        if (*copy_end == ' ') copy_end++;

        /* Exact match or the rule we are comparing is a subcommand denoted by '|' */
        size_t existing_len = rule_end - existing_rule;
        if (!memcmp(existing_rule, new_rule, min(existing_len, new_len))) {
            if ((existing_len == new_len) || (existing_len > new_len && (existing_rule[new_len]) == '|')) {
                /* Copy the remaining rules starting at the next rule to replace the rule to be
                 * deleted, including the terminating NULL character. */
                memmove(copy_position, copy_end, strlen(copy_end) + 1);
                existing_rule = copy_position;
                continue;
            }
        }
        existing_rule = copy_end;
    }

    /* There is now extra padding at the end of the rules, so clean that up. */
    sdsupdatelen(selector->command_rules);
}

/* This function is resopnsible for updating the command_rules struct so that relative ordering of
 * commands and categories is maintained and can be reproduced without loss. */
void ACLUpdateCommandRules(aclSelector *selector, const char *rule, int allow) {
    sds new_rule = sdsnew(rule);
    sdstolower(new_rule);

    ACLSelectorRemoveCommandRule(selector, new_rule);
    if (sdslen(selector->command_rules)) selector->command_rules = sdscat(selector->command_rules, " ");
    selector->command_rules = sdscatfmt(selector->command_rules, allow ? "+%S" : "-%S", new_rule);
    sdsfree(new_rule);
}

/* This function is used to allow/block a specific command.
 * Allowing/blocking a container command also applies for its subcommands */
void ACLChangeSelectorPerm(aclSelector *selector, struct redisCommand *cmd, int allow) {
    unsigned long id = cmd->id;
    ACLSetSelectorCommandBit(selector,id,allow);
    ACLResetFirstArgsForCommand(selector,id);
    if (cmd->subcommands_dict) {
        dictEntry *de;
        dictIterator di;
        dictInitSafeIterator(&di, cmd->subcommands_dict);
        while((de = dictNext(&di)) != NULL) {
            struct redisCommand *sub = (struct redisCommand *)dictGetVal(de);
            ACLSetSelectorCommandBit(selector,sub->id,allow);
        }
        dictResetIterator(&di);
    }
}

/* This is like ACLSetSelectorCommandBit(), but instead of setting the specified
 * ID, it will check all the commands in the category specified as argument,
 * and will set all the bits corresponding to such commands to the specified
 * value. Since the category passed by the user may be non existing, the
 * function returns C_ERR if the category was not found, or C_OK if it was
 * found and the operation was performed. */
void ACLSetSelectorCommandBitsForCategory(dict *commands, aclSelector *selector, uint64_t cflag, int value) {
    dictIterator di;
    dictEntry *de;
    dictInitIterator(&di, commands);
    while ((de = dictNext(&di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->acl_categories & cflag) {
            ACLChangeSelectorPerm(selector,cmd,value);
        }
        if (cmd->subcommands_dict) {
            ACLSetSelectorCommandBitsForCategory(cmd->subcommands_dict, selector, cflag, value);
        }
    }
    dictResetIterator(&di);
}

/* This function is responsible for recomputing the command bits for all selectors of the existing users.
 * It uses the 'command_rules', a string representation of the ordered categories and commands, 
 * to recompute the command bits. */
void ACLRecomputeCommandBitsFromCommandRulesAllUsers(void) {
    raxIterator ri;
    raxStart(&ri,Users);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        user *u = ri.data;
        listIter li;
        listNode *ln;
        listRewind(u->selectors,&li);
        while((ln = listNext(&li))) {
            aclSelector *selector = (aclSelector *) listNodeValue(ln);
            int argc = 0;
            sds *argv = sdssplitargs(selector->command_rules, &argc);
            serverAssert(argv != NULL);
            /* Checking selector's permissions for all commands to start with a clean state. */
            if (ACLSelectorCanExecuteFutureCommands(selector)) {
                int res = ACLSetSelector(selector,"+@all",-1);
                serverAssert(res == C_OK);
            } else {
                int res = ACLSetSelector(selector,"-@all",-1);
                serverAssert(res == C_OK);
            }

            /* Apply all of the commands and categories to this selector. */
            for(int i = 0; i < argc; i++) {
                int res = ACLSetSelector(selector, argv[i], sdslen(argv[i]));
                serverAssert(res == C_OK);
            }
            sdsfreesplitres(argv, argc);
        }
    }
    raxStop(&ri);

}

int ACLSetSelectorCategory(aclSelector *selector, const char *category, int allow) {
    uint64_t cflag = ACLGetCommandCategoryFlagByName(category + 1);
    if (!cflag) return C_ERR;

    ACLUpdateCommandRules(selector, category, allow);

    /* Set the actual command bits on the selector. */
    ACLSetSelectorCommandBitsForCategory(server.orig_commands, selector, cflag, allow);
    return C_OK;
}

void ACLCountCategoryBitsForCommands(dict *commands, aclSelector *selector, unsigned long *on, unsigned long *off, uint64_t cflag) {
    dictIterator di;
    dictEntry *de;
    dictInitIterator(&di, commands);
    while ((de = dictNext(&di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->acl_categories & cflag) {
            if (ACLGetSelectorCommandBit(selector,cmd->id))
                (*on)++;
            else
                (*off)++;
        }
        if (cmd->subcommands_dict) {
            ACLCountCategoryBitsForCommands(cmd->subcommands_dict, selector, on, off, cflag);
        }
    }
    dictResetIterator(&di);
}

/* Return the number of commands allowed (on) and denied (off) for the user 'u'
 * in the subset of commands flagged with the specified category name.
 * If the category name is not valid, C_ERR is returned, otherwise C_OK is
 * returned and on and off are populated by reference. */
int ACLCountCategoryBitsForSelector(aclSelector *selector, unsigned long *on, unsigned long *off,
                                const char *category)
{
    uint64_t cflag = ACLGetCommandCategoryFlagByName(category);
    if (!cflag) return C_ERR;

    *on = *off = 0;
    ACLCountCategoryBitsForCommands(server.orig_commands, selector, on, off, cflag);
    return C_OK;
}

/* This function returns an SDS string representing the specified selector ACL
 * rules related to command execution, in the same format you could set them
 * back using ACL SETUSER. The function will return just the set of rules needed
 * to recreate the user commands bitmap, without including other user flags such
 * as on/off, passwords and so forth. The returned string always starts with
 * the +@all or -@all rule, depending on the user bitmap, and is followed, if
 * needed, by the other rules needed to narrow or extend what the user can do. */
sds ACLDescribeSelectorCommandRules(aclSelector *selector) {
    sds rules = sdsempty();

    /* We use this fake selector as a "sanity" check to make sure the rules
     * we generate have the same bitmap as those on the current selector. */
    aclSelector *fake_selector = ACLCreateSelector(0);

    /* Here we want to understand if we should start with +@all or -@all.
     * Note that when starting with +@all and subtracting, the user
     * will be able to execute future commands, while -@all and adding will just
     * allow the user the run the selected commands and/or categories.
     * How do we test for that? We use the trick of a reserved command ID bit
     * that is set only by +@all (and its alias "allcommands"). */
    if (ACLSelectorCanExecuteFutureCommands(selector)) {
        rules = sdscat(rules,"+@all ");
        ACLSetSelector(fake_selector,"+@all",-1);
    } else {
        rules = sdscat(rules,"-@all ");
        ACLSetSelector(fake_selector,"-@all",-1);
    }

    /* Apply all of the commands and categories to the fake selector. */
    int argc = 0;
    sds *argv = sdssplitargs(selector->command_rules, &argc);
    serverAssert(argv != NULL);

    for(int i = 0; i < argc; i++) {
        int res = ACLSetSelector(fake_selector, argv[i], -1);
        serverAssert(res == C_OK);
    }
    if (sdslen(selector->command_rules)) {
        rules = sdscatfmt(rules, "%S ", selector->command_rules);
    }
    sdsfreesplitres(argv, argc);

    /* Trim the final useless space. */
    sdsrange(rules,0,-2);

    /* This is technically not needed, but we want to verify that now the
     * predicted bitmap is exactly the same as the user bitmap, and abort
     * otherwise, because aborting is better than a security risk in this
     * code path. */
    if (memcmp(fake_selector->allowed_commands,
                        selector->allowed_commands,
                        sizeof(selector->allowed_commands)) != 0)
    {
        serverLog(LL_WARNING,
            "CRITICAL ERROR: User ACLs don't match final bitmap: '%s'",
            rules);
        serverPanic("No bitmap match in ACLDescribeSelectorCommandRules()");
    }
    ACLFreeSelector(fake_selector);
    return rules;
}

sds ACLDescribeSelector(aclSelector *selector) {
    listIter li;
    listNode *ln;
    sds res = sdsempty();
    /* Key patterns. */
    if (selector->flags & SELECTOR_FLAG_ALLKEYS) {
        res = sdscatlen(res,"~* ",3);
    } else {
        listRewind(selector->patterns,&li);
        while((ln = listNext(&li))) {
            keyPattern *thispat = (keyPattern *)listNodeValue(ln);
            res = sdsCatPatternString(res, thispat);
            res = sdscatlen(res," ",1);
        }
    }

    /* Pub/sub channel patterns. */
    if (selector->flags & SELECTOR_FLAG_ALLCHANNELS) {
        res = sdscatlen(res,"&* ",3);
    } else {
        res = sdscatlen(res,"resetchannels ",14);
        listRewind(selector->channels,&li);
        while((ln = listNext(&li))) {
            sds thispat = listNodeValue(ln);
            res = sdscatlen(res,"&",1);
            res = sdscatsds(res,thispat);
            res = sdscatlen(res," ",1);
        }
    }

    /* Command rules. */
    sds rules = ACLDescribeSelectorCommandRules(selector);
    res = sdscatsds(res,rules);
    sdsfree(rules);
    return res;
}

/* This is similar to ACLDescribeSelectorCommandRules(), however instead of
 * describing just the user command rules, everything is described: user
 * flags, keys, passwords and finally the command rules obtained via
 * the ACLDescribeSelectorCommandRules() function. This is the function we call
 * when we want to rewrite the configuration files describing ACLs and
 * in order to show users with ACL LIST. */
robj *ACLDescribeUser(user *u) {
    if (u->acl_string) {
        incrRefCount(u->acl_string);
        return u->acl_string;
    }

    sds res = sdsempty();

    /* Flags. */
    for (int j = 0; ACLUserFlags[j].flag; j++) {
        if (u->flags & ACLUserFlags[j].flag) {
            res = sdscat(res,ACLUserFlags[j].name);
            res = sdscatlen(res," ",1);
        }
    }

    /* Passwords. */
    listIter li;
    listNode *ln;
    listRewind(u->passwords,&li);
    while((ln = listNext(&li))) {
        sds thispass = listNodeValue(ln);
        res = sdscatlen(res,"#",1);
        res = sdscatsds(res,thispass);
        res = sdscatlen(res," ",1);
    }

    /* Selectors (Commands and keys) */
    listRewind(u->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *selector = (aclSelector *) listNodeValue(ln);
        sds default_perm = ACLDescribeSelector(selector);
        if (selector->flags & SELECTOR_FLAG_ROOT) {
            res = sdscatfmt(res, "%s", default_perm);
        } else {
            res = sdscatfmt(res, " (%s)", default_perm);
        }
        sdsfree(default_perm);
    }

    u->acl_string = createObject(OBJ_STRING, res);
    /* because we are returning it, have to increase count */
    incrRefCount(u->acl_string);

    return u->acl_string;
}

/* Get a command from the original command table, that is not affected
 * by the command renaming operations: we base all the ACL work from that
 * table, so that ACLs are valid regardless of command renaming. */
struct redisCommand *ACLLookupCommand(const char *name) {
    struct redisCommand *cmd;
    sds sdsname = sdsnew(name);
    cmd = lookupCommandBySdsLogic(server.orig_commands,sdsname);
    sdsfree(sdsname);
    return cmd;
}

/* Flush the array of allowed first-args for the specified user
 * and command ID. */
void ACLResetFirstArgsForCommand(aclSelector *selector, unsigned long id) {
    if (selector->allowed_firstargs && selector->allowed_firstargs[id]) {
        for (int i = 0; selector->allowed_firstargs[id][i]; i++)
            sdsfree(selector->allowed_firstargs[id][i]);
        zfree(selector->allowed_firstargs[id]);
        selector->allowed_firstargs[id] = NULL;
    }
}

/* Flush the entire table of first-args. This is useful on +@all, -@all
 * or similar to return back to the minimal memory usage (and checks to do)
 * for the user. */
void ACLResetFirstArgs(aclSelector *selector) {
    if (selector->allowed_firstargs == NULL) return;
    for (int j = 0; j < USER_COMMAND_BITS_COUNT; j++) {
        if (selector->allowed_firstargs[j]) {
            for (int i = 0; selector->allowed_firstargs[j][i]; i++)
                sdsfree(selector->allowed_firstargs[j][i]);
            zfree(selector->allowed_firstargs[j]);
        }
    }
    zfree(selector->allowed_firstargs);
    selector->allowed_firstargs = NULL;
}

/* Add a first-arg to the list of subcommands for the user 'u' and
 * the command id specified. */
void ACLAddAllowedFirstArg(aclSelector *selector, unsigned long id, const char *sub) {
    /* If this is the first first-arg to be configured for
     * this user, we have to allocate the first-args array. */
    if (selector->allowed_firstargs == NULL) {
        selector->allowed_firstargs = zcalloc(USER_COMMAND_BITS_COUNT * sizeof(sds*));
    }

    /* We also need to enlarge the allocation pointing to the
     * null terminated SDS array, to make space for this one.
     * To start check the current size, and while we are here
     * make sure the first-arg is not already specified inside. */
    long items = 0;
    if (selector->allowed_firstargs[id]) {
        while(selector->allowed_firstargs[id][items]) {
            /* If it's already here do not add it again. */
            if (!strcasecmp(selector->allowed_firstargs[id][items],sub))
                return;
            items++;
        }
    }

    /* Now we can make space for the new item (and the null term). */
    items += 2;
    selector->allowed_firstargs[id] = zrealloc(selector->allowed_firstargs[id], sizeof(sds)*items);
    selector->allowed_firstargs[id][items-2] = sdsnew(sub);
    selector->allowed_firstargs[id][items-1] = NULL;
}

/* Create an ACL selector from the given ACL operations, which should be 
 * a list of space separate ACL operations that starts and ends 
 * with parentheses.
 *
 * If any of the operations are invalid, NULL will be returned instead
 * and errno will be set corresponding to the interior error. */
aclSelector *aclCreateSelectorFromOpSet(const char *opset, size_t opsetlen) {
    serverAssert(opset[0] == '(' && opset[opsetlen - 1] == ')');
    aclSelector *s = ACLCreateSelector(0);

    int argc = 0;
    sds trimmed = sdsnewlen(opset + 1, opsetlen - 2);
    sds *argv = sdssplitargs(trimmed, &argc);
    for (int i = 0; i < argc; i++) {
        if (ACLSetSelector(s, argv[i], sdslen(argv[i])) == C_ERR) {
            ACLFreeSelector(s);
            s = NULL;
            goto cleanup;
        }
    }

cleanup:
    sdsfreesplitres(argv, argc);
    sdsfree(trimmed);
    return s;
}

/* Set a selector's properties with the provided 'op'.
 *
 * +<command>   Allow the execution of that command.
 *              May be used with `|` for allowing subcommands (e.g "+config|get")
 * -<command>   Disallow the execution of that command.
 *              May be used with `|` for blocking subcommands (e.g "-config|set")
 * +@<category> Allow the execution of all the commands in such category
 *              with valid categories are like @admin, @set, @sortedset, ...
 *              and so forth, see the full list in the server.c file where
 *              the Redis command table is described and defined.
 *              The special category @all means all the commands, but currently
 *              present in the server, and that will be loaded in the future
 *              via modules.
 * +<command>|first-arg    Allow a specific first argument of an otherwise
 *                         disabled command. Note that this form is not
 *                         allowed as negative like -SELECT|1, but
 *                         only additive starting with "+".
 * allcommands  Alias for +@all. Note that it implies the ability to execute
 *              all the future commands loaded via the modules system.
 * nocommands   Alias for -@all.
 * ~<pattern>   Add a pattern of keys that can be mentioned as part of
 *              commands. For instance ~* allows all the keys. The pattern
 *              is a glob-style pattern like the one of KEYS.
 *              It is possible to specify multiple patterns.
 * %R~<pattern> Add key read pattern that specifies which keys can be read
 *              from.
 * %W~<pattern> Add key write pattern that specifies which keys can be
 *              written to.
 * allkeys      Alias for ~*
 * resetkeys    Flush the list of allowed keys patterns.
 * &<pattern>   Add a pattern of channels that can be mentioned as part of
 *              Pub/Sub commands. For instance &* allows all the channels. The
 *              pattern is a glob-style pattern like the one of PSUBSCRIBE.
 *              It is possible to specify multiple patterns.
 * allchannels              Alias for &*
 * resetchannels            Flush the list of allowed channel patterns.
 */
int ACLSetSelector(aclSelector *selector, const char* op, size_t oplen) {
    if (!strcasecmp(op,"allkeys") ||
               !strcasecmp(op,"~*"))
    {
        selector->flags |= SELECTOR_FLAG_ALLKEYS;
        listEmpty(selector->patterns);
    } else if (!strcasecmp(op,"resetkeys")) {
        selector->flags &= ~SELECTOR_FLAG_ALLKEYS;
        listEmpty(selector->patterns);
    } else if (!strcasecmp(op,"allchannels") ||
               !strcasecmp(op,"&*"))
    {
        selector->flags |= SELECTOR_FLAG_ALLCHANNELS;
        listEmpty(selector->channels);
    } else if (!strcasecmp(op,"resetchannels")) {
        selector->flags &= ~SELECTOR_FLAG_ALLCHANNELS;
        listEmpty(selector->channels);
    } else if (!strcasecmp(op,"allcommands") ||
               !strcasecmp(op,"+@all"))
    {
        memset(selector->allowed_commands,255,sizeof(selector->allowed_commands));
        selector->flags |= SELECTOR_FLAG_ALLCOMMANDS;
        sdsclear(selector->command_rules);
        ACLResetFirstArgs(selector);
    } else if (!strcasecmp(op,"nocommands") ||
               !strcasecmp(op,"-@all"))
    {
        memset(selector->allowed_commands,0,sizeof(selector->allowed_commands));
        selector->flags &= ~SELECTOR_FLAG_ALLCOMMANDS;
        sdsclear(selector->command_rules);
        ACLResetFirstArgs(selector);
    } else if (op[0] == '~' || op[0] == '%') {
        if (selector->flags & SELECTOR_FLAG_ALLKEYS) {
            errno = EEXIST;
            return C_ERR;
        }
        int flags = 0;
        size_t offset = 1;
        if (op[0] == '%') {
            int perm_ok = 1;
            for (; offset < oplen; offset++) {
                if (toupper(op[offset]) == 'R' && !(flags & ACL_READ_PERMISSION)) {
                    flags |= ACL_READ_PERMISSION;
                } else if (toupper(op[offset]) == 'W' && !(flags & ACL_WRITE_PERMISSION)) {
                    flags |= ACL_WRITE_PERMISSION;
                } else if (op[offset] == '~') {
                    offset++;
                    break;
                } else {
                    perm_ok = 0;
                    break;
                }
            }
            if (!flags || !perm_ok) {
                errno = EINVAL;
                return C_ERR;
            }
        } else {
            flags = ACL_ALL_PERMISSION;
        }

        if (ACLStringHasSpaces(op+offset,oplen-offset)) {
            errno = EINVAL;
            return C_ERR;
        }
        keyPattern *newpat = ACLKeyPatternCreate(sdsnewlen(op+offset,oplen-offset), flags);
        listNode *ln = listSearchKey(selector->patterns,newpat);
        /* Avoid re-adding the same key pattern multiple times. */
        if (ln == NULL) {
            listAddNodeTail(selector->patterns,newpat);
        } else {
            ((keyPattern *)listNodeValue(ln))->flags |= flags;
            ACLKeyPatternFree(newpat);
        }
        selector->flags &= ~SELECTOR_FLAG_ALLKEYS;
    } else if (op[0] == '&') {
        if (selector->flags & SELECTOR_FLAG_ALLCHANNELS) {
            errno = EISDIR;
            return C_ERR;
        }
        if (ACLStringHasSpaces(op+1,oplen-1)) {
            errno = EINVAL;
            return C_ERR;
        }
        sds newpat = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(selector->channels,newpat);
        /* Avoid re-adding the same channel pattern multiple times. */
        if (ln == NULL)
            listAddNodeTail(selector->channels,newpat);
        else
            sdsfree(newpat);
        selector->flags &= ~SELECTOR_FLAG_ALLCHANNELS;
    } else if (op[0] == '+' && op[1] != '@') {
        if (strrchr(op,'|') == NULL) {
            struct redisCommand *cmd = ACLLookupCommand(op+1);
            if (cmd == NULL) {
                errno = ENOENT;
                return C_ERR;
            }
            ACLChangeSelectorPerm(selector,cmd,1);
            ACLUpdateCommandRules(selector,cmd->fullname,1);
        } else {
            /* Split the command and subcommand parts. */
            char *copy = zstrdup(op+1);
            char *sub = strrchr(copy,'|');
            sub[0] = '\0';
            sub++;

            struct redisCommand *cmd = ACLLookupCommand(copy);

            /* Check if the command exists. We can't check the
             * first-arg to see if it is valid. */
            if (cmd == NULL) {
                zfree(copy);
                errno = ENOENT;
                return C_ERR;
            }

            /* We do not support allowing first-arg of a subcommand */
            if (cmd->parent) {
                zfree(copy);
                errno = ECHILD;
                return C_ERR;
            }

            /* The subcommand cannot be empty, so things like DEBUG|
             * are syntax errors of course. */
            if (strlen(sub) == 0) {
                zfree(copy);
                errno = EINVAL;
                return C_ERR;
            }

            if (cmd->subcommands_dict) {
                /* If user is trying to allow a valid subcommand we can just add its unique ID */
                cmd = ACLLookupCommand(op+1);
                if (cmd == NULL) {
                    zfree(copy);
                    errno = ENOENT;
                    return C_ERR;
                }
                ACLChangeSelectorPerm(selector,cmd,1);
            } else {
                /* If user is trying to use the ACL mech to block SELECT except SELECT 0 or
                 * block DEBUG except DEBUG OBJECT (DEBUG subcommands are not considered
                 * subcommands for now) we use the allowed_firstargs mechanism. */

                /* Add the first-arg to the list of valid ones. */
                serverLog(LL_WARNING, "Deprecation warning: Allowing a first arg of an otherwise "
                                      "blocked command is a misuse of ACL and may get disabled "
                                      "in the future (offender: +%s)", op+1);
                ACLAddAllowedFirstArg(selector,cmd->id,sub);
            }
            ACLUpdateCommandRules(selector,op+1,1);
            zfree(copy);
        }
    } else if (op[0] == '-' && op[1] != '@') {
        struct redisCommand *cmd = ACLLookupCommand(op+1);
        if (cmd == NULL) {
            errno = ENOENT;
            return C_ERR;
        }
        ACLChangeSelectorPerm(selector,cmd,0);
        ACLUpdateCommandRules(selector,cmd->fullname,0);
    } else if ((op[0] == '+' || op[0] == '-') && op[1] == '@') {
        int bitval = op[0] == '+' ? 1 : 0;
        if (ACLSetSelectorCategory(selector,op+1,bitval) == C_ERR) {
            errno = ENOENT;
            return C_ERR;
        }
    } else {
        errno = EINVAL;
        return C_ERR;
    }
    return C_OK;
}

/* Set user properties according to the string "op". The following
 * is a description of what different strings will do:
 *
 * on           Enable the user: it is possible to authenticate as this user.
 * off          Disable the user: it's no longer possible to authenticate
 *              with this user, however the already authenticated connections
 *              will still work.
 * skip-sanitize-payload    RESTORE dump-payload sanitization is skipped.
 * sanitize-payload         RESTORE dump-payload is sanitized (default).
 * ><password>  Add this password to the list of valid password for the user.
 *              For example >mypass will add "mypass" to the list.
 *              This directive clears the "nopass" flag (see later).
 * #<hash>      Add this password hash to the list of valid hashes for
 *              the user. This is useful if you have previously computed
 *              the hash, and don't want to store it in plaintext.
 *              This directive clears the "nopass" flag (see later).
 * <<password>  Remove this password from the list of valid passwords.
 * !<hash>      Remove this hashed password from the list of valid passwords.
 *              This is useful when you want to remove a password just by
 *              hash without knowing its plaintext version at all.
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
 * reset        Performs the following actions: resetpass, resetkeys, resetchannels,
 *              allchannels (if acl-pubsub-default is set), off, clearselectors, -@all.
 *              The user returns to the same state it has immediately after its creation.
 * (<options>)  Create a new selector with the options specified within the
 *              parentheses and attach it to the user. Each option should be
 *              space separated. The first character must be ( and the last
 *              character must be ).
 * clearselectors          Remove all of the currently attached selectors. 
 *                         Note this does not change the "root" user permissions,
 *                         which are the permissions directly applied onto the
 *                         user (outside the parentheses).
 * 
 * Selector options can also be specified by this function, in which case
 * they update the root selector for the user.
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
 * EINVAL: The specified opcode is not understood or the key/channel pattern is
 *         invalid (contains non allowed characters).
 * ENOENT: The command name or command category provided with + or - is not
 *         known.
 * EEXIST: You are adding a key pattern after "*" was already added. This is
 *         almost surely an error on the user side.
 * EISDIR: You are adding a channel pattern after "*" was already added. This is
 *         almost surely an error on the user side.
 * ENODEV: The password you are trying to remove from the user does not exist.
 * EBADMSG: The hash you are trying to add is not a valid hash.
 * ECHILD: Attempt to allow a specific first argument of a subcommand
 */
int ACLSetUser(user *u, const char *op, ssize_t oplen) {
    /* as we are changing the ACL, the old generated string is now invalid */
    if (u->acl_string) {
        decrRefCount(u->acl_string);
        u->acl_string = NULL;
    }

    if (oplen == -1) oplen = strlen(op);
    if (oplen == 0) return C_OK; /* Empty string is a no-operation. */
    if (!strcasecmp(op,"on")) {
        u->flags |= USER_FLAG_ENABLED;
        u->flags &= ~USER_FLAG_DISABLED;
    } else if (!strcasecmp(op,"off")) {
        u->flags |= USER_FLAG_DISABLED;
        u->flags &= ~USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"skip-sanitize-payload")) {
        u->flags |= USER_FLAG_SANITIZE_PAYLOAD_SKIP;
        u->flags &= ~USER_FLAG_SANITIZE_PAYLOAD;
    } else if (!strcasecmp(op,"sanitize-payload")) {
        u->flags &= ~USER_FLAG_SANITIZE_PAYLOAD_SKIP;
        u->flags |= USER_FLAG_SANITIZE_PAYLOAD;
    } else if (!strcasecmp(op,"nopass")) {
        u->flags |= USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (!strcasecmp(op,"resetpass")) {
        u->flags &= ~USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (op[0] == '>' || op[0] == '#') {
        sds newpass;
        if (op[0] == '>') {
            newpass = ACLHashPassword((unsigned char*)op+1,oplen-1);
        } else {
            if (ACLCheckPasswordHash((unsigned char*)op+1,oplen-1) == C_ERR) {
                errno = EBADMSG;
                return C_ERR;
            }
            newpass = sdsnewlen(op+1,oplen-1);
        }

        listNode *ln = listSearchKey(u->passwords,newpass);
        /* Avoid re-adding the same password multiple times. */
        if (ln == NULL)
            listAddNodeTail(u->passwords,newpass);
        else
            sdsfree(newpass);
        u->flags &= ~USER_FLAG_NOPASS;
    } else if (op[0] == '<' || op[0] == '!') {
        sds delpass;
        if (op[0] == '<') {
            delpass = ACLHashPassword((unsigned char*)op+1,oplen-1);
        } else {
            if (ACLCheckPasswordHash((unsigned char*)op+1,oplen-1) == C_ERR) {
                errno = EBADMSG;
                return C_ERR;
            }
            delpass = sdsnewlen(op+1,oplen-1);
        }
        listNode *ln = listSearchKey(u->passwords,delpass);
        sdsfree(delpass);
        if (ln) {
            listDelNode(u->passwords,ln);
        } else {
            errno = ENODEV;
            return C_ERR;
        }
    } else if (op[0] == '(' && op[oplen - 1] == ')') {
        aclSelector *selector = aclCreateSelectorFromOpSet(op, oplen);
        if (!selector) {
            /* No errorno set, propagate it from interior error. */
            return C_ERR;
        }
        listAddNodeTail(u->selectors, selector);
        return C_OK;
    } else if (!strcasecmp(op,"clearselectors")) {
        listIter li;
        listNode *ln;
        listRewind(u->selectors,&li);
        /* There has to be a root selector */
        serverAssert(listNext(&li));
        while((ln = listNext(&li))) {
            listDelNode(u->selectors, ln);
        }
        return C_OK;
    } else if (!strcasecmp(op,"reset")) {
        serverAssert(ACLSetUser(u,"resetpass",-1) == C_OK);
        serverAssert(ACLSetUser(u,"resetkeys",-1) == C_OK);
        serverAssert(ACLSetUser(u,"resetchannels",-1) == C_OK);
        if (server.acl_pubsub_default & SELECTOR_FLAG_ALLCHANNELS)
            serverAssert(ACLSetUser(u,"allchannels",-1) == C_OK);
        serverAssert(ACLSetUser(u,"off",-1) == C_OK);
        serverAssert(ACLSetUser(u,"sanitize-payload",-1) == C_OK);
        serverAssert(ACLSetUser(u,"clearselectors",-1) == C_OK);
        serverAssert(ACLSetUser(u,"-@all",-1) == C_OK);
    } else {
        aclSelector *selector = ACLUserGetRootSelector(u);
        if (ACLSetSelector(selector, op, oplen) == C_ERR) {
            return C_ERR;
        }
    }
    return C_OK;
}

/* Return a description of the error that occurred in ACLSetUser() according to
 * the errno value set by the function on error. */
const char *ACLSetUserStringError(void) {
    const char *errmsg = "Wrong format";
    if (errno == ENOENT)
        errmsg = "Unknown command or category name in ACL";
    else if (errno == EINVAL)
        errmsg = "Syntax error";
    else if (errno == EEXIST)
        errmsg = "Adding a pattern after the * pattern (or the "
                 "'allkeys' flag) is not valid and does not have any "
                 "effect. Try 'resetkeys' to start with an empty "
                 "list of patterns";
    else if (errno == EISDIR)
        errmsg = "Adding a pattern after the * pattern (or the "
                 "'allchannels' flag) is not valid and does not have any "
                 "effect. Try 'resetchannels' to start with an empty "
                 "list of channels";
    else if (errno == ENODEV)
        errmsg = "The password you are trying to remove from the user does "
                 "not exist";
    else if (errno == EBADMSG)
        errmsg = "The password hash must be exactly 64 characters and contain "
                 "only lowercase hexadecimal characters";
    else if (errno == EALREADY)
        errmsg = "Duplicate user found. A user can only be defined once in "
                 "config files";
    else if (errno == ECHILD)
        errmsg = "Allowing first-arg of a subcommand is not supported";
    return errmsg;
}

/* Create the default user, this has special permissions. */
user *ACLCreateDefaultUser(void) {
    user *new = ACLCreateUser("default",7);
    ACLSetUser(new,"+@all",-1);
    ACLSetUser(new,"~*",-1);
    ACLSetUser(new,"&*",-1);
    ACLSetUser(new,"on",-1);
    ACLSetUser(new,"nopass",-1);
    return new;
}

/* Initialization of the ACL subsystem. */
void ACLInit(void) {
    Users = raxNew();
    UsersToLoad = listCreate();
    ACLInitCommandCategories();
    listSetMatchMethod(UsersToLoad, ACLListMatchLoadedUser);
    ACLLog = listCreate();
    DefaultUser = ACLCreateDefaultUser();
}

/* Check the username and password pair and return C_OK if they are valid,
 * otherwise C_ERR is returned and errno is set to:
 *
 *  EINVAL: if the username-password do not match.
 *  ENOENT: if the specified user does not exist at all.
 */
int ACLCheckUserCredentials(robj *username, robj *password) {
    user *u = ACLGetUserByName(username->ptr,sdslen(username->ptr));
    if (u == NULL) {
        errno = ENOENT;
        return C_ERR;
    }

    /* Disabled users can't login. */
    if (u->flags & USER_FLAG_DISABLED) {
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
    sds hashed = ACLHashPassword(password->ptr,sdslen(password->ptr));
    while((ln = listNext(&li))) {
        sds thispass = listNodeValue(ln);
        if (!time_independent_strcmp(hashed, thispass, HASH_PASSWORD_LEN)) {
            sdsfree(hashed);
            return C_OK;
        }
    }
    sdsfree(hashed);

    /* If we reached this point, no password matched. */
    errno = EINVAL;
    return C_ERR;
}

/* If `err` is provided, this is added as an error reply to the client.
 * Otherwise, the standard Auth error is added as a reply. */
void addAuthErrReply(client *c, robj *err) {
    if (clientHasPendingReplies(c)) return;
    if (!err) {
        addReplyError(c, "-WRONGPASS invalid username-password pair or user is disabled.");
        return;
    }
    addReplyError(c, err->ptr);
}

/* This is like ACLCheckUserCredentials(), however if the user/pass
 * are correct, the connection is put in authenticated state and the
 * connection user reference is populated.
 *
 * The return value is AUTH_OK on success (valid username / password pair) & AUTH_ERR otherwise. */
int checkPasswordBasedAuth(client *c, robj *username, robj *password) {
    if (ACLCheckUserCredentials(username,password) == C_OK) {
        c->authenticated = 1;
        c->user = ACLGetUserByName(username->ptr,sdslen(username->ptr));
        moduleNotifyUserChanged(c);
        return AUTH_OK;
    } else {
        addACLLogEntry(c,ACL_DENIED_AUTH,(c->flags & CLIENT_MULTI) ? ACL_LOG_CTX_MULTI : ACL_LOG_CTX_TOPLEVEL,0,username->ptr,NULL);
        return AUTH_ERR;
    }
}

/* Attempt authenticating the user - first through module based authentication,
 * and then, if needed, with normal password based authentication.
 * Returns one of the following codes:
 * AUTH_OK - Indicates that authentication succeeded.
 * AUTH_ERR - Indicates that authentication failed.
 * AUTH_BLOCKED - Indicates module authentication is in progress through a blocking implementation.
 */
int ACLAuthenticateUser(client *c, robj *username, robj *password, robj **err) {
    int result = checkModuleAuthentication(c, username, password, err);
    /* If authentication was not handled by any Module, attempt normal password based auth. */
    if (result == AUTH_NOT_HANDLED) {
        result = checkPasswordBasedAuth(c, username, password);
    }
    return result;
}

/* For ACL purposes, every user has a bitmap with the commands that such
 * user is allowed to execute. In order to populate the bitmap, every command
 * should have an assigned ID (that is used to index the bitmap). This function
 * creates such an ID: it uses sequential IDs, reusing the same ID for the same
 * command name, so that a command retains the same ID in case of modules that
 * are unloaded and later reloaded.
 *
 * The function does not take ownership of the 'cmdname' SDS string.
 * */
unsigned long ACLGetCommandID(sds cmdname) {
    sds lowername = sdsdup(cmdname);
    sdstolower(lowername);
    if (commandId == NULL) commandId = raxNew();
    void *id;
    if (raxFind(commandId,(unsigned char*)lowername,sdslen(lowername),&id)) {
        sdsfree(lowername);
        return (unsigned long)id;
    }
    raxInsert(commandId,(unsigned char*)lowername,strlen(lowername),
              (void*)nextid,NULL);
    sdsfree(lowername);
    unsigned long thisid = nextid;
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
    return thisid;
}

/* Clear command id table and reset nextid to 0. */
void ACLClearCommandID(void) {
    if (commandId) raxFree(commandId);
    commandId = NULL;
    nextid = 0;
}

/* Return an username by its name, or NULL if the user does not exist. */
user *ACLGetUserByName(const char *name, size_t namelen) {
    void *myuser = NULL;
    raxFind(Users,(unsigned char*)name,namelen,&myuser);
    return myuser;
}

/* =============================================================================
 * ACL permission checks
 * ==========================================================================*/

/* Check if the key can be accessed by the selector.
 *
 * If the selector can access the key, ACL_OK is returned, otherwise
 * ACL_DENIED_KEY is returned. */
static int ACLSelectorCheckKey(aclSelector *selector, const char *key, int keylen, int keyspec_flags) {
    /* The selector can access any key */
    if (selector->flags & SELECTOR_FLAG_ALLKEYS) return ACL_OK;

    listIter li;
    listNode *ln;
    listRewind(selector->patterns,&li);

    int key_flags = 0;
    if (keyspec_flags & CMD_KEY_ACCESS) key_flags |= ACL_READ_PERMISSION;
    if (keyspec_flags & CMD_KEY_INSERT) key_flags |= ACL_WRITE_PERMISSION;
    if (keyspec_flags & CMD_KEY_DELETE) key_flags |= ACL_WRITE_PERMISSION;
    if (keyspec_flags & CMD_KEY_UPDATE) key_flags |= ACL_WRITE_PERMISSION;

    /* Is given key represent a prefix of a set of keys */
    int prefix = keyspec_flags & CMD_KEY_PREFIX;

    /* Test this key against every pattern. */
    while((ln = listNext(&li))) {
        keyPattern *pattern = listNodeValue(ln);
        if ((pattern->flags & key_flags) != key_flags)
            continue;
        size_t plen = sdslen(pattern->pattern);
        if (prefix) {
            if (prefixmatch(pattern->pattern,plen,key,keylen,0))
                return ACL_OK;
        } else {
            if (stringmatchlen(pattern->pattern, plen, key, keylen, 0))
                return ACL_OK;
        }
    }
    return ACL_DENIED_KEY;
}

/* Checks if the provided selector selector has access specified in flags
 * to all keys in the keyspace. For example, CMD_KEY_READ access requires either
 * '%R~*', '~*', or allkeys to be granted to the selector. Returns 1 if all 
 * the access flags are satisfied with this selector or 0 otherwise.
 */
static int ACLSelectorHasUnrestrictedKeyAccess(aclSelector *selector, int flags) {
    /* The selector can access any key */
    if (selector->flags & SELECTOR_FLAG_ALLKEYS) return 1;

    listIter li;
    listNode *ln;
    listRewind(selector->patterns,&li);

    int access_flags = 0;
    if (flags & CMD_KEY_ACCESS) access_flags |= ACL_READ_PERMISSION;
    if (flags & CMD_KEY_INSERT) access_flags |= ACL_WRITE_PERMISSION;
    if (flags & CMD_KEY_DELETE) access_flags |= ACL_WRITE_PERMISSION;
    if (flags & CMD_KEY_UPDATE) access_flags |= ACL_WRITE_PERMISSION;

    /* Test this key against every pattern. */
    while((ln = listNext(&li))) {
        keyPattern *pattern = listNodeValue(ln);
        if ((pattern->flags & access_flags) != access_flags)
            continue;
        if (!strcmp(pattern->pattern,"*")) {
           return 1;
       }
    }
    return 0;
}

/* Checks a channel against a provided list of channels. The is_pattern 
 * argument should only be used when subscribing (not when publishing)
 * and controls whether the input channel is evaluated as a channel pattern
 * (like in PSUBSCRIBE) or a plain channel name (like in SUBSCRIBE). 
 * 
 * Note that a plain channel name like in PUBLISH or SUBSCRIBE can be
 * matched against ACL channel patterns, but the pattern provided in PSUBSCRIBE
 * can only be matched as a literal against an ACL pattern (using plain string compare). */
static int ACLCheckChannelAgainstList(list *reference, const char *channel, int channellen, int is_pattern) {
    listIter li;
    listNode *ln;

    listRewind(reference, &li);
    while((ln = listNext(&li))) {
        sds pattern = listNodeValue(ln);
        size_t plen = sdslen(pattern);
        /* Channel patterns are matched literally against the channels in
         * the list. Regular channels perform pattern matching. */
        if ((is_pattern && !strcmp(pattern,channel)) || 
            (!is_pattern && stringmatchlen(pattern,plen,channel,channellen,0)))
        {
            return ACL_OK;
        }
    }
    return ACL_DENIED_CHANNEL;
}

/* To prevent duplicate calls to getKeysResult, a cache is maintained
 * in between calls to the various selectors. */
typedef struct {
    int keys_init;
    getKeysResult keys;
} aclKeyResultCache;

void initACLKeyResultCache(aclKeyResultCache *cache) {
    cache->keys_init = 0;
}

void cleanupACLKeyResultCache(aclKeyResultCache *cache) {
    if (cache->keys_init) getKeysFreeResult(&(cache->keys));
}

/* Check if the command is ready to be executed according to the
 * ACLs associated with the specified selector.
 *
 * If the selector can execute the command ACL_OK is returned, otherwise
 * ACL_DENIED_CMD, ACL_DENIED_KEY, or ACL_DENIED_CHANNEL is returned: the first in case the
 * command cannot be executed because the selector is not allowed to run such
 * command, the second and third if the command is denied because the selector is trying
 * to access a key or channel that are not among the specified patterns. */
static int ACLSelectorCheckCmd(aclSelector *selector, struct redisCommand *cmd, robj **argv, int argc, int *keyidxptr, aclKeyResultCache *cache) {
    uint64_t id = cmd->id;
    int ret;
    if (!(selector->flags & SELECTOR_FLAG_ALLCOMMANDS) && !(cmd->flags & CMD_NO_AUTH)) {
        /* If the bit is not set we have to check further, in case the
         * command is allowed just with that specific first argument. */
        if (ACLGetSelectorCommandBit(selector,id) == 0) {
            /* Check if the first argument matches. */
            if (argc < 2 ||
                selector->allowed_firstargs == NULL ||
                selector->allowed_firstargs[id] == NULL)
            {
                return ACL_DENIED_CMD;
            }

            long subid = 0;
            while (1) {
                if (selector->allowed_firstargs[id][subid] == NULL)
                    return ACL_DENIED_CMD;
                int idx = cmd->parent ? 2 : 1;
                if (!strcasecmp(argv[idx]->ptr,selector->allowed_firstargs[id][subid]))
                    break; /* First argument match found. Stop here. */
                subid++;
            }
        }
    }

    /* Check if the user can execute commands explicitly touching the keys
     * mentioned in the command arguments. */
    if (!(selector->flags & SELECTOR_FLAG_ALLKEYS) && doesCommandHaveKeys(cmd)) {
        if (!(cache->keys_init)) {
            cache->keys = (getKeysResult) GETKEYS_RESULT_INIT;
            getKeysFromCommandWithSpecs(cmd, argv, argc, GET_KEYSPEC_DEFAULT, &(cache->keys));
            cache->keys_init = 1;
        }
        getKeysResult *result = &(cache->keys);
        keyReference *resultidx = result->keys;
        for (int j = 0; j < result->numkeys; j++) {
            int idx = resultidx[j].pos;
            ret = ACLSelectorCheckKey(selector, argv[idx]->ptr, sdslen(argv[idx]->ptr), resultidx[j].flags);
            if (ret != ACL_OK) {
                if (keyidxptr) *keyidxptr = resultidx[j].pos;
                return ret;
            }
        }
    }

    /* Check if the user can execute commands explicitly touching the channels
     * mentioned in the command arguments */
    const int channel_flags = CMD_CHANNEL_PUBLISH | CMD_CHANNEL_SUBSCRIBE;
    if (!(selector->flags & SELECTOR_FLAG_ALLCHANNELS) && doesCommandHaveChannelsWithFlags(cmd, channel_flags)) {
        getKeysResult channels = (getKeysResult) GETKEYS_RESULT_INIT;
        getChannelsFromCommand(cmd, argv, argc, &channels);
        keyReference *channelref = channels.keys;
        for (int j = 0; j < channels.numkeys; j++) {
            int idx = channelref[j].pos;
            if (!(channelref[j].flags & channel_flags)) continue;
            int is_pattern = channelref[j].flags & CMD_CHANNEL_PATTERN;
            int ret = ACLCheckChannelAgainstList(selector->channels, argv[idx]->ptr, sdslen(argv[idx]->ptr), is_pattern);
            if (ret != ACL_OK) {
                if (keyidxptr) *keyidxptr = channelref[j].pos;
                getKeysFreeResult(&channels);
                return ret;
            }
        }
        getKeysFreeResult(&channels);
    }
    return ACL_OK;
}

/* Check if the key can be accessed by the client according to
 * the ACLs associated with the specified user according to the
 * keyspec access flags.
 *
 * If the user can access the key, ACL_OK is returned, otherwise
 * ACL_DENIED_KEY is returned. */
int ACLUserCheckKeyPerm(user *u, const char *key, int keylen, int flags) {
    listIter li;
    listNode *ln;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* Check all of the selectors */
    listRewind(u->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        if (ACLSelectorCheckKey(s, key, keylen, flags) == ACL_OK) {
            return ACL_OK;
        }
    }
    return ACL_DENIED_KEY;
}

/* Checks if the user can execute the given command with the added restriction
 * it must also have the access specified in flags to any key in the key space. 
 * For example, CMD_KEY_READ access requires either '%R~*', '~*', or allkeys to be 
 * granted in addition to the access required by the command. Returns 1 
 * if the user has access or 0 otherwise.
 */
int ACLUserCheckCmdWithUnrestrictedKeyAccess(user *u, struct redisCommand *cmd, robj **argv, int argc, int flags) {
    listIter li;
    listNode *ln;
    int local_idxptr;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return 1;

    /* For multiple selectors, we cache the key result in between selector
     * calls to prevent duplicate lookups. */
    aclKeyResultCache cache;
    initACLKeyResultCache(&cache);

    /* Check each selector sequentially */
    listRewind(u->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        int acl_retval = ACLSelectorCheckCmd(s, cmd, argv, argc, &local_idxptr, &cache);
        if (acl_retval == ACL_OK && ACLSelectorHasUnrestrictedKeyAccess(s, flags)) {
            cleanupACLKeyResultCache(&cache);
            return 1;
        }
    }
    cleanupACLKeyResultCache(&cache);
    return 0;
}

/* Check if the channel can be accessed by the client according to
 * the ACLs associated with the specified user.
 *
 * If the user can access the key, ACL_OK is returned, otherwise
 * ACL_DENIED_CHANNEL is returned. */
int ACLUserCheckChannelPerm(user *u, sds channel, int is_pattern) {
    listIter li;
    listNode *ln;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* Check all of the selectors */
    listRewind(u->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        /* The selector can run any keys */
        if (s->flags & SELECTOR_FLAG_ALLCHANNELS) return ACL_OK;

        /* Otherwise, loop over the selectors list and check each channel */
        if (ACLCheckChannelAgainstList(s->channels, channel, sdslen(channel), is_pattern) == ACL_OK) {
            return ACL_OK;
        }
    }
    return ACL_DENIED_CHANNEL;
}

/* Lower level API that checks if a specified user is able to execute a given command.
 *
 * If the command fails an ACL check, idxptr will be to set to the first argv entry that
 * causes the failure, either 0 if the command itself fails or the idx of the key/channel
 * that causes the failure */
int ACLCheckAllUserCommandPerm(user *u, struct redisCommand *cmd, robj **argv, int argc, int *idxptr) {
    listIter li;
    listNode *ln;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* We have to pick a single error to log, the logic for picking is as follows:
     * 1) If no selector can execute the command, return the command.
     * 2) Return the last key or channel that no selector could match. */
    int relevant_error = ACL_DENIED_CMD;
    int local_idxptr = 0, last_idx = 0;

    /* For multiple selectors, we cache the key result in between selector
     * calls to prevent duplicate lookups. */
    aclKeyResultCache cache;
    initACLKeyResultCache(&cache);

    /* Check each selector sequentially */
    listRewind(u->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        int acl_retval = ACLSelectorCheckCmd(s, cmd, argv, argc, &local_idxptr, &cache);
        if (acl_retval == ACL_OK) {
            cleanupACLKeyResultCache(&cache);
            return ACL_OK;
        }
        if (acl_retval > relevant_error ||
            (acl_retval == relevant_error && local_idxptr > last_idx))
        {
            relevant_error = acl_retval;
            last_idx = local_idxptr;
        }
    }

    *idxptr = last_idx;
    cleanupACLKeyResultCache(&cache);
    return relevant_error;
}

/* High level API for checking if a client can execute the queued up command */
int ACLCheckAllPerm(client *c, int *idxptr) {
    return ACLCheckAllUserCommandPerm(c->user, c->cmd, c->argv, c->argc, idxptr);
}

/* If 'new' can access all channels 'original' could then return NULL;
   Otherwise return a list of channels that the new user can access */
list *getUpcomingChannelList(user *new, user *original) {
    listIter li, lpi;
    listNode *ln, *lpn;

    /* Optimization: we check if any selector has all channel permissions. */
    listRewind(new->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        if (s->flags & SELECTOR_FLAG_ALLCHANNELS) return NULL;
    }

    /* Next, check if the new list of channels
     * is a strict superset of the original. This is done by
     * created an "upcoming" list of all channels that are in
     * the new user and checking each of the existing channels
     * against it.  */
    list *upcoming = listCreate();
    listRewind(new->selectors,&li);
    while((ln = listNext(&li))) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        listRewind(s->channels, &lpi);
        while((lpn = listNext(&lpi))) {
            listAddNodeTail(upcoming, listNodeValue(lpn));
        }
    }

    int match = 1;
    listRewind(original->selectors,&li);
    while((ln = listNext(&li)) && match) {
        aclSelector *s = (aclSelector *) listNodeValue(ln);
        /* If any of the original selectors has the all-channels permission, but
         * the new ones don't (this is checked earlier in this function), then the
         * new list is not a strict superset of the original.  */
        if (s->flags & SELECTOR_FLAG_ALLCHANNELS) {
            match = 0;
            break;
        }
        listRewind(s->channels, &lpi);
        while((lpn = listNext(&lpi)) && match) {
            if (!listSearchKey(upcoming, listNodeValue(lpn))) {
                match = 0;
                break;
            }
        }
    }

    if (match) {
        /* All channels were matched, no need to kill clients. */
        listRelease(upcoming);
        return NULL;
    }

    return upcoming;
}

/* Check if the client should be killed because it is subscribed to channels that were
 * permitted in the past, are not in the `upcoming` channel list. */
int ACLShouldKillPubsubClient(client *c, list *upcoming) {
    robj *o;
    int kill = 0;

    if (getClientType(c) == CLIENT_TYPE_PUBSUB) {
        /* Check for pattern violations. */
        dictIterator di;
        dictEntry *de;
        dictInitIterator(&di, c->pubsub_patterns);
        while (!kill && ((de = dictNext(&di)) != NULL)) {
            o = dictGetKey(de);
            int res = ACLCheckChannelAgainstList(upcoming, o->ptr, sdslen(o->ptr), 1);
            kill = (res == ACL_DENIED_CHANNEL);
        }
        dictResetIterator(&di);

        /* Check for channel violations. */
        if (!kill) {
            /* Check for global channels violation. */
            dictInitIterator(&di, c->pubsub_channels);

            while (!kill && ((de = dictNext(&di)) != NULL)) {
                o = dictGetKey(de);
                int res = ACLCheckChannelAgainstList(upcoming, o->ptr, sdslen(o->ptr), 0);
                kill = (res == ACL_DENIED_CHANNEL);
            }
            dictResetIterator(&di);
        }
        if (!kill) {
            /* Check for shard channels violation. */
            dictInitIterator(&di, c->pubsubshard_channels);
            while (!kill && ((de = dictNext(&di)) != NULL)) {
                o = dictGetKey(de);
                int res = ACLCheckChannelAgainstList(upcoming, o->ptr, sdslen(o->ptr), 0);
                kill = (res == ACL_DENIED_CHANNEL);
            }
            dictResetIterator(&di);
        }

        if (kill) {
            return 1;
        }
    }
    return 0;
}

/* Check if the user's existing pub/sub clients violate the ACL pub/sub
 * permissions specified via the upcoming argument, and kill them if so. */
void ACLKillPubsubClientsIfNeeded(user *new, user *original) {
    /* Do nothing if there are no subscribers. */
    if (pubsubTotalSubscriptions() == 0)
        return;

    list *channels = getUpcomingChannelList(new, original);
    /* If the new user's pubsub permissions are a strict superset of the original, return early. */
    if (!channels)
        return;

    listIter li;
    listNode *ln;

    /* Permissions have changed, so we need to iterate through all
     * the clients and disconnect those that are no longer valid.
     * Scan all connected clients to find the user's pub/subs. */
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->user != original)
            continue;
        if (ACLShouldKillPubsubClient(c, channels))
            deauthenticateAndCloseClient(c);
    }

    listRelease(channels);
}

/* =============================================================================
 * ACL loading / saving functions
 * ==========================================================================*/


/* Selector definitions should be sent as a single argument, however
 * we will be lenient and try to find selector definitions spread 
 * across multiple arguments since it makes for a simpler user experience
 * for ACL SETUSER as well as when loading from conf files. 
 * 
 * This function takes in an array of ACL operators, excluding the username,
 * and merges selector operations that are spread across multiple arguments. The return
 * value is a new SDS array, with length set to the passed in merged_argc. Arguments 
 * that are untouched are still duplicated. If there is an unmatched parenthesis, NULL 
 * is returned and invalid_idx is set to the argument with the start of the opening
 * parenthesis. */
sds *ACLMergeSelectorArguments(sds *argv, int argc, int *merged_argc, int *invalid_idx) {
    *merged_argc = 0;
    int open_bracket_start = -1;

    sds *acl_args = (sds *) zmalloc(sizeof(sds) * argc);

    sds selector = NULL;
    for (int j = 0; j < argc; j++) {
        char *op = argv[j];

        if (open_bracket_start == -1 &&
            (op[0] == '(' && op[sdslen(op) - 1] != ')')) {
            selector = sdsdup(argv[j]);
            open_bracket_start = j;
            continue;
        }

        if (open_bracket_start != -1) {
            selector = sdscatfmt(selector, " %s", op);
            if (op[sdslen(op) - 1] == ')') {
                open_bracket_start = -1;
                acl_args[*merged_argc] = selector;                        
                (*merged_argc)++;
            }
            continue;
        }

        acl_args[*merged_argc] = sdsdup(argv[j]);
        (*merged_argc)++;
    }

    if (open_bracket_start != -1) {
        for (int i = 0; i < *merged_argc; i++) sdsfree(acl_args[i]);
        zfree(acl_args);
        sdsfree(selector);
        if (invalid_idx) *invalid_idx = open_bracket_start;
        return NULL;
    }

    return acl_args;
}

/* takes an acl string already split on spaces and adds it to the given user
 * if the user object is NULL, will create a user with the given username
 *
 * Returns an error as an sds string if the ACL string is not parsable
 */
sds ACLStringSetUser(user *u, sds username, sds *argv, int argc) {
    serverAssert(u != NULL || username != NULL);

    sds error = NULL;

    int merged_argc = 0, invalid_idx = 0;
    sds *acl_args = ACLMergeSelectorArguments(argv, argc, &merged_argc, &invalid_idx);

    if (!acl_args) {
        return sdscatfmt(sdsempty(),
                         "Unmatched parenthesis in acl selector starting "
                         "at '%s'.", (char *) argv[invalid_idx]);
    }

    /* Create a temporary user to validate and stage all changes against
     * before applying to an existing user or creating a new user. If all
     * arguments are valid the user parameters will all be applied together.
     * If there are any errors then none of the changes will be applied. */
    user *tempu = ACLCreateUnlinkedUser();
    if (u) {
        ACLCopyUser(tempu, u);
    }

    for (int j = 0; j < merged_argc; j++) {
        if (ACLSetUser(tempu,acl_args[j],(ssize_t) sdslen(acl_args[j])) != C_OK) {
            const char *errmsg = ACLSetUserStringError();
            error = sdscatfmt(sdsempty(),
                              "Error in ACL SETUSER modifier '%s': %s",
                              (char*)acl_args[j], errmsg);
            goto cleanup;
        }
    }

    /* Existing pub/sub clients authenticated with the user may need to be
     * disconnected if (some of) their channel permissions were revoked. */
    if (u) {
        ACLKillPubsubClientsIfNeeded(tempu, u);
    }

    /* Overwrite the user with the temporary user we modified above. */
    if (!u) {
        u = ACLCreateUser(username,sdslen(username));
    }
    serverAssert(u != NULL);

    ACLCopyUser(u, tempu);

cleanup:
    ACLFreeUser(tempu);
    for (int i = 0; i < merged_argc; i++) {
        sdsfree(acl_args[i]);
    }
    zfree(acl_args);

    return error;
}

/* Given an argument vector describing a user in the form:
 *
 *      user <username> ... ACL rules and flags ...
 *
 * this function validates, and if the syntax is valid, appends
 * the user definition to a list for later loading.
 *
 * The rules are tested for validity and if there obvious syntax errors
 * the function returns C_ERR and does nothing, otherwise C_OK is returned
 * and the user is appended to the list.
 *
 * Note that this function cannot stop in case of commands that are not found
 * and, in that case, the error will be emitted later, because certain
 * commands may be defined later once modules are loaded.
 *
 * When an error is detected and C_ERR is returned, the function populates
 * by reference (if not set to NULL) the argc_err argument with the index
 * of the argv vector that caused the error. */
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err) {
    if (argc < 2 || strcasecmp(argv[0],"user")) {
        if (argc_err) *argc_err = 0;
        return C_ERR;
    }

    if (listSearchKey(UsersToLoad, argv[1])) {
        if (argc_err) *argc_err = 1;
        errno = EALREADY;
        return C_ERR; 
    }

    /* Merged selectors before trying to process */
    int merged_argc;
    sds *acl_args = ACLMergeSelectorArguments(argv + 2, argc - 2, &merged_argc, argc_err);

    if (!acl_args) {
        return C_ERR;
    }

    /* Try to apply the user rules in a fake user to see if they
     * are actually valid. */
    user *fakeuser = ACLCreateUnlinkedUser();

    for (int j = 0; j < merged_argc; j++) {
        if (ACLSetUser(fakeuser,acl_args[j],sdslen(acl_args[j])) == C_ERR) {
            if (errno != ENOENT) {
                ACLFreeUser(fakeuser);
                if (argc_err) *argc_err = j;
                for (int i = 0; i < merged_argc; i++) sdsfree(acl_args[i]);
                zfree(acl_args);
                return C_ERR;
            }
        }
    }

    /* Rules look valid, let's append the user to the list. */
    sds *copy = zmalloc(sizeof(sds)*(merged_argc + 2));
    copy[0] = sdsdup(argv[1]);
    for (int j = 0; j < merged_argc; j++) copy[j+1] = sdsdup(acl_args[j]);
    copy[merged_argc + 1] = NULL;
    listAddNodeTail(UsersToLoad,copy);
    ACLFreeUser(fakeuser);
    for (int i = 0; i < merged_argc; i++) sdsfree(acl_args[i]);
    zfree(acl_args);
    return C_OK;
}

/* This function will load the configured users appended to the server
 * configuration via ACLAppendUserForLoading(). On loading errors it will
 * log an error and return C_ERR, otherwise C_OK will be returned. */
int ACLLoadConfiguredUsers(void) {
    listIter li;
    listNode *ln;
    listRewind(UsersToLoad,&li);
    while ((ln = listNext(&li)) != NULL) {
        sds *aclrules = listNodeValue(ln);
        sds username = aclrules[0];

        if (ACLStringHasSpaces(aclrules[0],sdslen(aclrules[0]))) {
            serverLog(LL_WARNING,"Spaces not allowed in ACL usernames");
            return C_ERR;
        }

        user *u = ACLCreateUser(username,sdslen(username));
        if (!u) {
            /* Only valid duplicate user is the default one. */
            serverAssert(!strcmp(username, "default"));
            u = ACLGetUserByName("default",7);
            ACLSetUser(u,"reset",-1);
        }

        /* Load every rule defined for this user. */
        for (int j = 1; aclrules[j]; j++) {
            if (ACLSetUser(u,aclrules[j],sdslen(aclrules[j])) != C_OK) {
                const char *errmsg = ACLSetUserStringError();
                serverLog(LL_WARNING,"Error loading ACL rule '%s' for "
                                     "the user named '%s': %s",
                          aclrules[j],aclrules[0],errmsg);
                return C_ERR;
            }
        }

        /* Having a disabled user in the configuration may be an error,
         * warn about it without returning any error to the caller. */
        if (u->flags & USER_FLAG_DISABLED) {
            serverLog(LL_NOTICE, "The user '%s' is disabled (there is no "
                                 "'on' modifier in the user description). Make "
                                 "sure this is not a configuration error.",
                      aclrules[0]);
        }
    }
    return C_OK;
}

/* This function loads the ACL from the specified filename: every line
 * is validated and should be either empty or in the format used to specify
 * users in the redis.conf configuration or in the ACL file, that is:
 *
 *  user <username> ... rules ...
 *
 * Note that this function considers comments starting with '#' as errors
 * because the ACL file is meant to be rewritten, and comments would be
 * lost after the rewrite. Yet empty lines are allowed to avoid being too
 * strict.
 *
 * One important part of implementing ACL LOAD, that uses this function, is
 * to avoid ending with broken rules if the ACL file is invalid for some
 * reason, so the function will attempt to validate the rules before loading
 * each user. For every line that will be found broken the function will
 * collect an error message.
 *
 * IMPORTANT: If there is at least a single error, nothing will be loaded
 * and the rules will remain exactly as they were.
 *
 * At the end of the process, if no errors were found in the whole file then
 * NULL is returned. Otherwise an SDS string describing in a single line
 * a description of all the issues found is returned. */
sds ACLLoadFromFile(const char *filename) {
    FILE *fp;
    char buf[1024];

    /* Open the ACL file. */
    if ((fp = fopen(filename,"r")) == NULL) {
        sds errors = sdscatprintf(sdsempty(),
            "Error loading ACLs, opening file '%s': %s",
            filename, strerror(errno));
        return errors;
    }

    /* Load the whole file as a single string in memory. */
    sds acls = sdsempty();
    while(fgets(buf,sizeof(buf),fp) != NULL)
        acls = sdscat(acls,buf);
    fclose(fp);

    /* Split the file into lines and attempt to load each line. */
    int totlines;
    sds *lines, errors = sdsempty();
    lines = sdssplitlen(acls,strlen(acls),"\n",1,&totlines);
    sdsfree(acls);

    /* We do all the loading in a fresh instance of the Users radix tree,
     * so if there are errors loading the ACL file we can rollback to the
     * old version. */
    rax *old_users = Users;
    Users = raxNew();

    /* Load each line of the file. */
    for (int i = 0; i < totlines; i++) {
        sds *argv;
        int argc;
        int linenum = i+1;

        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip blank lines */
        if (lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitlen(lines[i],sdslen(lines[i])," ",1,&argc);
        if (argv == NULL) {
            errors = sdscatprintf(errors,
                     "%s:%d: unbalanced quotes in acl line. ",
                     server.acl_filename, linenum);
            continue;
        }

        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* The line should start with the "user" keyword. */
        if (strcmp(argv[0],"user") || argc < 2) {
            errors = sdscatprintf(errors,
                     "%s:%d should start with user keyword followed "
                     "by the username. ", server.acl_filename,
                     linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Spaces are not allowed in usernames. */
        if (ACLStringHasSpaces(argv[1],sdslen(argv[1]))) {
            errors = sdscatprintf(errors,
                     "'%s:%d: username '%s' contains invalid characters. ",
                     server.acl_filename, linenum, argv[1]);
            sdsfreesplitres(argv,argc);
            continue;
        }

        user *u = ACLCreateUser(argv[1],sdslen(argv[1]));

        /* If the user already exists we assume it's an error and abort. */
        if (!u) {
            errors = sdscatprintf(errors,"WARNING: Duplicate user '%s' found on line %d. ", argv[1], linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Finally process the options and validate they can
         * be cleanly applied to the user. If any option fails
         * to apply, the other values won't be applied since
         * all the pending changes will get dropped. */
        int merged_argc;
        sds *acl_args = ACLMergeSelectorArguments(argv + 2, argc - 2, &merged_argc, NULL);
        if (!acl_args) {
            errors = sdscatprintf(errors,
                    "%s:%d: Unmatched parenthesis in selector definition.",
                    server.acl_filename, linenum);
        }

        int syntax_error = 0;
        for (int j = 0; j < merged_argc; j++) {
            acl_args[j] = sdstrim(acl_args[j],"\t\r\n");
            if (ACLSetUser(u,acl_args[j],sdslen(acl_args[j])) != C_OK) {
                const char *errmsg = ACLSetUserStringError();
                if (errno == ENOENT) {
                    /* For missing commands, we print out more information since
                     * it shouldn't contain any sensitive information. */
                    errors = sdscatprintf(errors,
                            "%s:%d: Error in applying operation '%s': %s. ",
                            server.acl_filename, linenum, acl_args[j], errmsg);
                } else if (syntax_error == 0) {
                    /* For all other errors, only print out the first error encountered
                     * since it might affect future operations. */
                    errors = sdscatprintf(errors,
                            "%s:%d: %s. ",
                            server.acl_filename, linenum, errmsg);
                    syntax_error = 1;
                }
            }
        }

        for (int i = 0; i < merged_argc; i++) sdsfree(acl_args[i]);
        zfree(acl_args);

        /* Apply the rule to the new users set only if so far there
         * are no errors, otherwise it's useless since we are going
         * to discard the new users set anyway. */
        if (sdslen(errors) != 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        sdsfreesplitres(argv,argc);
    }

    sdsfreesplitres(lines,totlines);

    /* Check if we found errors and react accordingly. */
    if (sdslen(errors) == 0) {
        /* The default user pointer is referenced in different places: instead
         * of replacing such occurrences it is much simpler to copy the new
         * default user configuration in the old one. */
        user *new_default = ACLGetUserByName("default",7);
        if (!new_default) {
            new_default = ACLCreateDefaultUser();
        }

        ACLCopyUser(DefaultUser,new_default);
        ACLFreeUser(new_default);
        raxInsert(Users,(unsigned char*)"default",7,DefaultUser,NULL);
        raxRemove(old_users,(unsigned char*)"default",7,NULL);

        /* If there are some subscribers, we need to check if we need to drop some clients. */
        rax *user_channels = NULL;
        if (pubsubTotalSubscriptions() > 0) {
            user_channels = raxNew();
        }

        listIter li;
        listNode *ln;

        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            /* a MASTER client can do everything (and user = NULL) so we can skip it */
            if (c->flags & CLIENT_MASTER)
                continue;
            user *original = c->user;
            list *channels = NULL;
            user *new = ACLGetUserByName(c->user->name, sdslen(c->user->name));
            if (new && user_channels) {
                if (!raxFind(user_channels, (unsigned char*)(new->name), sdslen(new->name), (void**)&channels)) {
                    channels = getUpcomingChannelList(new, original);
                    raxInsert(user_channels, (unsigned char*)(new->name), sdslen(new->name), channels, NULL);
                }
            }
            /* When the new channel list is NULL, it means the new user's channel list is a superset of the old user's list. */
            if (!new || (channels && ACLShouldKillPubsubClient(c, channels))) {
                deauthenticateAndCloseClient(c);
                continue;
            }
            c->user = new;
        }

        if (user_channels)
            raxFreeWithCallback(user_channels, listReleaseGeneric);
        raxFreeWithCallback(old_users, ACLFreeUserGeneric);
        sdsfree(errors);
        return NULL;
    } else {
        raxFreeWithCallback(Users, ACLFreeUserGeneric);
        Users = old_users;
        errors = sdscat(errors,"WARNING: ACL errors detected, no change to the previously active ACL rules was performed");
        return errors;
    }
}

/* Generate a copy of the ACLs currently in memory in the specified filename.
 * Returns C_OK on success or C_ERR if there was an error during the I/O.
 * When C_ERR is returned a log is produced with hints about the issue. */
int ACLSaveToFile(const char *filename) {
    sds acl = sdsempty();
    int fd = -1;
    sds tmpfilename = NULL;
    int retval = C_ERR;

    /* Let's generate an SDS string containing the new version of the
     * ACL file. */
    raxIterator ri;
    raxStart(&ri,Users);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        user *u = ri.data;
        /* Return information in the configuration file format. */
        sds user = sdsnew("user ");
        user = sdscatsds(user,u->name);
        user = sdscatlen(user," ",1);
        robj *descr = ACLDescribeUser(u);
        user = sdscatsds(user,descr->ptr);
        decrRefCount(descr);
        acl = sdscatsds(acl,user);
        acl = sdscatlen(acl,"\n",1);
        sdsfree(user);
    }
    raxStop(&ri);

    /* Create a temp file with the new content. */
    tmpfilename = sdsnew(filename);
    tmpfilename = sdscatfmt(tmpfilename,".tmp-%i-%I",
        (int) getpid(),commandTimeSnapshot());
    if ((fd = open(tmpfilename,O_WRONLY|O_CREAT,0644)) == -1) {
        serverLog(LL_WARNING,"Opening temp ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }

    /* Write it. */
    size_t offset = 0;
    while (offset < sdslen(acl)) {
        ssize_t written_bytes = write(fd,acl + offset,sdslen(acl) - offset);
        if (written_bytes <= 0) {
            if (errno == EINTR) continue;
            serverLog(LL_WARNING,"Writing ACL file for ACL SAVE: %s",
                strerror(errno));
            goto cleanup;
        }
        offset += written_bytes;
    }
    if (redis_fsync(fd) == -1) {
        serverLog(LL_WARNING,"Syncing ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }
    close(fd); fd = -1;

    /* Let's replace the new file with the old one. */
    if (rename(tmpfilename,filename) == -1) {
        serverLog(LL_WARNING,"Renaming ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }
    if (fsyncFileDir(filename) == -1) {
        serverLog(LL_WARNING,"Syncing ACL directory for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }
    sdsfree(tmpfilename); tmpfilename = NULL;
    retval = C_OK; /* If we reached this point, everything is fine. */

cleanup:
    if (fd != -1) close(fd);
    if (tmpfilename) unlink(tmpfilename);
    sdsfree(tmpfilename);
    sdsfree(acl);
    return retval;
}

/* This function is called once the server is already running, modules are
 * loaded, and we are ready to start, in order to load the ACLs either from
 * the pending list of users defined in redis.conf, or from the ACL file.
 * The function will just exit with an error if the user is trying to mix
 * both the loading methods. */
void ACLLoadUsersAtStartup(void) {
    if (server.acl_filename[0] != '\0' && listLength(UsersToLoad) != 0) {
        serverLog(LL_WARNING,
            "Configuring Redis with users defined in redis.conf and at "
            "the same setting an ACL file path is invalid. This setup "
            "is very likely to lead to configuration errors and security "
            "holes, please define either an ACL file or declare users "
            "directly in your redis.conf, but not both.");
        exit(1);
    }

    if (ACLLoadConfiguredUsers() == C_ERR) {
        serverLog(LL_WARNING,
            "Critical error while loading ACLs. Exiting.");
        exit(1);
    }

    if (server.acl_filename[0] != '\0') {
        sds errors = ACLLoadFromFile(server.acl_filename);
        if (errors) {
            serverLog(LL_WARNING,
                "Aborting Redis startup because of ACL errors: %s", errors);
            sdsfree(errors);
            exit(1);
        }
    }
}

/* =============================================================================
 * ACL log
 * ==========================================================================*/

#define ACL_LOG_GROUPING_MAX_TIME_DELTA 60000

/* This structure defines an entry inside the ACL log. */
typedef struct ACLLogEntry {
    uint64_t count;     /* Number of times this happened recently. */
    int reason;         /* Reason for denying the command. ACL_DENIED_*. */
    int context;        /* Toplevel, Lua or MULTI/EXEC? ACL_LOG_CTX_*. */
    sds object;         /* The key name or command name. */
    sds username;       /* User the client is authenticated with. */
    mstime_t ctime;     /* Milliseconds time of last update to this entry. */
    sds cinfo;          /* Client info (last client if updated). */
    long long entry_id;         /* The pair (entry_id, timestamp_created) is a unique identifier of this entry 
                                  * in case the node dies and is restarted, it can detect that if it's a new series. */
    mstime_t timestamp_created; /* UNIX time in milliseconds at the time of this entry's creation. */
} ACLLogEntry;

/* This function will check if ACL entries 'a' and 'b' are similar enough
 * that we should actually update the existing entry in our ACL log instead
 * of creating a new one. */
int ACLLogMatchEntry(ACLLogEntry *a, ACLLogEntry *b) {
    if (a->reason != b->reason) return 0;
    if (a->context != b->context) return 0;
    mstime_t delta = a->ctime - b->ctime;
    if (delta < 0) delta = -delta;
    if (delta > ACL_LOG_GROUPING_MAX_TIME_DELTA) return 0;
    if (sdscmp(a->object,b->object) != 0) return 0;
    if (sdscmp(a->username,b->username) != 0) return 0;
    return 1;
}

/* Release an ACL log entry. */
void ACLFreeLogEntry(void *leptr) {
    ACLLogEntry *le = leptr;
    sdsfree(le->object);
    sdsfree(le->username);
    sdsfree(le->cinfo);
    zfree(le);
}

/* Update the relevant counter by the reason */
void ACLUpdateInfoMetrics(int reason){
    if (reason == ACL_DENIED_AUTH) {
        server.acl_info.user_auth_failures++;
    } else if (reason == ACL_DENIED_CMD) {
        server.acl_info.invalid_cmd_accesses++;
    } else if (reason == ACL_DENIED_KEY) {
        server.acl_info.invalid_key_accesses++;
    } else if (reason == ACL_DENIED_CHANNEL) {
        server.acl_info.invalid_channel_accesses++;
    } else {
        serverPanic("Unknown ACL_DENIED encoding");
    }
}

static void trimACLLogEntriesToMaxLen(void) {
    while(listLength(ACLLog) > server.acllog_max_len) {
        listNode *ln = listLast(ACLLog);
        ACLLogEntry *le = listNodeValue(ln);
        ACLFreeLogEntry(le);
        listDelNode(ACLLog,ln);
    }
}

/* Adds a new entry in the ACL log, making sure to delete the old entry
 * if we reach the maximum length allowed for the log. This function attempts
 * to find similar entries in the current log in order to bump the counter of
 * the log entry instead of creating many entries for very similar ACL
 * rules issues.
 *
 * The argpos argument is used when the reason is ACL_DENIED_KEY or 
 * ACL_DENIED_CHANNEL, since it allows the function to log the key or channel
 * name that caused the problem.
 *
 * The last 2 arguments are a manual override to be used, instead of any of the automatic
 * ones which depend on the client and reason arguments (use NULL for default).
 *
 * If `object` is not NULL, this functions takes over it.
 */
void addACLLogEntry(client *c, int reason, int context, int argpos, sds username, sds object) {
    /* Update ACL info metrics */
    ACLUpdateInfoMetrics(reason);
    
    if (server.acllog_max_len == 0) {
        trimACLLogEntriesToMaxLen();
        return;
    }
    
    /* Create a new entry. */
    struct ACLLogEntry *le = zmalloc(sizeof(*le));
    le->count = 1;
    le->reason = reason;
    le->username = sdsdup(username ? username : c->user->name);
    le->ctime = commandTimeSnapshot();
    le->entry_id = ACLLogEntryCount;
    le->timestamp_created = le->ctime;

    if (object) {
        le->object = object;
    } else {
        switch(reason) {
            case ACL_DENIED_CMD: le->object = sdsdup(c->cmd->fullname); break;
            case ACL_DENIED_KEY: le->object = sdsdup(c->argv[argpos]->ptr); break;
            case ACL_DENIED_CHANNEL: le->object = sdsdup(c->argv[argpos]->ptr); break;
            case ACL_DENIED_AUTH: le->object = sdsdup(c->argv[0]->ptr); break;
            default: le->object = sdsempty();
        }
    }

    /* if we have a real client from the network, use it (could be missing on module timers) */
    client *realclient = server.current_client? server.current_client : c;

    le->cinfo = catClientInfoString(sdsempty(),realclient);
    le->context = context;

    /* Try to match this entry with past ones, to see if we can just
     * update an existing entry instead of creating a new one. */
    long toscan = 10; /* Do a limited work trying to find duplicated. */
    listIter li;
    listNode *ln;
    listRewind(ACLLog,&li);
    ACLLogEntry *match = NULL;
    while (toscan-- && (ln = listNext(&li)) != NULL) {
        ACLLogEntry *current = listNodeValue(ln);
        if (ACLLogMatchEntry(current,le)) {
            match = current;
            listDelNode(ACLLog,ln);
            listAddNodeHead(ACLLog,current);
            break;
        }
    }

    /* If there is a match update the entry, otherwise add it as a
     * new one. */
    if (match) {
        /* We update a few fields of the existing entry and bump the
         * counter of events for this entry. */
        sdsfree(match->cinfo);
        match->cinfo = le->cinfo;
        match->ctime = le->ctime;
        match->count++;

        /* Release the old entry. */
        le->cinfo = NULL;
        ACLFreeLogEntry(le);
    } else {
        /* Add it to our list of entries. We'll have to trim the list
         * to its maximum size. */
        ACLLogEntryCount++; /* Incrementing the entry_id count to make each record in the log unique. */
        listAddNodeHead(ACLLog, le);
        trimACLLogEntriesToMaxLen();
    }
}

sds getAclErrorMessage(int acl_res, user *user, struct redisCommand *cmd, sds errored_val, int verbose) {
    switch (acl_res) {
    case ACL_DENIED_CMD:
        return sdscatfmt(sdsempty(), "User %S has no permissions to run "
                                     "the '%S' command", user->name, cmd->fullname);
    case ACL_DENIED_KEY:
        if (verbose) {
            return sdscatfmt(sdsempty(), "User %S has no permissions to access "
                                         "the '%S' key", user->name, errored_val);
        } else {
            return sdsnew("No permissions to access a key");
        }
    case ACL_DENIED_CHANNEL:
        if (verbose) {
            return sdscatfmt(sdsempty(), "User %S has no permissions to access "
                                         "the '%S' channel", user->name, errored_val);
        } else {
            return sdsnew("No permissions to access a channel");
        }
    }
    serverPanic("Reached deadcode on getAclErrorMessage");
}

/* =============================================================================
 * ACL related commands
 * ==========================================================================*/

/* ACL CAT category */
void aclCatWithFlags(client *c, dict *commands, uint64_t cflag, int *arraylen) {
    dictEntry *de;
    dictIterator di;
    dictInitIterator(&di, commands);
    while ((de = dictNext(&di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->acl_categories & cflag) {
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            (*arraylen)++;
        }

        if (cmd->subcommands_dict) {
            aclCatWithFlags(c, cmd->subcommands_dict, cflag, arraylen);
        }
    }
    dictResetIterator(&di);
}

/* Add the formatted response from a single selector to the ACL GETUSER
 * response. This function returns the number of fields added. 
 * 
 * Setting verbose to 1 means that the full qualifier for key and channel
 * permissions are shown.
 */
int aclAddReplySelectorDescription(client *c, aclSelector *s) {
    listIter li;
    listNode *ln;

    /* Commands */
    addReplyBulkCString(c,"commands");
    sds cmddescr = ACLDescribeSelectorCommandRules(s);
    addReplyBulkSds(c,cmddescr);
    
    /* Key patterns */
    addReplyBulkCString(c,"keys");
    if (s->flags & SELECTOR_FLAG_ALLKEYS) {
        addReplyBulkCBuffer(c,"~*",2);
    } else {
        sds dsl = sdsempty();
        listRewind(s->patterns,&li);
        while((ln = listNext(&li))) {
            keyPattern *thispat = (keyPattern *) listNodeValue(ln);
            if (ln != listFirst(s->patterns)) dsl = sdscat(dsl, " ");
            dsl = sdsCatPatternString(dsl, thispat);
        }
        addReplyBulkSds(c, dsl);
    }

    /* Pub/sub patterns */
    addReplyBulkCString(c,"channels");
    if (s->flags & SELECTOR_FLAG_ALLCHANNELS) {
        addReplyBulkCBuffer(c,"&*",2);
    } else {
        sds dsl = sdsempty();
        listRewind(s->channels,&li);
        while((ln = listNext(&li))) {
            sds thispat = listNodeValue(ln);
            if (ln != listFirst(s->channels)) dsl = sdscat(dsl, " ");
            dsl = sdscatfmt(dsl, "&%S", thispat);
        }
        addReplyBulkSds(c, dsl);
    }
    return 3;
}

/* ACL -- show and modify the configuration of ACL users.
 * ACL HELP
 * ACL LOAD
 * ACL SAVE
 * ACL LIST
 * ACL USERS
 * ACL CAT [<category>]
 * ACL SETUSER <username> ... acl rules ...
 * ACL DELUSER <username> [...]
 * ACL GETUSER <username>
 * ACL GENPASS [<bits>]
 * ACL WHOAMI
 * ACL LOG [<count> | RESET]
 */
void aclCommand(client *c) {
    char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub,"setuser") && c->argc >= 3) {
        /* Initially redact all of the arguments to not leak any information
         * about the user. */
        for (int j = 2; j < c->argc; j++) {
            redactClientCommandArgument(c, j);
        }

        sds username = c->argv[2]->ptr;
        /* Check username validity. */
        if (ACLStringHasSpaces(username,sdslen(username))) {
            addReplyError(c, "Usernames can't contain spaces or null characters");
            return;
        }

        user *u = ACLGetUserByName(username,sdslen(username));

        sds *temp_argv = zmalloc(c->argc * sizeof(sds));
        for (int i = 3; i < c->argc; i++) temp_argv[i-3] = c->argv[i]->ptr;

        sds error = ACLStringSetUser(u, username, temp_argv, c->argc - 3);
        zfree(temp_argv);
        if (error == NULL) {
            addReply(c,shared.ok);
        } else {
            addReplyErrorSdsSafe(c, error);
        }
        return;
    } else if (!strcasecmp(sub,"deluser") && c->argc >= 3) {
        /* Initially redact all the arguments to not leak any information
         * about the users. */
        for (int j = 2; j < c->argc; j++) redactClientCommandArgument(c, j);

        int deleted = 0;
        for (int j = 2; j < c->argc; j++) {
            sds username = c->argv[j]->ptr;
            if (!strcmp(username,"default")) {
                addReplyError(c,"The 'default' user cannot be removed");
                return;
            }
        }

        for (int j = 2; j < c->argc; j++) {
            sds username = c->argv[j]->ptr;
            user *u;
            if (raxRemove(Users,(unsigned char*)username,
                          sdslen(username),
                          (void**)&u))
            {
                ACLFreeUserAndKillClients(u);
                deleted++;
            }
        }
        addReplyLongLong(c,deleted);
    } else if (!strcasecmp(sub,"getuser") && c->argc == 3) {
        /* Redact the username to not leak any information about the user. */
        redactClientCommandArgument(c, 2);

        user *u = ACLGetUserByName(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        if (u == NULL) {
            addReplyNull(c);
            return;
        }

        void *ufields = addReplyDeferredLen(c);
        int fields = 3;

        /* Flags */
        addReplyBulkCString(c,"flags");
        void *deflen = addReplyDeferredLen(c);
        int numflags = 0;
        for (int j = 0; ACLUserFlags[j].flag; j++) {
            if (u->flags & ACLUserFlags[j].flag) {
                addReplyBulkCString(c,ACLUserFlags[j].name);
                numflags++;
            }
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
        /* Include the root selector at the top level for backwards compatibility */
        fields += aclAddReplySelectorDescription(c, ACLUserGetRootSelector(u));

        /* Describe all of the selectors on this user, including duplicating the root selector */
        addReplyBulkCString(c,"selectors");
        addReplyArrayLen(c, listLength(u->selectors) - 1);
        listRewind(u->selectors,&li);
        serverAssert(listNext(&li));
        while((ln = listNext(&li))) {
            void *slen = addReplyDeferredLen(c);
            int sfields = aclAddReplySelectorDescription(c, (aclSelector *)listNodeValue(ln));
            setDeferredMapLen(c, slen, sfields);
        } 
        setDeferredMapLen(c, ufields, fields);
    } else if ((!strcasecmp(sub,"list") || !strcasecmp(sub,"users")) &&
               c->argc == 2)
    {
        int justnames = !strcasecmp(sub,"users");
        addReplyArrayLen(c,raxSize(Users));
        raxIterator ri;
        raxStart(&ri,Users);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            user *u = ri.data;
            if (justnames) {
                addReplyBulkCBuffer(c,u->name,sdslen(u->name));
            } else {
                /* Return information in the configuration file format. */
                sds config = sdsnew("user ");
                config = sdscatsds(config,u->name);
                config = sdscatlen(config," ",1);
                robj *descr = ACLDescribeUser(u);
                config = sdscatsds(config,descr->ptr);
                decrRefCount(descr);
                addReplyBulkSds(c,config);
            }
        }
        raxStop(&ri);
    } else if (!strcasecmp(sub,"whoami") && c->argc == 2) {
        if (c->user != NULL) {
            addReplyBulkCBuffer(c,c->user->name,sdslen(c->user->name));
        } else {
            addReplyNull(c);
        }
    } else if (server.acl_filename[0] == '\0' &&
               (!strcasecmp(sub,"load") || !strcasecmp(sub,"save")))
    {
        addReplyError(c,"This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.");
        return;
    } else if (!strcasecmp(sub,"load") && c->argc == 2) {
        sds errors = ACLLoadFromFile(server.acl_filename);
        if (errors == NULL) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,errors);
            sdsfree(errors);
        }
    } else if (!strcasecmp(sub,"save") && c->argc == 2) {
        if (ACLSaveToFile(server.acl_filename) == C_OK) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,"There was an error trying to save the ACLs. "
                            "Please check the server logs for more "
                            "information");
        }
    } else if (!strcasecmp(sub,"cat") && c->argc == 2) {
        void *dl = addReplyDeferredLen(c);
        int j;
        for (j = 0; ACLCommandCategories[j].flag != 0; j++)
            addReplyBulkCString(c,ACLCommandCategories[j].name);
        setDeferredArrayLen(c,dl,j);
    } else if (!strcasecmp(sub,"cat") && c->argc == 3) {
        uint64_t cflag = ACLGetCommandCategoryFlagByName(c->argv[2]->ptr);
        if (cflag == 0) {
            addReplyErrorFormat(c, "Unknown category '%.128s'", (char*)c->argv[2]->ptr);
            return;
        }
        int arraylen = 0;
        void *dl = addReplyDeferredLen(c);
        aclCatWithFlags(c, server.orig_commands, cflag, &arraylen);
        setDeferredArrayLen(c,dl,arraylen);
    } else if (!strcasecmp(sub,"genpass") && (c->argc == 2 || c->argc == 3)) {
        #define GENPASS_MAX_BITS 4096
        char pass[GENPASS_MAX_BITS/8*2]; /* Hex representation. */
        long bits = 256; /* By default generate 256 bits passwords. */

        if (c->argc == 3 && getLongFromObjectOrReply(c,c->argv[2],&bits,NULL)
            != C_OK) return;

        if (bits <= 0 || bits > GENPASS_MAX_BITS) {
            addReplyErrorFormat(c,
                "ACL GENPASS argument must be the number of "
                "bits for the output password, a positive number "
                "up to %d",GENPASS_MAX_BITS);
            return;
        }

        long chars = (bits+3)/4; /* Round to number of characters to emit. */
        getRandomHexChars(pass,chars);
        addReplyBulkCBuffer(c,pass,chars);
    } else if (!strcasecmp(sub,"log") && (c->argc == 2 || c->argc ==3)) {
        long count = 10; /* Number of entries to emit by default. */

        /* Parse the only argument that LOG may have: it could be either
         * the number of entries the user wants to display, or alternatively
         * the "RESET" command in order to flush the old entries. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"reset")) {
                listSetFreeMethod(ACLLog,ACLFreeLogEntry);
                listEmpty(ACLLog);
                listSetFreeMethod(ACLLog,NULL);
                addReply(c,shared.ok);
                return;
            } else if (getLongFromObjectOrReply(c,c->argv[2],&count,NULL)
                       != C_OK)
            {
                return;
            }
            if (count < 0) count = 0;
        }

        /* Fix the count according to the number of entries we got. */
        if ((size_t)count > listLength(ACLLog))
            count = listLength(ACLLog);

        addReplyArrayLen(c,count);
        listIter li;
        listNode *ln;
        listRewind(ACLLog,&li);
        mstime_t now = commandTimeSnapshot();
        while (count-- && (ln = listNext(&li)) != NULL) {
            ACLLogEntry *le = listNodeValue(ln);
            addReplyMapLen(c,10);
            addReplyBulkCString(c,"count");
            addReplyLongLong(c,le->count);

            addReplyBulkCString(c,"reason");
            char *reasonstr;
            switch(le->reason) {
            case ACL_DENIED_CMD: reasonstr="command"; break;
            case ACL_DENIED_KEY: reasonstr="key"; break;
            case ACL_DENIED_CHANNEL: reasonstr="channel"; break;
            case ACL_DENIED_AUTH: reasonstr="auth"; break;
            default: reasonstr="unknown";
            }
            addReplyBulkCString(c,reasonstr);

            addReplyBulkCString(c,"context");
            char *ctxstr;
            switch(le->context) {
            case ACL_LOG_CTX_TOPLEVEL: ctxstr="toplevel"; break;
            case ACL_LOG_CTX_MULTI: ctxstr="multi"; break;
            case ACL_LOG_CTX_LUA: ctxstr="lua"; break;
            case ACL_LOG_CTX_MODULE: ctxstr="module"; break;
            default: ctxstr="unknown";
            }
            addReplyBulkCString(c,ctxstr);

            addReplyBulkCString(c,"object");
            addReplyBulkCBuffer(c,le->object,sdslen(le->object));
            addReplyBulkCString(c,"username");
            addReplyBulkCBuffer(c,le->username,sdslen(le->username));
            addReplyBulkCString(c,"age-seconds");
            double age = (double)(now - le->ctime)/1000;
            addReplyDouble(c,age);
            addReplyBulkCString(c,"client-info");
            addReplyBulkCBuffer(c,le->cinfo,sdslen(le->cinfo));
            addReplyBulkCString(c, "entry-id");
            addReplyLongLong(c, le->entry_id);
            addReplyBulkCString(c, "timestamp-created");
            addReplyLongLong(c, le->timestamp_created);
            addReplyBulkCString(c, "timestamp-last-updated");
            addReplyLongLong(c, le->ctime);
        }
    } else if (!strcasecmp(sub,"dryrun") && c->argc >= 4) {
        struct redisCommand *cmd;
        user *u = ACLGetUserByName(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        if (u == NULL) {
            addReplyErrorFormat(c, "User '%s' not found", (char *)c->argv[2]->ptr);
            return;
        }

        if ((cmd = lookupCommand(c->argv + 3, c->argc - 3)) == NULL) {
            addReplyErrorFormat(c, "Command '%s' not found", (char *)c->argv[3]->ptr);
            return;
        }

        if ((cmd->arity > 0 && cmd->arity != c->argc-3) ||
            (c->argc-3 < -cmd->arity))
        {
            addReplyErrorFormat(c,"wrong number of arguments for '%s' command", cmd->fullname);
            return;
        }

        int idx;
        int result = ACLCheckAllUserCommandPerm(u, cmd, c->argv + 3, c->argc - 3, &idx);
        if (result != ACL_OK) {
            sds err = getAclErrorMessage(result, u, cmd,  c->argv[idx+3]->ptr, 1);
            addReplyBulkSds(c, err);
            return;
        }

        addReply(c,shared.ok);
    } else if (c->argc == 2 && !strcasecmp(sub,"help")) {
        const char *help[] = {
"CAT [<category>]",
"    List all commands that belong to <category>, or all command categories",
"    when no category is specified.",
"DELUSER <username> [<username> ...]",
"    Delete a list of users.",
"DRYRUN <username> <command> [<arg> ...]",
"    Returns whether the user can execute the given command without executing the command.",
"GETUSER <username>",
"    Get the user's details.",
"GENPASS [<bits>]",
"    Generate a secure 256-bit user password. The optional `bits` argument can",
"    be used to specify a different size.",
"LIST",
"    Show users details in config file format.",
"LOAD",
"    Reload users from the ACL file.",
"LOG [<count> | RESET]",
"    Show the ACL log entries.",
"SAVE",
"    Save the current config to the ACL file.",
"SETUSER <username> <attribute> [<attribute> ...]",
"    Create or modify a user with the specified attributes.",
"USERS",
"    List all the registered usernames.",
"WHOAMI",
"    Return the current connection username.",
NULL
        };
        addReplyHelp(c,help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void addReplyCommandCategories(client *c, struct redisCommand *cmd) {
    int flagcount = 0;
    void *flaglen = addReplyDeferredLen(c);
    for (int j = 0; ACLCommandCategories[j].flag != 0; j++) {
        if (cmd->acl_categories & ACLCommandCategories[j].flag) {
            addReplyStatusFormat(c, "@%s", ACLCommandCategories[j].name);
            flagcount++;
        }
    }
    setDeferredSetLen(c, flaglen, flagcount);
}

/* When successful, initiates an internal connection, that is able to execute
 * internal commands (see CMD_INTERNAL). */
static void internalAuth(client *c) {
    if (server.cluster == NULL) {
        addReplyError(c, "Cannot authenticate as an internal connection on non-cluster instances");
        return;
    }

    sds password = c->argv[2]->ptr;

    /* Get internal secret. */
    size_t len = -1;
    const char *internal_secret = clusterGetSecret(&len);
    if (sdslen(password) != len) {
        addReplyError(c, "-WRONGPASS invalid internal password");
        return;
    }
    if (!time_independent_strcmp((char *)internal_secret, (char *)password, len)) {
        c->flags |= CLIENT_INTERNAL;
        /* No further authentication is needed. */
        c->authenticated = 1;
        /* Set the user to the unrestricted user, if it is not already set (default). */
        if (c->user != NULL) {
            c->user = NULL;
            moduleNotifyUserChanged(c);
        }
        addReply(c, shared.ok);
    } else {
        addReplyError(c, "-WRONGPASS invalid internal password");
    }
}

/* AUTH <password>
 * AUTH <username> <password> (Redis >= 6.0 form)
 *
 * When the user is omitted it means that we are trying to authenticate
 * against the default user. */
void authCommand(client *c) {
    /* Only two or three argument forms are allowed. */
    if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }
    /* Always redact the second argument */
    redactClientCommandArgument(c, 1);

    /* Handle the two different forms here. The form with two arguments
     * will just use "default" as username. */
    robj *username, *password;
    if (c->argc == 2) {
        /* Mimic the old behavior of giving an error for the two argument
         * form if no password is configured. */
        if (DefaultUser->flags & USER_FLAG_NOPASS) {
            addReplyError(c,"AUTH <password> called without any password "
                            "configured for the default user. Are you sure "
                            "your configuration is correct?");
            return;
        }

        username = shared.default_username; 
        password = c->argv[1];
    } else {
        username = c->argv[1];
        password = c->argv[2];
        redactClientCommandArgument(c, 2);

        /* Handle internal authentication commands.
         * Note: No user-defined ACL user can have this username (no spaces
         * allowed), thus no conflicts with ACL possible. */
        if (!strcmp(username->ptr, "internal connection")) {
            internalAuth(c);
            return;
        }
    }

    robj *err = NULL;
    int result = ACLAuthenticateUser(c, username, password, &err);
    if (result == AUTH_OK) {
        addReply(c, shared.ok);
    } else if (result == AUTH_ERR) {
        addAuthErrReply(c, err);
    }
    if (err) decrRefCount(err);
}

/* Set the password for the "default" ACL user. This implements supports for
 * requirepass config, so passing in NULL will set the user to be nopass. */
void ACLUpdateDefaultUserPassword(sds password) {
    ACLSetUser(DefaultUser,"resetpass",-1);
    if (password) {
        sds aclop = sdscatlen(sdsnew(">"), password, sdslen(password));
        ACLSetUser(DefaultUser,aclop,sdslen(aclop));
        sdsfree(aclop);
    } else {
        ACLSetUser(DefaultUser,"nopass",-1);
    }
}
