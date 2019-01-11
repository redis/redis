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

/* Create a new user with the specified name, store it in the list
 * of users (the Users global radix tree), and returns a reference to
 * the structure representing the user.
 *
 * If the user with such name already exists NULL is returned. */
user *ACLCreateUser(const char *name, size_t namelen) {
    if (raxFind(Users,(unsigned char*)name,namelen) != raxNotFound) return NULL;
    user *u = zmalloc(sizeof(*u));
    u->flags = 0;
    u->allowed_subcommands = NULL;
    u->passwords = listCreate();
    u->patterns = NULL; /* Just created users cannot access to any key, however
                           if the "~*" directive was enabled to match all the
                           keys, the user will be flagged with the ALLKEYS
                           flag. */
    memset(u->allowed_commands,0,sizeof(u->allowed_commands));
    raxInsert(Users,(unsigned char*)name,namelen,u,NULL);
    return u;
}

/* Set user properties according to the string "op". The following
 * is a description of what different strings will do:
 *
 * on           Enable the user
 * off          Disable the user
 * +<command>   Allow the execution of that command
 * -<command>   Disallow the execution of that command
 * +@<category> Allow the execution of all the commands in such category
 *              with valid categories being @set, @sortedset, @list, @hash,
 *                                          @string, @bitmap, @hyperloglog,
 *                                          @stream, @admin, @readonly,
 *                                          @readwrite, @fast, @slow,
 *                                          @pubsub.
 *              The special category @all means all the commands.
 * +<command>|subcommand    Allow a specific subcommand of an otherwise
 *                          disabled command. Note that this form is not
 *                          allowed as negative like -DEBUG|SEGFAULT, but
 *                          only additive starting with "+".
 * ~<pattern>   Add a pattern of keys that can be mentioned as part of
 *              commands. For instance ~* allows all the keys. The pattern
 *              is a glob-style pattern like the one of KEYS.
 *              It is possible to specify multiple patterns.
 * ><password>  Add this passowrd to the list of valid password for the user.
 *              For example >mypass will add "mypass" to the list.
 * <<password>  Remove this password from the list of valid passwords.
 * allkeys      Alias for ~*
 * resetpass    Flush the list of allowed passwords.
 * resetkeys    Flush the list of allowed keys patterns.
 * reset        Performs the following actions: resetpass, resetkeys, off,
 *              -@all. The user returns to the same state it has immediately
 *              after its creation.
 *
 * The function returns C_OK if the action to perform was understood because
 * the 'op' string made sense. Otherwise C_ERR is returned if the operation
 * is unknown or has some syntax error.
 */
int ACLSetUser(user *u, const char *op) {
    if (!strcasecmp(op,"on")) {
        u->flags |= USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"off")) {
        u->flags &= ~USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"allkeys") ||
               !strcasecmp(op,"~*"))
    {
        memset(u->allowed_subcommands,255,sizeof(u->allowed_commands));
        u->flags |= USER_FLAG_ALLKEYS;
    } else {
        return C_ERR;
    }
    return C_OK;
}

/* Initialization of the ACL subsystem. */
void ACLInit(void) {
    Users = raxNew();
    DefaultUser = ACLCreateUser("default",7);
    ACLSetUser(DefaultUser,"+@all");
    ACLSetUser(DefaultUser,"on");
}

/* Check the username and password pair and return C_OK if they are valid,
 * otherwise C_ERR is returned and errno is set to:
 *
 *  EINVAL: if the username-password do not match.
 *  ENONENT: if the specified user does not exist at all.
 */
int ACLCheckUserCredentials(robj *username, robj *password) {
    /* For now only the "default" user is allowed. When the RCP1 ACLs
     * will be implemented multiple usernames will be supproted. */
    if (username != NULL && strcmp(username->ptr,"default")) {
        errno = ENOENT;
        return C_ERR;
    }

    /* For now we just compare the password with the system wide one. */
    if (!time_independent_strcmp(password->ptr, server.requirepass)) {
        return C_OK;
    } else {
        errno = EINVAL;
        return C_ERR;
    }
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

    if (map == NULL) map = raxNew();
    void *id = raxFind(map,(unsigned char*)cmdname,strlen(cmdname));
    if (id != raxNotFound) return (unsigned long)id;
    raxInsert(map,(unsigned char*)cmdname,strlen(cmdname),(void*)nextid,NULL);
    return nextid++;
}

/* Return an username by its name, or NULL if the user does not exist. */
user *ACLGetUserByName(const char *name, size_t namelen) {
    void *myuser = raxFind(Users,(unsigned char*)name,namelen);
    if (myuser == raxNotFound) return NULL;
    return myuser;
}

/* =============================================================================
 * ACL related commands
 * ==========================================================================*/
