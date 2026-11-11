/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __REDIS_TLSHELPERS_H
#define __REDIS_TLSHELPERS_H

/* Definitions shared by every translation unit that performs TLS peer
 * certificate identity verification: tls.c (server-to-server connections and
 * the cluster bus, possibly built as a module), sentinel.c (its hiredis-based
 * links to the instances it monitors) and cli_common.c (redis-cli and
 * redis-benchmark, which do not link tls.c). Everything is header-only, since
 * these units share no object file. The OpenSSL-dependent part is compiled
 * only when OpenSSL is available, so the header may be included
 * unconditionally. */

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>  /* AF_INET, AF_INET6 */
#include <netinet/in.h>  /* struct in6_addr */
#include <arpa/inet.h>   /* inet_pton(); on FreeBSD it provides none of the above */

/* Return 1 if `name` is an IPv4 or IPv6 address literal, 0 otherwise. This
 * decides whether an identity is matched against a certificate's IP or DNS
 * entries, and whether it may be sent as SNI (which cannot carry an address). */
static inline int tlsIsIPAddressLiteral(const char *name) {
    unsigned char buf[sizeof(struct in6_addr)];
    return inet_pton(AF_INET, name, buf) == 1 || inet_pton(AF_INET6, name, buf) == 1;
}

#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

/* Peer certificate identity verification -- tls-expected-peer-name, Sentinel's
 * tls-verify-hostnames and the tools' default identity check -- relies on the
 * OpenSSL X509_VERIFY_PARAM host/IP API, available since OpenSSL 1.0.2.
 *
 * Building against an older OpenSSL is a hard error by default, so a build
 * cannot silently lack the check. Defining TLS_NO_PEER_NAME_VERIFICATION
 * compiles the feature out on any OpenSSL version: it is both the required
 * opt-out for building against an older OpenSSL and a kill switch. Each caller
 * documents what it does when compiled out; the common rule is that an implicit
 * check degrades to CA chain validation with a warning, while an explicitly
 * configured identity that cannot be honored is refused. */
#if defined(TLS_NO_PEER_NAME_VERIFICATION)
#define CONN_TLS_SUPPORTS_VERIFY_NAME 0
#elif OPENSSL_VERSION_NUMBER >= 0x10002000L
#define CONN_TLS_SUPPORTS_VERIFY_NAME 1
#else
#error "TLS peer certificate identity verification requires OpenSSL 1.0.2 or \
newer. Build against a newer OpenSSL, or define TLS_NO_PEER_NAME_VERIFICATION to \
build without it."
#endif

#if CONN_TLS_SUPPORTS_VERIFY_NAME
/* Bind one expected peer identity to OpenSSL's verification parameters. Every
 * identity check -- tls-expected-peer-name, Sentinel's address check and the
 * tools' default check -- goes through this one function, so OpenSSL's
 * subtleties live here and nowhere else:
 *
 *  - An IP address literal is matched against the certificate's IP SAN entries
 *    (set1_ip_asc). The host matcher never consults those, so an address handed
 *    to it would silently never match. Anything else is matched against the DNS
 *    SAN entries, with the CN as a fallback when there are none (set1_host /
 *    add1_host). OpenSSL keeps a single expected IP and, when host names are set
 *    as well, requires BOTH to match -- callers accepting a list of identities
 *    must therefore not mix the two kinds.
 *  - `replace` selects set1_host, which replaces the expected host list, over
 *    add1_host, which appends to it so that any listed host may match.
 *  - The length is passed explicitly: OpenSSL reads a length of 0 as "call
 *    strlen", but BoringSSL rejects a zero length outright.
 *  - Partial-label wildcards in the certificate ("f*.example.com") are rejected;
 *    full-label ones ("*.example.com") still match.
 *
 * The X509_VERIFY_PARAM_* host API (available since OpenSSL 1.0.2) is used
 * rather than the SSL_set1_host()/SSL_add1_host()/SSL_set_hostflags()
 * convenience wrappers (introduced only in OpenSSL 1.1.0). This keeps
 * certificate name verification available on every OpenSSL version that
 * actually provides the hostname-checking machinery, matching the older
 * versions the callers still support. It is also why the function takes the
 * X509_VERIFY_PARAM obtained with SSL_get0_param() rather than the SSL object.
 *
 * Returns 1 on success, 0 on failure, like the OpenSSL calls it wraps. */
static inline int tlsVerifyParamAddIdentity(X509_VERIFY_PARAM *param, const char *name,
                                            size_t len, int replace)
{
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (tlsIsIPAddressLiteral(name))
        return X509_VERIFY_PARAM_set1_ip_asc(param, name);
    return replace ? X509_VERIFY_PARAM_set1_host(param, name, len)
                   : X509_VERIFY_PARAM_add1_host(param, name, len);
}
#endif /* CONN_TLS_SUPPORTS_VERIFY_NAME */

#endif /* USE_OPENSSL */

#endif /* __REDIS_TLSHELPERS_H */
