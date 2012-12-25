/*
** mruby/proc.h - Proc class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_PROC_H
#define MRUBY_PROC_H

#include "mruby/irep.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct REnv {
  MRB_OBJECT_HEADER;
  mrb_value *stack;
  mrb_sym mid;
  int cioff;
};

struct RProc {
  MRB_OBJECT_HEADER;
  union {
    mrb_irep *irep;
    mrb_func_t func;
  } body;
  struct RClass *target_class;
  struct REnv *env;
};

/* aspec access */
#define ARGS_GETREQ(a)          (((a) >> 19) & 0x1f)
#define ARGS_GETOPT(a)          (((a) >> 14) & 0x1f)
#define ARGS_GETREST(a)         ((a) & (1<<13))
#define ARGS_GETPOST(a)         (((a) >> 8) & 0x1f)
#define ARGS_GETKEY(a)          (((a) >> 3) & 0x1f))
#define ARGS_GETKDICT(a)        ((a) & (1<<2))
#define ARGS_GETBLOCK(a)        ((a) & (1<<1))

#define MRB_PROC_CFUNC 128
#define MRB_PROC_CFUNC_P(p) ((p)->flags & MRB_PROC_CFUNC)
#define MRB_PROC_STRICT 256
#define MRB_PROC_STRICT_P(p) ((p)->flags & MRB_PROC_STRICT)

#define mrb_proc_ptr(v)    ((struct RProc*)((v).value.p))

struct RProc *mrb_proc_new(mrb_state*, mrb_irep*);
struct RProc *mrb_proc_new_cfunc(mrb_state*, mrb_func_t);
struct RProc *mrb_closure_new(mrb_state*, mrb_irep*);
struct RProc *mrb_closure_new_cfunc(mrb_state *mrb, mrb_func_t func, int nlocals);
void mrb_proc_copy(struct RProc *a, struct RProc *b);

#include "mruby/khash.h"
KHASH_DECLARE(mt, mrb_sym, struct RProc*, 1)

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif  /* MRUBY_PROC_H */
