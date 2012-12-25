/*
** mruby/array.h - Array class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_ARRAY_H
#define MRUBY_ARRAY_H

#if defined(__cplusplus)
extern "C" {
#endif

struct mrb_shared_array {
  int refcnt;
  mrb_value *ptr;
  int len;
};

struct RArray {
  MRB_OBJECT_HEADER;
  int len;
  union {
    int capa;
    struct mrb_shared_array *shared;
  } aux;
  mrb_value *ptr;
};

#define mrb_ary_ptr(v)    ((struct RArray*)((v).value.p))
#define mrb_ary_value(p)  mrb_obj_value((void*)(p))
#define RARRAY(v)  ((struct RArray*)((v).value.p))

#define RARRAY_LEN(a) (RARRAY(a)->len)
#define RARRAY_PTR(a) (RARRAY(a)->ptr)
#define MRB_ARY_SHARED      256

void mrb_ary_decref(mrb_state*, struct mrb_shared_array*);
mrb_value mrb_ary_new_capa(mrb_state*, int);
mrb_value mrb_ary_new(mrb_state *mrb);
mrb_value mrb_ary_new_elts(mrb_state *mrb, int n, const mrb_value *elts);
void mrb_ary_concat(mrb_state*, mrb_value, mrb_value);
mrb_value mrb_ary_splat(mrb_state*, mrb_value);
void mrb_ary_push(mrb_state*, mrb_value, mrb_value);
mrb_value mrb_ary_pop(mrb_state *mrb, mrb_value ary);
mrb_value mrb_ary_new_from_values(mrb_state *mrb, int size, mrb_value *vals);
mrb_value mrb_ary_aget(mrb_state *mrb, mrb_value self);
mrb_value mrb_ary_ref(mrb_state *mrb, mrb_value ary, mrb_int n);
void mrb_ary_set(mrb_state *mrb, mrb_value ary, mrb_int n, mrb_value val);
int mrb_ary_len(mrb_state *mrb, mrb_value ary);
void mrb_ary_replace(mrb_state *mrb, mrb_value a, mrb_value b);
mrb_value mrb_check_array_type(mrb_state *mrb, mrb_value self);
mrb_value mrb_ary_unshift(mrb_state *mrb, mrb_value self, mrb_value item);
mrb_value mrb_ary_new4(mrb_state *mrb, int n, const mrb_value *elts);
mrb_value mrb_assoc_new(mrb_state *mrb, mrb_value car, mrb_value cdr);
mrb_value mrb_ary_entry(mrb_value ary, int offset);
mrb_value mrb_ary_shift(mrb_state *mrb, mrb_value self);

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif  /* MRUBY_ARRAY_H */
