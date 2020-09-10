/* TLS Authentication module -- Handle automatic user authentication
 * based on TLS client side certificate attributes.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2020, Redis Labs
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

#define REDISMODULE_EXPERIMENTAL_API
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include "redismodule.h"

/* Required attribute configuration */
typedef struct RequiredAttr {
    int nid;                    /* Attribute, represented as an OpenSSL NID */
    const char *value;          /* Required value */
    struct RequiredAttr *next;  /* Next element in RequiredAttr singly-linked list */
} RequiredAttr;

/* Module configuration */
typedef struct Config {
    int user_attr;                      /* Attribute to derive user identity from */
    RequiredAttr *required_attr_head;   /* Head of AttrCheck singly-linked list */
} Config;

static Config config = {
    .user_attr              = NID_commonName,
    .required_attr_head     = NULL
};

/* Compare a RedisModuleString and a null-terminated C string.
 * Returns a negative, zero or positive value in strcmp() semantics.
 */
static int moduleStrCaseCmp(RedisModuleString *rmstr, const char *str)
{
    size_t len;
    const char *s = RedisModule_StringPtrLen(rmstr, &len);

    if (len != strlen(str))
        return len - strlen(str);
    return strncasecmp(s, str, len);
}

/* Duplicate a RedisModuleString into a newly allocated, null-terminated C
 * string.
 */
static char *moduleStrDup(RedisModuleString *rmstr)
{
    size_t len;
    const char *str = RedisModule_StringPtrLen(rmstr, &len);
    char *ret = RedisModule_Alloc(len + 1);
    memcpy(ret, str, len);
    ret[len] = '\0';

    return ret;
}

/* Parse a specified RedisModuleString as an OpenSSL attribute name and
 * return the corresponding NID (or NID_undef).
 */
static int parseAttributeName(RedisModuleString *rmstr)
{
    char *str = moduleStrDup(rmstr);
    int nid = OBJ_txt2nid(str);
    RedisModule_Free(str);

    return nid;
}

/* Parse configuration provided as module arguments and set up the module.
 * Returns REDISMODULE_ERR on parse errors, or REDISMODULE_OK on success.
 */
static int parseConfigArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, Config *config)
{
    const char kw_user_attr[] = "user-attribute";
    const char kw_required_attr[] = "required-attribute";
    const char *err = NULL;
    int nid;
    int i;

    for (i = 0; i < argc; i++) {
        if (!moduleStrCaseCmp(argv[i], kw_user_attr)) {
            if (argc <= i + 1) {
                err = "Use: USER-ATTRIBUTE <attribute name>";
                break;
            }

            if ((nid = parseAttributeName(argv[++i])) == NID_undef) {
                err = "Unknown USER-ATTRIBUTE name";
                break;
            }

            config->user_attr = nid;
        } else if (!moduleStrCaseCmp(argv[i], kw_required_attr)) {
            if (argc <= i + 2) {
                err = "Use: REQUIRED-ATTRIBUTE <attribute name> <value>";
                break;
            }

            if ((nid = parseAttributeName(argv[++i])) == NID_undef) {
                err = "Unknown REQUIRED-ATTRIBUTE attribute name";
                break;
            }

            struct RequiredAttr *reqattr = RedisModule_Alloc(sizeof(RequiredAttr));
            reqattr->nid = nid;
            reqattr->value = moduleStrDup(argv[++i]);
            reqattr->next = config->required_attr_head;
            config->required_attr_head = reqattr;
        } else {
            err = "Invalid argument specified";
            break;
        }
    }

    if (err) {
        RedisModule_Log(ctx, "warning", "Failed to load tlsauth configuration: %s", err);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

/* Decode a RedisModuleString that contains a PEM-encoded X.509 certificate
 * and returns a newly allocated OpenSSL X509 struct.
 */
static X509 *decodeCertificate(RedisModuleString *cert_str)
{
    size_t len;
    const char *str = RedisModule_StringPtrLen(cert_str, &len);

    BIO *bio = BIO_new(BIO_s_mem());
    BIO_write(bio, str, len);

    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return cert;
}

/* Check that a given X.509 certificate conforms to a list of required attributes.
 * Returns a non-zero value if valid, or zero if at least one attribute is missing
 * or has the wrong value.
 */
static int checkRequiredAttrs(X509 *cert, RequiredAttr *required_attr_list)
{
    int loc;
    X509_NAME *subj = X509_get_subject_name(cert);
    X509_NAME_ENTRY *entry;
    struct RequiredAttr *req_attr = required_attr_list;

    while (req_attr != NULL) {
        /* Find entry */
        if ((loc = X509_NAME_get_index_by_NID(subj, req_attr->nid, -1)) < 0)
            return 0;

        /* Compare */
        if (!(entry = X509_NAME_get_entry(subj, loc)))
            return 0;

        const char *val = (const char *) ASN1_STRING_get0_data(X509_NAME_ENTRY_get_data(entry));
        if (strcmp(val, req_attr->value)) return 0;

        /* Next */
        req_attr = req_attr->next;
    }

    return 1;
}

/* Fetch an attribute identified by its OpenSSL NID. Returns a null-terminated
 * string.
 */
const char *getAttribute(X509 *cert, int nid)
{
    X509_NAME_ENTRY *entry;
    X509_NAME *subj = X509_get_subject_name(cert);
    int loc;

    if ((loc = X509_NAME_get_index_by_NID(subj, nid, -1)) < 0)
        return NULL;

    if (!(entry = X509_NAME_get_entry(subj, loc)))
        return NULL;

    return (const char *) ASN1_STRING_get0_data(X509_NAME_ENTRY_get_data(entry));
}

/* Module's main entry point. This is where we fetch the certificate of new
 * incoming connections, run checks, extract user identity and authenticate.
 */
static void handleClientConnection(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data)
{
    /* We only care about new connections */
    if (eid.id == REDISMODULE_EVENT_CLIENT_CHANGE &&
            subevent == REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) {

        /* Try to fetch certificate */
        RedisModuleClientInfo *ci = (RedisModuleClientInfo *) data;
        RedisModuleString *cert_str = RedisModule_GetClientCertificate(ctx, ci->id);
        X509 *cert = NULL;

        /* Try to decode it */
        if (cert_str != NULL) {
            cert = decodeCertificate(cert_str);
            RedisModule_FreeString(ctx, cert_str);
        }

        /* Nothing to do without a certificate */
        if (!cert) return;

        /* If certificate passes checks and we can extract user identity,
         * authenticate the client now.
         */
        const char *user;
        if (checkRequiredAttrs(cert, config.required_attr_head) &&
            (user = getAttribute(cert, config.user_attr)) != NULL) {

            if (RedisModule_AuthenticateClientWithACLUser(ctx, user, strlen(user),
                    NULL, NULL, NULL) == REDISMODULE_ERR) {
                RedisModule_Log(ctx, "verbose",
                    "Failed to authorize user %s", user);
            } else {
                RedisModule_Log(ctx, "debug", "Authorized user %s", user);
            }
        }
    }
}

/* Module initialization */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "tlsauth", 1, REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (parseConfigArgs(ctx, argv, argc, &config) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClientChange,
        handleClientConnection) == REDISMODULE_ERR) {
            return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
