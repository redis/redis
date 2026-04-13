/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "server.h"
#include <math.h>

/* GCRA algorithm for rate limiting.
 * Implementation is heavily based on the implementation of (redis-cell)
 * [https://github.com/brandur/redis-cell] by (brandur)[https://github.com/brandur].
 *
 * It is a leaky-bucket type algorithm but instead of periodically dripping, we
 * calculate the next time the bucket has capacity - called
 * Theoretical arrival time(TaT) by the algorithm. We allow requests at a
 * sustained rate (f.e 5 request per 10 seconds, i.e 1 request per 2 seconds)
 * but also allow bursts of multiple request at one time.
 *
 * Explanation of the algorithm follows using the leaky-bucket analogy.
 *
 * GCRA works by keeping track of the next TaT and updating it after requests
 * are allowed. Let T be the emission interval for a request - in the
 * leaky-bucket analogy this will be the period at which the bucket drips.
 * Using N requests will result in the (next TaT) = (current TaT) + N * T (time
 * needed to drain the bucket). To determine if a request can be allowed we can
 * calculate the time at which "the bucket dripped", which is TaT-T.
 * If this time is in the past the request is allowed, otherwise we wait and TaT
 * is not updated. This only accounts for 1 request though. In order to allow
 * bursts we can imagine a full burst fully filling an empty bucket, thus
 * we need to calculate the time after which "the bucket will completely drain"
 * the requests of the burst - this will be t = T * max_burst.
 * At last the allowance check will be:
 *
 *   "now" >= TaT - (T + t)
 *
 * And in this case a picture is worth about 250 words:
 *
 * +-------------------+
 * |  ALLOWED REQUEST  |
 * +-------------------+
 * 
 *   +-----------+          +-----+    +-----+
 *   | allow at  |          | now |    | TaT |
 *   |  (past)   |          +-----+    +-----+
 *   +-----------+            |          |
 *                            |          |
 * ---+-----------------------+----------+-----------> time
 *    |//////////////////////////////////|
 *    |//////////////////////////////////|
 *    +----------------------------------+
 *    |                                  |
 *    |<------------- t + T ------------>|
 * 
 * 
 *    +------------------------------------------+
 *    | T     = Emission interval                |
 *    | t     = Capacity of bucket               |
 *    | t + T = Delay variation tolerance        |
 *    | tat   = Theoretical arrival time         |
 *    | now   = Actual time of request           |
 *    +------------------------------------------+
 *
 * (ASCII art adapted from https://brandur.org/rate-limiting). */

/* GCRA key max_burst tokens_per_period period [TOKENS count]
 *
 * key: Key related to specific rate limiting case
 * max_burst: Maximum tokens allowed as burst (in addition to sustained rate)
 * tokens_per_period: Number of tokens allowed per period
 * period: Period in seconds for calculating sustained rate
 * tokens: Optional, cost of this request (default: 1)
 */
void gcraCommand(client *c) {
    robj *key = c->argv[1];

    /* GCRA parameters */
    long max_burst;
    long tokens_per_period;
    long num_tokens = 1;
    double period;

    /* Variables used in the reply */
    int limited; /* Whether or not the request was limited */
    long long remaining = 0; /* Number of requests available immediately */
    long long retry_after_s = -1; /* Time in seconds after which the caller can retry */
    long long reset_after_s = 0; /* Number of seconds after which a full burst will be allowed */

    if (c->argc > 7) {
        addReplyErrorArity(c);
        return;
    }

    if (getPositiveLongFromObjectOrReply(c, c->argv[2], &max_burst, NULL) != C_OK) {
        return;
    }
    if (likely(max_burst < LONG_MAX)) max_burst += 1;

    if (getRangeLongFromObjectOrReply(c, c->argv[3], 1, LONG_MAX, &tokens_per_period, NULL) != C_OK) {
        return;
    }

    if (getDoubleFromObjectOrReply(c, c->argv[4], &period, NULL) != C_OK) {
        return;
    }
    if (period <= 0 || period >= 1e12) {
        addReplyError(c, "period must be > 0 and < 1e12");
        return;
    }

    if (c->argc >= 6) {
        if (strcasecmp("tokens", c->argv[5]->ptr)) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        if (c->argc == 6) {
            addReplyError(c, "Missing TOKENS value");
            return;
        }
        if (getRangeLongFromObjectOrReply(c, c->argv[6], 1, LONG_MAX, &num_tokens, NULL) != C_OK) {
            return;
        }
    }

    ustime_t now = commandTimeSnapshot() * 1000;

    long long tat_us, new_tat_us;
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, key, &link);
    if (checkType(c, kv, OBJ_STRING)) {
        return;
    }
    if (kv != NULL) {
        /* Note the value of the key may have been overwritten outside of the
         * GCRA command (f.e by calling SET). We don't try to catch such errors
         * as this would be possible only with a dedicated structures for GCRA,
         * while using STRING gives us all the benefits of a redis key -
         * replication, setting expiration, etc. */
        if (getLongLongFromObject(kv, &tat_us) != C_OK) {
            addReplyError(c, "Invalid GCRA key");
            return;
        }
        if (tat_us <= 0) {
            addReplyError(c, "Negative time is invalid value for GCRA");
            return;
        }
    } else {
        tat_us = now;
    }

    /* microsecond accuracy */
    double period_us = period * 1000000.;

    /* Emission interval is the minimum amount of time between requests.
     * Note on calculations:
     * Even if emission_interval_us becomes less than 1us, we assume it's min
     * 1ms. The API is already in seconds granularity so it is expected the user
     * won't need a submicrosecond accuracy. */
    long long emission_interval_us = (long long)(period_us / tokens_per_period + 0.5);
    if (unlikely(emission_interval_us == 0)) emission_interval_us = 1;

    /* overflow checks. In normal circumstances we shouldn't get these but the
     * user may have wrongfully specified very large values.
     * Note that all values are positive. */
    if (emission_interval_us > LLONG_MAX / num_tokens) {
        addReplyError(c, "GCRA limiting uses microsecond accuracy. Combination of period, tokens_per_period and TOKENS would cause an overflow");
        return;
    }
    if (emission_interval_us > LLONG_MAX / max_burst) {
        addReplyError(c, "GCRA limiting uses microsecond accuracy. Combination of period, tokens_per_period and max_burst would cause an overflow");
        return;
    }

    /* Max bursts give us an amount of requests we can use up at one time.
     * The variance will calculate the amount of time that many request need
     * to "refill the bucket". */
    long long variance_us = emission_interval_us * max_burst;

    /* If a request is allowed the next TaT is after an emission_interval_us time.
     * Hence for multiple requests we multiple by their number. */
    long long increment_us = emission_interval_us * num_tokens;

    long long base_us = (now > tat_us) ? now : tat_us;
    if (LLONG_MAX - base_us < increment_us) {
        addReplyError(c, "GCRA limiting uses microsecond accuracy. Combination of period, tokens_per_period and TOKENS would cause an overflow");
        return;
    }
    new_tat_us = base_us + increment_us;

    /* Calculate the time a request is allowed. This is TaT, but because we allow
     * a burst we move that time in the past. If the allow time is before the
     * time we ask (i.e now) we allow the request, otherwise we limit it and
     * calculate after how much time the user should retry. */
    long long allow_at = new_tat_us - variance_us;
    long long diff_us = now - allow_at;
    long long ttl_us;
    if (diff_us < 0) {
        limited = 1;
        /* NOTE: if increment is more than variance, then number of requests is
         * more than what is maximally allowed (i.e max_bursts + 1) so we leave
         * retry_after_s to -1 in this case, as it should never be retried. */
        if (increment_us <= variance_us) {
            retry_after_s = ceil((-diff_us) / 1000000.);
        }
        ttl_us = tat_us - now;
    } else {
        limited = 0;
        ttl_us = new_tat_us - now;
        robj *tatobj = createStringObjectFromLongLong(new_tat_us);
        setKeyByLink(c, c->db, key, &tatobj, kv ? SETKEY_ALREADY_EXIST : SETKEY_DOESNT_EXIST, &link);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);

        long long when = new_tat_us / 1000;
        kv = setExpireByLink(c, c->db, key->ptr, when, link);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);

        /* Replicating the command directly would mess up TaT as we use
         * commandTimeSnapshot. We instead rewrite the command as SET with the
         * appropriate expire time. */
        robj *pexat_obj = createStringObjectFromLongLong(when);
        rewriteClientCommandVector(c, 5, shared.set, key, kv, shared.pxat, pexat_obj);
        decrRefCount(pexat_obj);

        server.dirty++;
    }

    long long next_us = variance_us - ttl_us;
    if (next_us > -emission_interval_us) {
        remaining = next_us / emission_interval_us;
    }
    reset_after_s = ceil(ttl_us / 1000000.);

    addReplyArrayLen(c, 5);
    addReply(c, limited ? shared.cone : shared.czero);
    addReplyLongLong(c, max_burst);
    addReplyLongLong(c, remaining);
    addReplyLongLong(c, retry_after_s);
    addReplyLongLong(c, reset_after_s);
}
