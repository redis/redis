/*
** mruby/string.h - String class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_STRING_H
#define MRUBY_STRING_H

#if defined(__cplusplus)
extern "C" {
#endif

#ifdef INCLUDE_ENCODING
#include "encoding.h"
#endif

#ifndef RB_GC_GUARD
#define RB_GC_GUARD(v) v
#endif

#define IS_EVSTR(p,e) ((p) < (e) && (*(p) == '$' || *(p) == '@' || *(p) == '{'))

#define STR_BUF_MIN_SIZE 128

extern const char mrb_digitmap[];

struct mrb_shared_string {
  int refcnt;
  char *ptr;
  int len;
};

struct RString {
  MRB_OBJECT_HEADER;
  int len;
  union {
    int capa;
    struct mrb_shared_string *shared;
  } aux;
  char *ptr;
};

#define mrb_str_ptr(s)    ((struct RString*)((s).value.p))
#define RSTRING(s)        ((struct RString*)((s).value.p))
#define RSTRING_PTR(s)    (RSTRING(s)->ptr)
#define RSTRING_LEN(s)    (RSTRING(s)->len)
#define RSTRING_CAPA(s)   (RSTRING(s)->aux.capa)
#define RSTRING_END(s)    (RSTRING(s)->ptr + RSTRING(s)->len)
#define MRB_STR_SHARED      256

void mrb_str_decref(mrb_state*, struct mrb_shared_string*);
mrb_value mrb_str_literal(mrb_state*, mrb_value);
void mrb_str_concat(mrb_state*, mrb_value, mrb_value);
mrb_value mrb_str_plus(mrb_state*, mrb_value, mrb_value);
mrb_value mrb_obj_as_string(mrb_state *mrb, mrb_value obj);
mrb_value mrb_str_resize(mrb_state *mrb, mrb_value str, int len); /* mrb_str_resize */
mrb_value mrb_string_value(mrb_state *mrb, mrb_value *ptr); /* StringValue */
mrb_value mrb_str_substr(mrb_state *mrb, mrb_value str, mrb_int beg, int len);
mrb_value mrb_check_string_type(mrb_state *mrb, mrb_value str);
mrb_value mrb_str_buf_new(mrb_state *mrb, int capa);
mrb_value mrb_str_buf_cat(mrb_state *mrb, mrb_value str, const char *ptr, int len);

char *mrb_string_value_cstr(mrb_state *mrb, mrb_value *ptr);
char *mrb_string_value_ptr(mrb_state *mrb, mrb_value ptr);
int mrb_str_sublen(mrb_state *mrb, mrb_value str, int pos);
int mrb_str_offset(mrb_state *mrb, mrb_value str, int pos);
mrb_value mrb_str_dup(mrb_state *mrb, mrb_value str); /* mrb_str_dup */
mrb_value mrb_str_intern(mrb_state *mrb, mrb_value self);
mrb_value mrb_str_cat2(mrb_state *mrb, mrb_value str, const char *ptr);
mrb_value mrb_str_catf(mrb_state *mrb, mrb_value str, const char *format, ...);
mrb_value mrb_str_to_inum(mrb_state *mrb, mrb_value str, int base, int badcheck);
double mrb_str_to_dbl(mrb_state *mrb, mrb_value str, int badcheck);
mrb_value mrb_str_to_str(mrb_state *mrb, mrb_value str);
mrb_int mrb_str_hash(mrb_state *mrb, mrb_value str);
int mrb_str_hash_cmp(mrb_state *mrb, mrb_value str1, mrb_value str2);
mrb_value mrb_str_buf_append(mrb_state *mrb, mrb_value str, mrb_value str2);
mrb_value mrb_str_inspect(mrb_state *mrb, mrb_value str);
int mrb_str_equal(mrb_state *mrb, mrb_value str1, mrb_value str2);
mrb_value mrb_str_dump(mrb_state *mrb, mrb_value str);
mrb_value mrb_str_cat(mrb_state *mrb, mrb_value str, const char *ptr, long len);
mrb_value mrb_str_append(mrb_state *mrb, mrb_value str, mrb_value str2);

int mrb_str_cmp(mrb_state *mrb, mrb_value str1, mrb_value str2);

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif  /* MRUBY_STRING_H */
