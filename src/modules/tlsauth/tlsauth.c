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

/* To see this module in action, follow these steps:
 *
 */

#define REDISMODULE_EXPERIMENTAL_API
#include <string.h>
#include <sys/types.h>

#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include "redismodule.h"

struct AttrCheck {
    int nid;
    const char *req_value;
    struct AttrCheck *next;
};

static int configUserAttr = NID_commonName;
static struct AttrCheck *configAttrChecks = NULL;

static struct AttrCheck *parseAttrCheck(RedisModuleCtx *ctx, char *attr_check_str)
{
    char *delim = strchr(attr_check_str, ':');
    if (!delim) {
        RedisModule_Log(ctx, "warn",
                "Invalid attr-check: expected 'attribute:value' format: %s", attr_check_str);
        return NULL;
    }

    *delim = '\0';
    int nid = OBJ_txt2nid(attr_check_str);
    if (nid == NID_undef) {
        RedisModule_Log(ctx, "warn",
                "Invalid attr-check attribute: %s", attr_check_str);
        return NULL;
    }

    struct AttrCheck *check = RedisModule_Alloc(sizeof(struct AttrCheck));
    check->nid = nid;
    check->req_value = RedisModule_Strdup(delim + 1);
    check->next = NULL;

    return check;
}

static int parseConfigArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    const char kw_attr_user[] = "attr-user";
    const char kw_attr_check[] = "attr-check";
    int error = 0;
    int i;

    for (i = 0; i < argc; i++) {
        size_t len;
        const char *str = RedisModule_StringPtrLen(argv[i], &len);

        const char *delim = memchr(str, '=', len);
        if (!delim) {
            RedisModule_Log(ctx, "warn",
                    "Invalid argument: %.*s", (int) len, str);
            return REDISMODULE_ERR;
        }

        /* Set up keyword, value lengths and a null-terminated value */
        size_t kw_len = delim - str;
        size_t val_len = len - (kw_len + 1);
        char *val = RedisModule_Alloc(val_len + 1);
        memcpy(val, delim + 1, val_len);
        val[val_len] = '\0';


        if (kw_len == strlen(kw_attr_user) && !memcmp(str, kw_attr_user, kw_len)) {
            int nid = OBJ_txt2nid(val);
            if (nid == NID_undef) {
                RedisModule_Log(ctx, "warn",
                        "Invalid attribute name: %s", val);
                error = 1;
            } else {
                configUserAttr = nid;
            }
        } else if (kw_len == strlen(kw_attr_check) && !memcmp(str, kw_attr_check, kw_len)) {
            struct AttrCheck *check = parseAttrCheck(ctx, val);
            if (!check) {
                error = 1;
            } else {
                check->next = configAttrChecks;
                configAttrChecks = check;
            }
        } else {
            RedisModule_Log(ctx, "warn",
                    "Unknown configuration argument: %.*s", (int) kw_len, str);
            error = 1;
        }

        RedisModule_Free(val);
        if (error) return 0;
    }

    return 1;
}

static X509 *decodeCertificate(RedisModuleString *cert_str)
{
    size_t len;
    const char *str = RedisModule_StringPtrLen(cert_str, &len);

    BIO *bio = BIO_new(BIO_s_mem());
    BIO_write(bio, str, len);

    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    return cert;
}

static int checkCertificate(X509 *cert)
{
    int loc;
    X509_NAME *subj = X509_get_subject_name(cert);
    X509_NAME_ENTRY *entry;
    struct AttrCheck *check = configAttrChecks;

    while (check != NULL) {
        /* Find entry */
        if ((loc = X509_NAME_get_index_by_NID(subj, check->nid, -1)) < 0)
            return 0;

        /* Compare */
        if (!(entry = X509_NAME_get_entry(subj, loc)))
            return 0;

        const char *val = (const char *) ASN1_STRING_get0_data(X509_NAME_ENTRY_get_data(entry));
        if (strcmp(val, check->req_value)) return 0;

        /* Next */
        check = check->next;
    }

    return 1;
}

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

static void handleClientConnection(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data)
{
    if (eid.id == REDISMODULE_EVENT_CLIENT_CHANGE &&
            subevent == REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) {
        RedisModuleClientInfo *ci = (RedisModuleClientInfo *) data;
        RedisModuleString *cert_str = RedisModule_GetClientCertificate(ctx, ci->id);
        if (cert_str != NULL) {
            X509 *cert = decodeCertificate(cert_str);
            const char *user;

            if (cert && checkCertificate(cert) &&
                    (user = getAttribute(cert, configUserAttr)) != NULL) {

                    if (RedisModule_AuthenticateClientWithACLUser(ctx, user, strlen(user),
                                NULL, NULL, NULL) == REDISMODULE_ERR) {
                        RedisModule_Log(ctx, "verbose",
                                "Failed to authorize user %s", user);
                    } else {
                        RedisModule_Log(ctx, "debug", "Authorized user %s", user);
                    }
            }

            RedisModule_FreeString(ctx, cert_str);
        }
    }
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "tlsauth", 1, REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (!parseConfigArgs(ctx, argv, argc))
        return REDISMODULE_ERR;

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClientChange,
        handleClientConnection) == REDISMODULE_ERR) {
            return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
