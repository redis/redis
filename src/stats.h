#ifndef STATS_H
#define STATS_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

/* stats */
void stats_prefix_init(void);
void stats_prefix_clear(void);
void stats_prefix_record_get(const char *key, const size_t nkey, const bool is_hit);
void stats_prefix_record_delete(const char *key, const size_t nkey);
void stats_prefix_record_set(const char *key, const size_t nkey);
/*@null@*/
char *stats_prefix_dump(int *length);

uint64_t hash(const char *in, const size_t inlen);

#define STATS_LOCK()
#define STATS_UNLOCK()
#define prefix_delimiter ':'

#endif // STATS_H
