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
 * redis-benchmark, which do not link tls.c). Only include this from code that
 * is compiled with OpenSSL available. */

#include <openssl/ssl.h>

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

#endif /* __REDIS_TLSHELPERS_H */
