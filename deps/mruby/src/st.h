/* This is a public domain general purpose hash table package written by Peter Moore @ UCB. */

/* @(#) st.h 5.1 89/12/14 */

#ifndef RUBY_ST_H
#define RUBY_ST_H 1

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>

#ifndef CHAR_BIT
# ifdef HAVE_LIMITS_H
#  include <limits.h>
# else
#  define CHAR_BIT 8
# endif
#endif

#ifndef ANYARGS
# ifdef __cplusplus
#   define ANYARGS ...
# else
#   define ANYARGS
# endif
#endif

typedef uintptr_t st_data_t;
typedef struct st_table st_table;

typedef st_data_t st_index_t;
typedef int st_compare_func(st_data_t, st_data_t);
typedef st_index_t st_hash_func(st_data_t);

typedef struct st_table_entry st_table_entry;

struct st_table_entry {
    st_index_t hash;
    st_data_t key;
    st_data_t record;
    st_table_entry *next;
    st_table_entry *fore, *back;
};

struct st_hash_type {
    int (*compare)(ANYARGS /*st_data_t, st_data_t*/); /* st_compare_func* */
    st_index_t (*hash)(ANYARGS /*st_data_t*/);        /* st_hash_func* */
};

struct st_table {
    const struct st_hash_type *type;
    int num_bins;
    int num_entries;
    struct st_table_entry **bins;
    struct st_table_entry *head, *tail;
};

#define st_is_member(table,key) st_lookup(table,key,(st_data_t *)0)

enum st_retval {ST_CONTINUE, ST_STOP, ST_DELETE, ST_CHECK};

st_table *st_init_table(const struct st_hash_type *);
st_table *st_init_table_with_size(const struct st_hash_type *, int);
st_table *st_init_numtable(void);
st_table *st_init_numtable_with_size(int);
st_table *st_init_strtable(void);
st_table *st_init_strtable_with_size(int);
st_table *st_init_strcasetable(void);
st_table *st_init_strcasetable_with_size(st_index_t);
int st_delete(st_table *, st_data_t *, st_data_t *);
int st_delete_safe(st_table *, st_data_t *, st_data_t *, st_data_t);
int st_insert(st_table *, st_data_t, st_data_t);
int st_lookup(st_table *, st_data_t, st_data_t *);
int st_foreach(st_table *, enum st_retval (*)(ANYARGS), st_data_t);
void st_add_direct(st_table *, st_data_t, st_data_t);
void st_free_table(st_table *);
void st_cleanup_safe(st_table *, st_data_t);
st_table *st_copy(st_table *);
int st_strcasecmp(const char *s1, const char *s2);
int st_strncasecmp(const char *s1, const char *s2, size_t n);
#define STRCASECMP(s1, s2) (st_strcasecmp(s1, s2))
#define STRNCASECMP(s1, s2, n) (st_strncasecmp(s1, s2, n))

#define ST_NUMCMP       ((int (*)()) 0)
#define ST_NUMHASH      ((int (*)()) -2)

#define st_numcmp       ST_NUMCMP
#define st_numhash      ST_NUMHASH

int st_strhash();

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif /* ST_INCLUDED */
