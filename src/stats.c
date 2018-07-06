/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Detailed statistics management. For simple stats like total number of
 * "get" requests, we use inline code in memcached.c and friends, but when
 * stats detail mode is activated, the code here records more information.
 *
 * Author:
 *   Steven Grimm <sgrimm@facebook.com>
 */
#include "stats.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Stats are tracked on the basis of key prefixes. This is a simple
 * fixed-size hash of prefixes; we run the prefixes through the same
 * CRC function used by the cache hashtable.
 */
typedef struct _prefix_stats PREFIX_STATS;
struct _prefix_stats {
    char         *prefix;
    size_t        prefix_len;
    uint64_t      num_gets;
    uint64_t      num_sets;
    uint64_t      num_deletes;
    uint64_t      num_hits;
    PREFIX_STATS *next;
};

#define PREFIX_HASH_SIZE 256

static PREFIX_STATS *prefix_stats[PREFIX_HASH_SIZE];
static int num_prefixes = 0;
static int total_prefix_size = 0;

static uint8_t stats_hash_function_seed[16];

extern uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t hash(const char *in, const size_t inlen) {
    return siphash((const uint8_t *)in, inlen, stats_hash_function_seed);
}

void stats_prefix_init() {
    memset(prefix_stats, 0, sizeof(prefix_stats));
    
    memcpy(stats_hash_function_seed, "1234567812345678", sizeof(stats_hash_function_seed));
}

/*
 * Cleans up all our previously collected stats. NOTE: the stats lock is
 * assumed to be held when this is called.
 */
void stats_prefix_clear() {
    int i;

    for (i = 0; i < PREFIX_HASH_SIZE; i++) {
        PREFIX_STATS *cur, *next;
        for (cur = prefix_stats[i]; cur != NULL; cur = next) {
            next = cur->next;
            free(cur->prefix);
            free(cur);
        }
        prefix_stats[i] = NULL;
    }
    num_prefixes = 0;
    total_prefix_size = 0;
}

/*
 * Returns the stats structure for a prefix, creating it if it's not already
 * in the list.
 */
/*@null@*/
static PREFIX_STATS *stats_prefix_find(const char *key, const size_t nkey) {
    PREFIX_STATS *pfs;
    uint32_t hashval;
    size_t length;
    bool bailout = true;

    assert(key != NULL);

    for (length = 0; length < nkey && key[length] != '\0'; length++) {
        if (key[length] == prefix_delimiter) {
            bailout = false;
            break;
        }
    }

    if (bailout) {
        return NULL;
    }

    hashval = hash(key, length) % PREFIX_HASH_SIZE;

    for (pfs = prefix_stats[hashval]; NULL != pfs; pfs = pfs->next) {
        if (strncmp(pfs->prefix, key, length) == 0)
            return pfs;
    }

    pfs = calloc(sizeof(PREFIX_STATS), 1);
    if (NULL == pfs) {
        perror("Can't allocate space for stats structure: calloc");
        return NULL;
    }

    pfs->prefix = malloc(length + 1);
    if (NULL == pfs->prefix) {
        perror("Can't allocate space for copy of prefix: malloc");
        free(pfs);
        return NULL;
    }

    strncpy(pfs->prefix, key, length);
    pfs->prefix[length] = '\0';      /* because strncpy() sucks */
    pfs->prefix_len = length;

    pfs->next = prefix_stats[hashval];
    prefix_stats[hashval] = pfs;

    num_prefixes++;
    total_prefix_size += length;

    return pfs;
}

/*
 * Records a "get" of a key.
 */
void stats_prefix_record_get(const char *key, const size_t nkey, const bool is_hit) {
    PREFIX_STATS *pfs;

    STATS_LOCK();
    pfs = stats_prefix_find(key, nkey);
    if (NULL != pfs) {
        pfs->num_gets++;
        if (is_hit) {
            pfs->num_hits++;
        }
    }
    STATS_UNLOCK();
}

/*
 * Records a "delete" of a key.
 */
void stats_prefix_record_delete(const char *key, const size_t nkey) {
    PREFIX_STATS *pfs;

    STATS_LOCK();
    pfs = stats_prefix_find(key, nkey);
    if (NULL != pfs) {
        pfs->num_deletes++;
    }
    STATS_UNLOCK();
}

/*
 * Records a "set" of a key.
 */
void stats_prefix_record_set(const char *key, const size_t nkey) {
    PREFIX_STATS *pfs;

    STATS_LOCK();
    pfs = stats_prefix_find(key, nkey);
    if (NULL != pfs) {
        pfs->num_sets++;
    }
    STATS_UNLOCK();
}

/*
 * Returns stats in textual form suitable for writing to a client.
 */
/*@null@*/
char *stats_prefix_dump(int *length) {
    const char *format = "PREFIX %s get %llu hit %llu set %llu del %llu\r\n";
    PREFIX_STATS *pfs;
    char *buf;
    int i, pos;
    size_t size = 0, written = 0, total_written = 0;

    /*
     * Figure out how big the buffer needs to be. This is the sum of the
     * lengths of the prefixes themselves, plus the size of one copy of
     * the per-prefix output with 20-digit values for all the counts,
     * plus space for the "END" at the end.
     */
    STATS_LOCK();
    size = strlen(format) + total_prefix_size +
           num_prefixes * (strlen(format) - 2 /* %s */
                           + 4 * (20 - 4)) /* %llu replaced by 20-digit num */
                           + sizeof("END\r\n");
    buf = malloc(size);
    if (NULL == buf) {
        perror("Can't allocate stats response: malloc");
        STATS_UNLOCK();
        return NULL;
    }

    pos = 0;
    for (i = 0; i < PREFIX_HASH_SIZE; i++) {
        for (pfs = prefix_stats[i]; NULL != pfs; pfs = pfs->next) {
            written = snprintf(buf + pos, size-pos, format,
                           pfs->prefix, pfs->num_gets, pfs->num_hits,
                           pfs->num_sets, pfs->num_deletes);
            pos += written;
            total_written += written;
            assert(total_written < size);
        }
    }

    STATS_UNLOCK();
    memcpy(buf + pos, "END\r\n", 6);

    *length = pos + 5;
    return buf;
}


#ifdef UNIT_TEST

/****************************************************************************
      To run unit tests, compile with $(CC) -DUNIT_TEST stats.c assoc.o
      (need assoc.o to get the hash() function).
****************************************************************************/

// struct settings settings;

static char *current_test = "";
static int test_count = 0;
static int fail_count = 0;

static void fail(const char *what) { printf("\tFAIL: %s\n", what); fflush(stdout); fail_count++; }
static void test_equals_int(const char *what, int a, int b) { test_count++; if (a != b) fail(what); }
static void test_equals_ptr(const char *what, void *a, void *b) { test_count++; if (a != b) fail(what); }
static void test_equals_str(const char *what, const char *a, const char *b) { test_count++; if (strcmp(a, b)) fail(what); }
static void test_equals_ull(const char *what, uint64_t a, uint64_t b) { test_count++; if (a != b) fail(what); }
static void test_notequals_ptr(const char *what, void *a, void *b) { test_count++; if (a == b) fail(what); }
static void test_notnull_ptr(const char *what, void *a) { test_count++; if (NULL == a) fail(what); }

static void test_prefix_find() {
    PREFIX_STATS *pfs1, *pfs2;

    pfs1 = stats_prefix_find("abc", 3);
    test_equals_ptr("initial null without delimiter", pfs1, 0);

    pfs1 = stats_prefix_find("abc:", 4);
    test_notnull_ptr("initial prefix find", pfs1);
    test_equals_ull("request counts", 0ULL,
        pfs1->num_gets + pfs1->num_sets + pfs1->num_deletes + pfs1->num_hits);
    pfs2 = stats_prefix_find("abc:", 4);
    test_equals_ptr("find of same prefix", pfs1, pfs2);
    pfs2 = stats_prefix_find("abc:", 4);
    test_equals_ptr("find of same prefix, ignoring delimiter", pfs1, pfs2);
    pfs2 = stats_prefix_find("abc:d", 5);
    test_equals_ptr("find of same prefix, ignoring extra chars", pfs1, pfs2);
    pfs2 = stats_prefix_find("xyz123", 6);
    test_equals_ptr("initial null without delimiter", pfs2, 0);
    test_notequals_ptr("find of different prefix", pfs1, pfs2);
    pfs2 = stats_prefix_find("ab:", 3);
    test_notequals_ptr("find of shorter prefix", pfs1, pfs2);
}

static void test_prefix_record_get() {
    PREFIX_STATS *pfs;

    stats_prefix_record_get("abc:123", 7, 0);
    pfs = stats_prefix_find("abc:123", 7);
    test_equals_ull("get count after get #1", 1, pfs->num_gets);
    test_equals_ull("hit count after get #1", 0, pfs->num_hits);
    stats_prefix_record_get("abc:456", 7, 0);
    test_equals_ull("get count after get #2", 2, pfs->num_gets);
    test_equals_ull("hit count after get #2", 0, pfs->num_hits);
    stats_prefix_record_get("abc:456", 7, 1);
    test_equals_ull("get count after get #3", 3, pfs->num_gets);
    test_equals_ull("hit count after get #3", 1, pfs->num_hits);
    stats_prefix_record_get("def:", 4, 1);
    test_equals_ull("get count after get #4", 3, pfs->num_gets);
    test_equals_ull("hit count after get #4", 1, pfs->num_hits);
}

static void test_prefix_record_delete() {
    PREFIX_STATS *pfs;

    stats_prefix_record_delete("abc:123", 7);
    pfs = stats_prefix_find("abc:123", 7);
    test_equals_ull("get count after delete #1", 0, pfs->num_gets);
    test_equals_ull("hit count after delete #1", 0, pfs->num_hits);
    test_equals_ull("delete count after delete #1", 1, pfs->num_deletes);
    test_equals_ull("set count after delete #1", 0, pfs->num_sets);
    stats_prefix_record_delete("def:", 4);
    test_equals_ull("delete count after delete #2", 1, pfs->num_deletes);
}

static void test_prefix_record_set() {
    PREFIX_STATS *pfs;

    stats_prefix_record_set("abc:123", 7);
    pfs = stats_prefix_find("abc:123", 7);
    test_equals_ull("get count after set #1", 0, pfs->num_gets);
    test_equals_ull("hit count after set #1", 0, pfs->num_hits);
    test_equals_ull("delete count after set #1", 0, pfs->num_deletes);
    test_equals_ull("set count after set #1", 1, pfs->num_sets);
    stats_prefix_record_delete("def:", 4);
    test_equals_ull("set count after set #2", 1, pfs->num_sets);
}

static void test_prefix_dump() {
    int hashval = hash("abc", 3) % PREFIX_HASH_SIZE;
    char tmp[500];
    char *expected;
    int keynum;
    int length;

    test_equals_str("empty stats", "END\r\n", stats_prefix_dump(&length));
    test_equals_int("empty stats length", 5, length);
    stats_prefix_record_set("abc:123", 7);
    expected = "PREFIX abc get 0 hit 0 set 1 del 0\r\nEND\r\n";
    test_equals_str("stats after set", expected, stats_prefix_dump(&length));
    test_equals_int("stats length after set", strlen(expected), length);
    stats_prefix_record_get("abc:123", 7, 0);
    expected = "PREFIX abc get 1 hit 0 set 1 del 0\r\nEND\r\n";
    test_equals_str("stats after get #1", expected, stats_prefix_dump(&length));
    test_equals_int("stats length after get #1", strlen(expected), length);
    stats_prefix_record_get("abc:123", 7, 1);
    expected = "PREFIX abc get 2 hit 1 set 1 del 0\r\nEND\r\n";
    test_equals_str("stats after get #2", expected, stats_prefix_dump(&length));
    test_equals_int("stats length after get #2", strlen(expected), length);
    stats_prefix_record_delete("abc:123", 7);
    expected = "PREFIX abc get 2 hit 1 set 1 del 1\r\nEND\r\n";
    test_equals_str("stats after del #1", expected, stats_prefix_dump(&length));
    test_equals_int("stats length after del #1", strlen(expected), length);

    /* The order of results might change if we switch hash functions. */
    stats_prefix_record_delete("def:123", 7);
    expected = "PREFIX abc get 2 hit 1 set 1 del 1\r\n"
               "PREFIX def get 0 hit 0 set 0 del 1\r\n"
               "END\r\n";
    test_equals_str("stats after del #2", expected, stats_prefix_dump(&length));
    test_equals_int("stats length after del #2", strlen(expected), length);

    /* Find a key that hashes to the same bucket as "abc" */
    for (keynum = 0; keynum < PREFIX_HASH_SIZE * 100; keynum++) {
        snprintf(tmp, sizeof(tmp), "%d:", keynum);
        if (hashval == hash(tmp, strlen(tmp)) % PREFIX_HASH_SIZE) {
            printf("same hash: %s hashval: %d\n", tmp, hashval);
            break;
        }
    }
    stats_prefix_record_set(tmp, strlen(tmp));
    snprintf(tmp, sizeof(tmp),
             "PREFIX abc get 2 hit 1 set 1 del 1\r\n"
             "PREFIX %d get 0 hit 0 set 1 del 0\r\n"
             "PREFIX def get 0 hit 0 set 0 del 1\r\n"
             "END\r\n", keynum);
    test_equals_str("stats with two stats in one bucket",
                    tmp, stats_prefix_dump(&length));
    test_equals_int("stats length with two stats in one bucket",
                    strlen(tmp), length);
}

static void run_test(char *what, void (*func)(void)) {
    current_test = what;
    test_count = fail_count = 0;
    puts(what);
    fflush(stdout);

    stats_prefix_clear();
    (func)();
    printf("\t%d / %d pass\n", (test_count - fail_count), test_count);
}

/* In case we're compiled in thread mode */
void mt_stats_lock() { }
void mt_stats_unlock() { }

int main(int argc, char **argv) {
    stats_prefix_init();
    // settings.prefix_delimiter = ':';
    run_test("stats_prefix_find", test_prefix_find);
    run_test("stats_prefix_record_get", test_prefix_record_get);
    run_test("stats_prefix_record_delete", test_prefix_record_delete);
    run_test("stats_prefix_record_set", test_prefix_record_set);
    run_test("stats_prefix_dump", test_prefix_dump);

    return 0;
}

#endif
