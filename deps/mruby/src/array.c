/*
** array.c - Array class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/array.h"
#include <string.h>
#include "mruby/string.h"
#include "mruby/class.h"

#define ARY_DEFAULT_LEN   4
#define ARY_SHRINK_RATIO  5 /* must be larger than 2 */
#ifdef INT_MAX
#  define ARY_MAX_SIZE (INT_MAX / sizeof(mrb_value))
#endif

static inline mrb_value
ary_elt(mrb_value ary, int offset)
{
  if (RARRAY_LEN(ary) == 0) return mrb_nil_value();
  if (offset < 0 || RARRAY_LEN(ary) <= offset) {
    return mrb_nil_value();
  }
  return RARRAY_PTR(ary)[offset];
}

static struct RArray*
ary_new_capa(mrb_state *mrb, int capa)
{
  struct RArray *a;
  int blen;

#ifdef INT_MAX
  if (capa > ARY_MAX_SIZE) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "ary size too big");
  }
#endif
  blen = capa * sizeof(mrb_value) ;
  if (blen < capa) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "ary size too big");
  }

  a = (struct RArray*)mrb_obj_alloc(mrb, MRB_TT_ARRAY, mrb->array_class);
  a->ptr = (mrb_value *)mrb_malloc(mrb, blen);
  a->aux.capa = capa;
  a->len = 0;

  return a;
}

mrb_value
mrb_ary_new_capa(mrb_state *mrb, int capa)
{
  struct RArray *a = ary_new_capa(mrb, capa);
  return mrb_obj_value(a);
}

mrb_value
mrb_ary_new(mrb_state *mrb)
{
  return mrb_ary_new_capa(mrb, 0);
}

static inline void
array_copy(mrb_value *dst, const mrb_value *src, size_t size)
{
  int i;

  for (i = 0; i < size; i++) {
    dst[i] = src[i];
  }
}


mrb_value
mrb_ary_new_from_values(mrb_state *mrb, int size, mrb_value *vals)
{
  mrb_value ary;
  struct RArray *a;

  ary = mrb_ary_new_capa(mrb, size);
  a = mrb_ary_ptr(ary);
  array_copy(a->ptr, vals, size);
  a->len = size;

  return ary;
}

mrb_value
mrb_assoc_new(mrb_state *mrb, mrb_value car, mrb_value cdr)
{
  mrb_value arv[2];
  arv[0] = car;
  arv[1] = cdr;
  return mrb_ary_new_from_values(mrb, 2, arv);
}

static void
ary_fill_with_nil(mrb_value *ptr, int size)
{
  mrb_value nil = mrb_nil_value();

  while((int)(size--)) {
    *ptr++ = nil;
  }
}

static void
ary_modify(mrb_state *mrb, struct RArray *a)
{
  if (a->flags & MRB_ARY_SHARED) {
    struct mrb_shared_array *shared = a->aux.shared;

    if (shared->refcnt == 1 && a->ptr == shared->ptr) {
      a->ptr = shared->ptr;
      a->aux.capa = a->len;
      mrb_free(mrb, shared);
    }
    else {
      mrb_value *ptr, *p;
      int len;

      p = a->ptr;
      len = a->len * sizeof(mrb_value);
      ptr = (mrb_value *)mrb_malloc(mrb, len);
      if (p) {
	array_copy(ptr, p, a->len);
      }
      a->ptr = ptr;
      a->aux.capa = a->len;
      mrb_ary_decref(mrb, shared);
    }
    a->flags &= ~MRB_ARY_SHARED;
  }
}

static void
ary_make_shared(mrb_state *mrb, struct RArray *a)
{
  if (!(a->flags & MRB_ARY_SHARED)) {
    struct mrb_shared_array *shared = (struct mrb_shared_array *)mrb_malloc(mrb, sizeof(struct mrb_shared_array));

    shared->refcnt = 1;
    if (a->aux.capa > a->len) {
      a->ptr = shared->ptr = (mrb_value *)mrb_realloc(mrb, a->ptr, sizeof(mrb_value)*a->len+1);
    }
    else {
      shared->ptr = a->ptr;
    }
    shared->len = a->len;
    a->aux.shared = shared;
    a->flags |= MRB_ARY_SHARED;
  }
}

static void
ary_expand_capa(mrb_state *mrb, struct RArray *a, int len)
{
  int capa = a->aux.capa;

#ifdef INT_MAX
  if (len > ARY_MAX_SIZE) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "array size too big");
  }
#endif

  while(capa < len) {
    if (capa == 0) {
      capa = ARY_DEFAULT_LEN;
    }
    else {
      capa *= 2;
    }
  }

#ifdef INT_MAX
  if (capa > ARY_MAX_SIZE) capa = ARY_MAX_SIZE; /* len <= capa <= ARY_MAX_SIZE */
#endif

  if (capa > a->aux.capa) {
    a->aux.capa = capa;
    a->ptr = (mrb_value *)mrb_realloc(mrb, a->ptr, sizeof(mrb_value)*capa);
  }
}

static void
ary_shrink_capa(mrb_state *mrb, struct RArray *a)
{
  int capa = a->aux.capa;

  if (capa < ARY_DEFAULT_LEN * 2) return;
  if (capa <= a->len * ARY_SHRINK_RATIO) return;

  do {
    capa /= 2;
    if (capa < ARY_DEFAULT_LEN) {
      capa = ARY_DEFAULT_LEN;
      break;
    }
  } while(capa > a->len * ARY_SHRINK_RATIO);

  if (capa > a->len && capa < a->aux.capa) {
    a->aux.capa = capa;
    a->ptr = (mrb_value *)mrb_realloc(mrb, a->ptr, sizeof(mrb_value)*capa);
  }
}

mrb_value
mrb_ary_s_create(mrb_state *mrb, mrb_value self)
{
  mrb_value *vals;
  int len;

  mrb_get_args(mrb, "*", &vals, &len);
  return mrb_ary_new_from_values(mrb, (int)len, vals);
}

static void
ary_concat(mrb_state *mrb, struct RArray *a, mrb_value *ptr, int blen)
{
  int len = a->len + blen;

  ary_modify(mrb, a);
  if (a->aux.capa < len) ary_expand_capa(mrb, a, len);
  array_copy(a->ptr+a->len, ptr, blen);
  mrb_write_barrier(mrb, (struct RBasic*)a);
  a->len = len;
}

void
mrb_ary_concat(mrb_state *mrb, mrb_value self, mrb_value other)
{
  struct RArray *a2 = mrb_ary_ptr(other);

  ary_concat(mrb, mrb_ary_ptr(self), a2->ptr, a2->len);
}

mrb_value
mrb_ary_concat_m(mrb_state *mrb, mrb_value self)
{
  mrb_value *ptr;
  int blen;

  mrb_get_args(mrb, "a", &ptr, &blen);
  ary_concat(mrb, mrb_ary_ptr(self), ptr, blen);
  return self;
}

mrb_value
mrb_ary_plus(mrb_state *mrb, mrb_value self)
{
  struct RArray *a1 = mrb_ary_ptr(self);
  struct RArray *a2;
  mrb_value ary;
  mrb_value *ptr;
  int blen;

  mrb_get_args(mrb, "a", &ptr, &blen);
  ary = mrb_ary_new_capa(mrb, a1->len + blen);
  a2 = mrb_ary_ptr(ary);
  array_copy(a2->ptr, a1->ptr, a1->len);
  array_copy(a2->ptr + a1->len, ptr, blen);
  a2->len = a1->len + blen;

  return ary;
}

/*
 *  call-seq:
 *     ary <=> other_ary   ->  -1, 0, +1 or nil
 *
 *  Comparison---Returns an integer (-1, 0, or +1)
 *  if this array is less than, equal to, or greater than <i>other_ary</i>.
 *  Each object in each array is compared (using <=>). If any value isn't
 *  equal, then that inequality is the return value. If all the
 *  values found are equal, then the return is based on a
 *  comparison of the array lengths.  Thus, two arrays are
 *  ``equal'' according to <code>Array#<=></code> if and only if they have
 *  the same length and the value of each element is equal to the
 *  value of the corresponding element in the other array.
 *
 *     [ "a", "a", "c" ]    <=> [ "a", "b", "c" ]   #=> -1
 *     [ 1, 2, 3, 4, 5, 6 ] <=> [ 1, 2 ]            #=> +1
 *
 */
mrb_value
mrb_ary_cmp(mrb_state *mrb, mrb_value ary1)
{
  mrb_value ary2;
  struct RArray *a1, *a2;
  mrb_value r = mrb_nil_value();
  int i, len;

  mrb_get_args(mrb, "o", &ary2);
  if (!mrb_array_p(ary2)) return mrb_nil_value();
  a1 = RARRAY(ary1); a2 = RARRAY(ary2);
  if (a1->len == a2->len && a1->ptr == a2->ptr) return mrb_fixnum_value(0);
  else {
    mrb_sym cmp = mrb_intern(mrb, "<=>");

    len = RARRAY_LEN(ary1);
    if (len > RARRAY_LEN(ary2)) {
      len = RARRAY_LEN(ary2);
    }
    for (i=0; i<len; i++) {
      mrb_value v = ary_elt(ary2, i);
      r = mrb_funcall_argv(mrb, ary_elt(ary1, i), cmp, 1, &v);
      if (mrb_type(r) != MRB_TT_FIXNUM || mrb_fixnum(r) != 0) return r;
    }
  }
  len = a1->len - a2->len;
  return mrb_fixnum_value((len == 0)? 0: (len > 0)? 1: -1);
}

static void
ary_replace(mrb_state *mrb, struct RArray *a, mrb_value *argv, int len)
{
  ary_modify(mrb, a);
  if (a->aux.capa < len)
    ary_expand_capa(mrb, a, len);
  array_copy(a->ptr, argv, len);
  mrb_write_barrier(mrb, (struct RBasic*)a);
  a->len = len;
}

void
mrb_ary_replace(mrb_state *mrb, mrb_value self, mrb_value other)
{
  struct RArray *a2 = mrb_ary_ptr(other);

  ary_replace(mrb, mrb_ary_ptr(self), a2->ptr, a2->len);
}

mrb_value
mrb_ary_replace_m(mrb_state *mrb, mrb_value self)
{
  mrb_value other;

  mrb_get_args(mrb, "A", &other);
  mrb_ary_replace(mrb, self, other);

  return self;
}

mrb_value
mrb_ary_times(mrb_state *mrb, mrb_value self)
{
  struct RArray *a1 = mrb_ary_ptr(self);
  struct RArray *a2;
  mrb_value ary;
  mrb_value *ptr;
  mrb_int times;

  mrb_get_args(mrb, "i", &times);
  if (times < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative argument");
  }
  if (times == 0) return mrb_ary_new(mrb);

  ary = mrb_ary_new_capa(mrb, a1->len * times);
  a2 = mrb_ary_ptr(ary);
  ptr = a2->ptr;
  while(times--) {
    array_copy(ptr, a1->ptr, a1->len);
    ptr += a1->len;
    a2->len += a1->len;
  }

  return ary;
}

mrb_value
mrb_ary_reverse_bang(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  if (a->len > 1) {
    mrb_value *p1, *p2;

    ary_modify(mrb, a);
    p1 = a->ptr;
    p2 = a->ptr + a->len - 1;

    while(p1 < p2) {
      mrb_value tmp = *p1;
      *p1++ = *p2;
      *p2-- = tmp;
    }
  }
  return self;
}

mrb_value
mrb_ary_reverse(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self), *b;
  mrb_value ary;

  ary = mrb_ary_new_capa(mrb, a->len);
  b = mrb_ary_ptr(ary);
  if (a->len > 0) {
    mrb_value *p1, *p2, *e;

    p1 = a->ptr;
    e  = p1 + a->len;
    p2 = b->ptr + a->len - 1;
    while(p1 < e) {
      *p2-- = *p1++;
    }
    b->len = a->len;
  }
  return ary;
}

mrb_value
mrb_ary_new4(mrb_state *mrb, int n, const mrb_value *elts)
{
  mrb_value ary;

  ary = mrb_ary_new_capa(mrb, n);
  if (n > 0 && elts) {
    array_copy(RARRAY_PTR(ary), elts, n);
    RARRAY_LEN(ary) = n;
  }

  return ary;
}

mrb_value
mrb_ary_new_elts(mrb_state *mrb, int n, const mrb_value *elts)
{
  return mrb_ary_new4(mrb, n, elts);
}

void
mrb_ary_push(mrb_state *mrb, mrb_value ary, mrb_value elem) /* mrb_ary_push */
{
  struct RArray *a = mrb_ary_ptr(ary);

  ary_modify(mrb, a);
  if (a->len == a->aux.capa)
    ary_expand_capa(mrb, a, a->len + 1);
  a->ptr[a->len++] = elem;
  mrb_write_barrier(mrb, (struct RBasic*)a);
}

mrb_value
mrb_ary_push_m(mrb_state *mrb, mrb_value self)
{
  mrb_value *argv;
  int len;

  mrb_get_args(mrb, "*", &argv, &len);
  while(len--) {
    mrb_ary_push(mrb, self, *argv++);
  }

  return self;
}

mrb_value
mrb_ary_pop(mrb_state *mrb, mrb_value ary)
{
  struct RArray *a = mrb_ary_ptr(ary);

  if (a->len == 0) return mrb_nil_value();
  return a->ptr[--a->len];
}

#define ARY_SHIFT_SHARED_MIN 10

mrb_value
mrb_ary_shift(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_value val;

  if (a->len == 0) return mrb_nil_value();
  if (a->flags & MRB_ARY_SHARED) {
  L_SHIFT:
    val = a->ptr[0];
    a->ptr++;
    a->len--;
    return val;
  }
  if (a->len > ARY_SHIFT_SHARED_MIN) {
    ary_make_shared(mrb, a);
    goto L_SHIFT;
  }
  else {
    mrb_value *ptr = a->ptr;
    int size = a->len;

    val = *ptr;
    while((int)(--size)) {
      *ptr = *(ptr+1);
      ++ptr;
    }
    --a->len;
  }
  return val;
}

/* self = [1,2,3]
   item = 0
   self.unshift item
   p self #=> [0, 1, 2, 3] */
mrb_value
mrb_ary_unshift(mrb_state *mrb, mrb_value self, mrb_value item)
{
  struct RArray *a = mrb_ary_ptr(self);

  if ((a->flags & MRB_ARY_SHARED)
      && a->aux.shared->refcnt == 1 /* shared only referenced from this array */
      && a->ptr - a->aux.shared->ptr >= 1) /* there's room for unshifted item */ {
    a->ptr--;
    a->ptr[0] = item;
  }
  else {
    ary_modify(mrb, a);
    if (a->aux.capa < a->len + 1)
      ary_expand_capa(mrb, a, a->len + 1);
    memmove(a->ptr + 1, a->ptr, sizeof(mrb_value)*a->len);
    a->ptr[0] = item;
  }
  a->len++;
  mrb_write_barrier(mrb, (struct RBasic*)a);

  return self;
}

mrb_value
mrb_ary_unshift_m(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_value *vals;
  int len;

  mrb_get_args(mrb, "*", &vals, &len);
  if ((a->flags & MRB_ARY_SHARED)
      && a->aux.shared->refcnt == 1 /* shared only referenced from this array */
      && a->ptr - a->aux.shared->ptr >= len) /* there's room for unshifted item */ {
    a->ptr -= len;
  }
  else {
    ary_modify(mrb, a);
    if (len == 0) return self;
    if (a->aux.capa < a->len + len)
      ary_expand_capa(mrb, a, a->len + len);
    memmove(a->ptr + len, a->ptr, sizeof(mrb_value)*a->len);
  }
  array_copy(a->ptr, vals, len);
  a->len += len;
  mrb_write_barrier(mrb, (struct RBasic*)a);

  return self;
}

mrb_value
mrb_ary_ref(mrb_state *mrb, mrb_value ary, mrb_int n)
{
  struct RArray *a = mrb_ary_ptr(ary);

  /* range check */
  if (n < 0) n += a->len;
  if (n < 0 || a->len <= (int)n) return mrb_nil_value();

  return a->ptr[n];
}

void
mrb_ary_set(mrb_state *mrb, mrb_value ary, mrb_int n, mrb_value val) /* rb_ary_store */
{
  struct RArray *a = mrb_ary_ptr(ary);

  ary_modify(mrb, a);
  /* range check */
  if (n < 0) {
    n += a->len;
    if (n < 0) {
      mrb_raisef(mrb, E_INDEX_ERROR, "index %ld out of array", n - a->len);
    }
  }
  if (a->len <= (int)n) {
    if (a->aux.capa <= (int)n)
      ary_expand_capa(mrb, a, n + 1);
    ary_fill_with_nil(a->ptr + a->len, n + 1 - a->len);
    a->len = n + 1;
  }

  a->ptr[n] = val;
  mrb_write_barrier(mrb, (struct RBasic*)a);
}

mrb_value
mrb_ary_splice(mrb_state *mrb, mrb_value ary, mrb_int head, mrb_int len, mrb_value rpl)
{
  struct RArray *a = mrb_ary_ptr(ary);
  int tail, size;
  mrb_value *argv;
  int i, argc;

  ary_modify(mrb, a);
  /* range check */
  if (head < 0) {
    head += a->len;
    if (head < 0) {
      mrb_raise(mrb, E_INDEX_ERROR, "index is out of array");
    }
  }
  if (a->len < len || a->len < head + len) {
    len = a->len - head;
  }
  tail = head + len;

  /* size check */
  if (mrb_array_p(rpl)) {
    argc = RARRAY_LEN(rpl);
    argv = RARRAY_PTR(rpl);
  }
  else {
    argc = 1;
    argv = &rpl;
  }
  size = head + argc;

  if (tail < a->len) size += a->len - tail;
  if (size > a->aux.capa)
    ary_expand_capa(mrb, a, size);

  if (head > a->len) {
    ary_fill_with_nil(a->ptr + a->len, (int)(head - a->len));
  }
  else if (head < a->len) {
    memmove(a->ptr + head + argc, a->ptr + tail, sizeof(mrb_value)*(a->len - tail));
  }

  for(i = 0; i < argc; i++) {
    *(a->ptr + head + i) = *(argv + i);
  }

  a->len = size;

  return ary;
}

int
mrb_ary_alen(mrb_state *mrb, mrb_value ary)
{
  return RARRAY_LEN(ary);
}

void
mrb_ary_decref(mrb_state *mrb, struct mrb_shared_array *shared)
{
  shared->refcnt--;
  if (shared->refcnt == 0) {
    mrb_free(mrb, shared->ptr);
    mrb_free(mrb, shared);
  }
}

static mrb_value
ary_subseq(mrb_state *mrb, struct RArray *a, int beg, int len)
{
  struct RArray *b;

  ary_make_shared(mrb, a);
  b  = (struct RArray*)mrb_obj_alloc(mrb, MRB_TT_ARRAY, mrb->array_class);
  b->ptr = a->ptr + beg;
  b->len = len;
  b->aux.shared = a->aux.shared;
  b->aux.shared->refcnt++;
  b->flags |= MRB_ARY_SHARED;

  return mrb_obj_value(b);
}

mrb_value
mrb_ary_aget(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int index, len;
  mrb_value *argv;
  int size;

  mrb_get_args(mrb, "i*", &index, &argv, &size);
  switch(size) {
  case 0:
    return mrb_ary_ref(mrb, self, index);

  case 1:
    if (mrb_type(argv[0]) != MRB_TT_FIXNUM) {
      mrb_raise(mrb, E_TYPE_ERROR, "expected Fixnum");
    }
    if (index < 0) index += a->len;
    if (index < 0 || a->len < (int)index) return mrb_nil_value();
    len = mrb_fixnum(argv[0]);
    if (len < 0) return mrb_nil_value();
    if (a->len == (int)index) return mrb_ary_new(mrb);
    if ((int)len > a->len - index) len = a->len - index;
    return ary_subseq(mrb, a, index, len);

  default:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
  }

  return mrb_nil_value(); /* dummy to avoid warning : not reach here */
}

mrb_value
mrb_ary_aset(mrb_state *mrb, mrb_value self)
{
  mrb_value *argv;
  int argc;

  mrb_get_args(mrb, "*", &argv, &argc);
  switch(argc) {
  case 2:
    if (!mrb_fixnum_p(argv[0])) {
      /* Should we support Range object for 1st arg ? */
      mrb_raise(mrb, E_TYPE_ERROR, "expected Fixnum for 1st argument");
    }
    mrb_ary_set(mrb, self, mrb_fixnum(argv[0]), argv[1]);
    return argv[1];

  case 3:
    mrb_ary_splice(mrb, self, mrb_fixnum(argv[0]), mrb_fixnum(argv[1]), argv[2]);
    return argv[2];

  default:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
    return mrb_nil_value();
  }
}

mrb_value
mrb_ary_delete_at(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int   index;
  mrb_value val;
  mrb_value *ptr;
  int len;

  mrb_get_args(mrb, "i", &index);
  if (index < 0) index += a->len;
  if (index < 0 || a->len <= (int)index) return mrb_nil_value();

  ary_modify(mrb, a);
  val = a->ptr[index];

  ptr = a->ptr + index;
  len = a->len - index;
  while((int)(--len)) {
    *ptr = *(ptr+1);
    ++ptr;
  }
  --a->len;

  ary_shrink_capa(mrb, a);

  return val;
}

mrb_value
mrb_ary_first(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  mrb_int size;

  if (mrb_get_args(mrb, "|i", &size) == 0) {
    return (a->len > 0)? a->ptr[0]: mrb_nil_value();
  }
  if (size < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative array size");
  }

  if (size > a->len) size = a->len;
  if (a->flags & MRB_ARY_SHARED) {
    return ary_subseq(mrb, a, 0, size);
  }
  return mrb_ary_new_from_values(mrb, size, a->ptr);
}

mrb_value
mrb_ary_last(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);
  int size;
  mrb_value *vals;
  int len;

  mrb_get_args(mrb, "*", &vals, &len);
  if (len > 1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
  }

  if (len == 0) return (a->len > 0)? a->ptr[a->len - 1]: mrb_nil_value();

  /* len == 1 */
  size = mrb_fixnum(*vals);
  if (size < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "negative array size");
  }
  if (size > a->len) size = a->len;
  if ((a->flags & MRB_ARY_SHARED) || size > ARY_DEFAULT_LEN) {
    return ary_subseq(mrb, a, a->len - size, size);
  }
  return mrb_ary_new_from_values(mrb, size, a->ptr + a->len - size);
}

mrb_value
mrb_ary_index_m(mrb_state *mrb, mrb_value self)
{
  mrb_value obj;
  int i;

  mrb_get_args(mrb, "o", &obj);
  for (i = 0; i < RARRAY_LEN(self); i++) {
    if (mrb_equal(mrb, RARRAY_PTR(self)[i], obj)) {
      return mrb_fixnum_value(i);
    }
  }
  return mrb_nil_value();
}

mrb_value
mrb_ary_rindex_m(mrb_state *mrb, mrb_value self)
{
  mrb_value obj;
  int i;

  mrb_get_args(mrb, "o", &obj);
  for (i = RARRAY_LEN(self) - 1; i >= 0; i--) {
    if (mrb_equal(mrb, RARRAY_PTR(self)[i], obj)) {
      return mrb_fixnum_value(i);
    }
  }
  return mrb_nil_value();
}

mrb_value
mrb_ary_splat(mrb_state *mrb, mrb_value v)
{
  if (mrb_array_p(v)) {
    return v;
  }
  else {
    return mrb_ary_new_from_values(mrb, 1, &v);
  }
}

static mrb_value
mrb_ary_size(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  return mrb_fixnum_value(a->len);
}

mrb_value
mrb_ary_clear(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  ary_modify(mrb, a);
  a->len = 0;
  a->aux.capa = 0;
  mrb_free(mrb, a->ptr);
  a->ptr = 0;

  return self;
}

mrb_value
mrb_ary_empty_p(mrb_state *mrb, mrb_value self)
{
  struct RArray *a = mrb_ary_ptr(self);

  return ((a->len == 0)? mrb_true_value(): mrb_false_value());
}

mrb_value
mrb_check_array_type(mrb_state *mrb, mrb_value ary)
{
    return mrb_check_convert_type(mrb, ary, MRB_TT_ARRAY, "Array", "to_ary");
}

mrb_value
mrb_ary_entry(mrb_value ary, int offset)
{
  if (offset < 0) {
    offset += RARRAY_LEN(ary);
  }
  return ary_elt(ary, offset);
}

static mrb_value
inspect_ary(mrb_state *mrb, mrb_value ary, mrb_value list)
{
  int i;
  mrb_value s, arystr;
  char head[] = { '[' };
  char sep[] = { ',', ' ' };
  char tail[] = { ']' };

  /* check recursive */
  for(i=0; i<RARRAY_LEN(list); i++) {
    if (mrb_obj_equal(mrb, ary, RARRAY_PTR(list)[i])) {
      return mrb_str_new(mrb, "[...]", 5);
    }
  }

  mrb_ary_push(mrb, list, ary);

  arystr = mrb_str_buf_new(mrb, 64);
  mrb_str_buf_cat(mrb, arystr, head, sizeof(head));

  for(i=0; i<RARRAY_LEN(ary); i++) {
    int ai = mrb_gc_arena_save(mrb);

    if (i > 0) {
      mrb_str_buf_cat(mrb, arystr, sep, sizeof(sep));
    }
    if (mrb_array_p(RARRAY_PTR(ary)[i])) {
      s = inspect_ary(mrb, RARRAY_PTR(ary)[i], list);
    } else {
      s = mrb_inspect(mrb, RARRAY_PTR(ary)[i]);
    }
    mrb_str_buf_cat(mrb, arystr, RSTRING_PTR(s), RSTRING_LEN(s));
    mrb_gc_arena_restore(mrb, ai);
  }

  mrb_str_buf_cat(mrb, arystr, tail, sizeof(tail));
  mrb_ary_pop(mrb, list);

  return arystr;
}

/* 15.2.12.5.31 (x) */
/*
 *  call-seq:
 *     ary.to_s -> string
 *     ary.inspect  -> string
 *
 *  Creates a string representation of +self+.
 */

static mrb_value
mrb_ary_inspect(mrb_state *mrb, mrb_value ary)
{
  if (RARRAY_LEN(ary) == 0) return mrb_str_new(mrb, "[]", 2);
  #if 0 /* THREAD */
    return mrb_exec_recursive(inspect_ary_r, ary, 0);
  #else
    return inspect_ary(mrb, ary, mrb_ary_new(mrb));
  #endif
}

static mrb_value
join_ary(mrb_state *mrb, mrb_value ary, mrb_value sep, mrb_value list)
{
  int i;
  mrb_value result, val, tmp;

  /* check recursive */
  for(i=0; i<RARRAY_LEN(list); i++) {
    if (mrb_obj_equal(mrb, ary, RARRAY_PTR(list)[i])) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "recursive array join");
    }
  }

  mrb_ary_push(mrb, list, ary);

  result = mrb_str_buf_new(mrb, 64);

  for(i=0; i<RARRAY_LEN(ary); i++) {
    if (i > 0 && !mrb_nil_p(sep)) {
      mrb_str_buf_cat(mrb, result, RSTRING_PTR(sep), RSTRING_LEN(sep));
    }

    val = RARRAY_PTR(ary)[i];
    switch(mrb_type(val)) {
    case MRB_TT_ARRAY:
    ary_join:
      val = join_ary(mrb, val, sep, list);
      /* fall through */

    case MRB_TT_STRING:
    str_join:
      mrb_str_buf_cat(mrb, result, RSTRING_PTR(val), RSTRING_LEN(val));
      break;

    default:
      tmp = mrb_check_string_type(mrb, val);
      if (!mrb_nil_p(tmp)) {
        val = tmp;
        goto str_join;
      }
      tmp = mrb_check_convert_type(mrb, val, MRB_TT_ARRAY, "Array", "to_ary");
      if (!mrb_nil_p(tmp)) {
        val = tmp;
        goto ary_join;
      }
      val = mrb_obj_as_string(mrb, val);
      goto str_join;
    }
  }

  mrb_ary_pop(mrb, list);

  return result;
}

mrb_value
mrb_ary_join(mrb_state *mrb, mrb_value ary, mrb_value sep)
{
  sep = mrb_obj_as_string(mrb, sep);
  return join_ary(mrb, ary, sep, mrb_ary_new(mrb));
}

/*
 *  call-seq:
 *     ary.join(sep="")    -> str
 *
 *  Returns a string created by converting each element of the array to
 *  a string, separated by <i>sep</i>.
 *
 *     [ "a", "b", "c" ].join        #=> "abc"
 *     [ "a", "b", "c" ].join("-")   #=> "a-b-c"
 */

static mrb_value
mrb_ary_join_m(mrb_state *mrb, mrb_value ary)
{
  mrb_value sep = mrb_nil_value();

  mrb_get_args(mrb, "|S", &sep);
  return mrb_ary_join(mrb, ary, sep);
}

/* 15.2.12.5.33 (x) */
/*
 *  call-seq:
 *     ary == other_ary   ->   bool
 *
 *  Equality---Two arrays are equal if they contain the same number
 *  of elements and if each element is equal to (according to
 *  Object.==) the corresponding element in the other array.
 *
 *     [ "a", "c" ]    == [ "a", "c", 7 ]     #=> false
 *     [ "a", "c", 7 ] == [ "a", "c", 7 ]     #=> true
 *     [ "a", "c", 7 ] == [ "a", "d", "f" ]   #=> false
 *
 */

static mrb_value
mrb_ary_equal(mrb_state *mrb, mrb_value ary1)
{
  mrb_value ary2;

  mrb_get_args(mrb, "o", &ary2);
  if (mrb_obj_equal(mrb, ary1, ary2)) return mrb_true_value();
  if (mrb_special_const_p(ary2)) return mrb_false_value();
  if (!mrb_array_p(ary2)) {
    if (!mrb_respond_to(mrb, ary2, mrb_intern(mrb, "to_ary"))) {
        return mrb_false_value();
    }
    if (mrb_equal(mrb, ary2, ary1)){
      return mrb_true_value();
    }
    else {
      return mrb_false_value();
    }
  }
  if (RARRAY_LEN(ary1) != RARRAY_LEN(ary2)) return mrb_false_value();
  else {
    int i;

    for (i=0; i<RARRAY_LEN(ary1); i++) {
      if (!mrb_equal(mrb, ary_elt(ary1, i), ary_elt(ary2, i)))
        return mrb_false_value();
    }
    return mrb_true_value();
  }
}

/* 15.2.12.5.34 (x) */
/*
 *  call-seq:
 *     ary.eql?(other)  -> true or false
 *
 *  Returns <code>true</code> if +self+ and _other_ are the same object,
 *  or are both arrays with the same content.
 */

static mrb_value
mrb_ary_eql(mrb_state *mrb, mrb_value ary1)
{
  mrb_value ary2;

  mrb_get_args(mrb, "o", &ary2);
  if (mrb_obj_equal(mrb, ary1, ary2)) return mrb_true_value();
  if (!mrb_array_p(ary2)) return mrb_false_value();
  if (RARRAY_LEN(ary1) != RARRAY_LEN(ary2)) return mrb_false_value();
  else {
    int i;

    for (i=0; i<RARRAY_LEN(ary1); i++) {
      if (!mrb_eql(mrb, ary_elt(ary1, i), ary_elt(ary2, i)))
	return mrb_false_value();
    }
    return mrb_true_value();
  }
}

void
mrb_init_array(mrb_state *mrb)
{
  struct RClass *a;

  a = mrb->array_class = mrb_define_class(mrb, "Array", mrb->object_class);
  MRB_SET_INSTANCE_TT(a, MRB_TT_ARRAY);
  mrb_include_module(mrb, a, mrb_class_get(mrb, "Enumerable"));

  mrb_define_class_method(mrb, a, "[]",        mrb_ary_s_create,     ARGS_ANY());  /* 15.2.12.4.1 */

  mrb_define_method(mrb, a, "*",               mrb_ary_times,        ARGS_REQ(1)); /* 15.2.12.5.1  */
  mrb_define_method(mrb, a, "+",               mrb_ary_plus,         ARGS_REQ(1)); /* 15.2.12.5.2  */
  mrb_define_method(mrb, a, "<<",              mrb_ary_push_m,       ARGS_REQ(1)); /* 15.2.12.5.3  */
  mrb_define_method(mrb, a, "[]",              mrb_ary_aget,         ARGS_ANY());  /* 15.2.12.5.4  */
  mrb_define_method(mrb, a, "[]=",             mrb_ary_aset,         ARGS_ANY());  /* 15.2.12.5.5  */
  mrb_define_method(mrb, a, "clear",           mrb_ary_clear,        ARGS_NONE()); /* 15.2.12.5.6  */
  mrb_define_method(mrb, a, "concat",          mrb_ary_concat_m,     ARGS_REQ(1)); /* 15.2.12.5.8  */
  mrb_define_method(mrb, a, "delete_at",       mrb_ary_delete_at,    ARGS_REQ(1)); /* 15.2.12.5.9  */
  mrb_define_method(mrb, a, "empty?",          mrb_ary_empty_p,      ARGS_NONE()); /* 15.2.12.5.12 */
  mrb_define_method(mrb, a, "first",           mrb_ary_first,        ARGS_OPT(1)); /* 15.2.12.5.13 */
  mrb_define_method(mrb, a, "index",           mrb_ary_index_m,      ARGS_REQ(1)); /* 15.2.12.5.14 */
  mrb_define_method(mrb, a, "initialize_copy", mrb_ary_replace_m,    ARGS_REQ(1)); /* 15.2.12.5.16 */
  mrb_define_method(mrb, a, "join",            mrb_ary_join_m,       ARGS_ANY());  /* 15.2.12.5.17 */
  mrb_define_method(mrb, a, "last",            mrb_ary_last,         ARGS_ANY());  /* 15.2.12.5.18 */
  mrb_define_method(mrb, a, "length",          mrb_ary_size,         ARGS_NONE()); /* 15.2.12.5.19 */
  mrb_define_method(mrb, a, "pop",             mrb_ary_pop,          ARGS_NONE()); /* 15.2.12.5.21 */
  mrb_define_method(mrb, a, "push",            mrb_ary_push_m,       ARGS_ANY());  /* 15.2.12.5.22 */
  mrb_define_method(mrb, a, "replace",         mrb_ary_replace_m,    ARGS_REQ(1)); /* 15.2.12.5.23 */
  mrb_define_method(mrb, a, "reverse",         mrb_ary_reverse,      ARGS_NONE()); /* 15.2.12.5.24 */
  mrb_define_method(mrb, a, "reverse!",        mrb_ary_reverse_bang, ARGS_NONE()); /* 15.2.12.5.25 */
  mrb_define_method(mrb, a, "rindex",          mrb_ary_rindex_m,     ARGS_REQ(1)); /* 15.2.12.5.26 */
  mrb_define_method(mrb, a, "shift",           mrb_ary_shift,        ARGS_NONE()); /* 15.2.12.5.27 */
  mrb_define_method(mrb, a, "size",            mrb_ary_size,         ARGS_NONE()); /* 15.2.12.5.28 */
  mrb_define_method(mrb, a, "slice",           mrb_ary_aget,         ARGS_ANY());  /* 15.2.12.5.29 */
  mrb_define_method(mrb, a, "unshift",         mrb_ary_unshift_m,    ARGS_ANY());  /* 15.2.12.5.30 */

  mrb_define_method(mrb, a, "inspect",         mrb_ary_inspect,      ARGS_NONE()); /* 15.2.12.5.31 (x) */
  mrb_define_alias(mrb,   a, "to_s", "inspect");                                   /* 15.2.12.5.32 (x) */
  mrb_define_method(mrb, a, "==",              mrb_ary_equal,        ARGS_REQ(1)); /* 15.2.12.5.33 (x) */
  mrb_define_method(mrb, a, "eql?",            mrb_ary_eql,          ARGS_REQ(1)); /* 15.2.12.5.34 (x) */
  mrb_define_method(mrb, a, "<=>",             mrb_ary_cmp,          ARGS_REQ(1)); /* 15.2.12.5.36 (x) */
}
