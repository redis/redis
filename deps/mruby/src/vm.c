/*
** vm.c - virtual machine for mruby
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "opcode.h"
#include "mruby/irep.h"
#include "mruby/variable.h"
#include "mruby/proc.h"
#include "mruby/array.h"
#include "mruby/string.h"
#include "mruby/hash.h"
#include "mruby/range.h"
#include "mruby/class.h"
#include "mruby/numeric.h"
#include "error.h"

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdarg.h>

#define SET_TRUE_VALUE(r) MRB_SET_VALUE(r, MRB_TT_TRUE, value.i, 1)
#define SET_FALSE_VALUE(r) MRB_SET_VALUE(r, MRB_TT_FALSE, value.i, 1)
#define SET_NIL_VALUE(r) MRB_SET_VALUE(r, MRB_TT_FALSE, value.i, 0)
#define SET_INT_VALUE(r,n) MRB_SET_VALUE(r, MRB_TT_FIXNUM, value.i, (n))
#define SET_SYM_VALUE(r,v) MRB_SET_VALUE(r, MRB_TT_SYMBOL, value.sym, (v))
#define SET_OBJ_VALUE(r,v) MRB_SET_VALUE(r, (((struct RObject*)(v))->tt), value.p, (v))
#ifdef MRB_NAN_BOXING
#define SET_FLT_VALUE(r,v) r.f = (v)
#else
#define SET_FLT_VALUE(r,v) MRB_SET_VALUE(r, MRB_TT_FLOAT, value.f, (v))
#endif

#define STACK_INIT_SIZE 128
#define CALLINFO_INIT_SIZE 32

/* Define amount of linear stack growth. */
#ifndef MRB_STACK_GROWTH
#define MRB_STACK_GROWTH 128
#endif

/* Maximum stack depth. Should be set lower on memory constrained systems.
The value below allows about 60000 recursive calls in the simplest case. */
#ifndef MRB_STACK_MAX
#define MRB_STACK_MAX ((1<<18) - MRB_STACK_GROWTH)
#endif



static inline void
stack_copy(mrb_value *dst, const mrb_value *src, size_t size)
{
  int i;

  for (i = 0; i < size; i++) {
    dst[i] = src[i];
  }
}

static void
stack_init(mrb_state *mrb)
{
  /* assert(mrb->stack == NULL); */
  mrb->stbase = (mrb_value *)mrb_calloc(mrb, STACK_INIT_SIZE,   sizeof(mrb_value));
  mrb->stend = mrb->stbase + STACK_INIT_SIZE;
  mrb->stack = mrb->stbase;

  /* assert(mrb->ci == NULL); */
  mrb->cibase = (mrb_callinfo *)mrb_calloc(mrb, CALLINFO_INIT_SIZE, sizeof(mrb_callinfo));
  mrb->ciend = mrb->cibase + CALLINFO_INIT_SIZE;
  mrb->ci = mrb->cibase;
  mrb->ci->target_class = mrb->object_class;
}

static void
envadjust(mrb_state *mrb, mrb_value *oldbase, mrb_value *newbase)
{
  mrb_callinfo *ci = mrb->cibase;

  while (ci <= mrb->ci) {
    struct REnv *e = ci->env;
    if (e && e->cioff >= 0) {
      int off = e->stack - oldbase;

      e->stack = newbase + off;
    }
    ci++;
  }
}

/** def rec ; $deep =+ 1 ; if $deep > 1000 ; return 0 ; end ; rec ; end  */

static void
stack_extend(mrb_state *mrb, int room, int keep)
{
  int size, off;
  if (mrb->stack + room >= mrb->stend) {
    mrb_value *oldbase = mrb->stbase;

    size = mrb->stend - mrb->stbase;
    off = mrb->stack - mrb->stbase;

    /* Use linear stack growth.
       It is slightly slower than doubling thestack space,
       but it saves memory on small devices. */
    if (room <= size)
      size += MRB_STACK_GROWTH;
    else
      size += room;

    mrb->stbase = (mrb_value *)mrb_realloc(mrb, mrb->stbase, sizeof(mrb_value) * size);
    mrb->stack = mrb->stbase + off;
    mrb->stend = mrb->stbase + size;
    envadjust(mrb, oldbase, mrb->stbase);
    /* Raise an exception if the new stack size will be too large,
    to prevent infinite recursion. However, do this only after resizing the stack, so mrb_raisef has stack space to work with. */
    if(size > MRB_STACK_MAX) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "stack level too deep. (limit=%d)", MRB_STACK_MAX);
    }
  }

  if (room > keep) {
    int i;
    for (i=keep; i<room; i++) {
#ifndef MRB_NAN_BOXING
      static const mrb_value mrb_value_zero = { { 0 } };
      mrb->stack[i] = mrb_value_zero;
#else
      SET_NIL_VALUE(mrb->stack[i]);
#endif
    }
  }
}

int
mrb_checkstack(mrb_state *mrb, int size)
{
  stack_extend(mrb, size+1, 1);
  return 0;
}

struct REnv*
uvenv(mrb_state *mrb, int up)
{
  struct REnv *e = mrb->ci->proc->env;

  while (up--) {
    if (!e) return 0;
    e = (struct REnv*)e->c;
  }
  return e;
}

static mrb_value
uvget(mrb_state *mrb, int up, int idx)
{
  struct REnv *e = uvenv(mrb, up);

  if (!e) return mrb_nil_value();
  return e->stack[idx];
}

static void
uvset(mrb_state *mrb, int up, int idx, mrb_value v)
{
  struct REnv *e = uvenv(mrb, up);

  if (!e) return;
  e->stack[idx] = v;
  mrb_write_barrier(mrb, (struct RBasic*)e);
}

static inline int
is_strict(mrb_state *mrb, struct REnv *e)
{
  int cioff = e->cioff;

  if (cioff >= 0 && mrb->cibase[cioff].proc &&
      MRB_PROC_STRICT_P(mrb->cibase[cioff].proc)) {
    return 1;
  }
  return 0;
}

struct REnv*
top_env(mrb_state *mrb, struct RProc *proc)
{
  struct REnv *e = proc->env;

  if (is_strict(mrb, e)) return e;
  while (e->c) {
    e = (struct REnv*)e->c;
    if (is_strict(mrb, e)) return e;
  }
  return e;
}

static mrb_callinfo*
cipush(mrb_state *mrb)
{
  int eidx = mrb->ci->eidx;
  int ridx = mrb->ci->ridx;

  if (mrb->ci + 1 == mrb->ciend) {
    size_t size = mrb->ci - mrb->cibase;

    mrb->cibase = (mrb_callinfo *)mrb_realloc(mrb, mrb->cibase, sizeof(mrb_callinfo)*size*2);
    mrb->ci = mrb->cibase + size;
    mrb->ciend = mrb->cibase + size * 2;
  }
  mrb->ci++;
  mrb->ci->nregs = 2;		/* protect method_missing arg and block */
  mrb->ci->eidx = eidx;
  mrb->ci->ridx = ridx;
  mrb->ci->env = 0;
  return mrb->ci;
}

static void
cipop(mrb_state *mrb)
{
  if (mrb->ci->env) {
    struct REnv *e = mrb->ci->env;
    int len = (int)e->flags;
    mrb_value *p = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value)*len);

    e->cioff = -1;
    stack_copy(p, e->stack, len);
    e->stack = p;
  }

  mrb->ci--;
}

static void
ecall(mrb_state *mrb, int i)
{
  struct RProc *p;
  mrb_callinfo *ci;
  mrb_value *self = mrb->stack;
  struct RObject *exc;

  p = mrb->ensure[i];
  ci = cipush(mrb);
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->mid = ci[-1].mid;
  ci->acc = -1;
  ci->argc = 0;
  ci->proc = p;
  ci->nregs = p->body.irep->nregs;
  ci->target_class = p->target_class;
  mrb->stack = mrb->stack + ci[-1].nregs;
  exc = mrb->exc; mrb->exc = 0;
  mrb_run(mrb, p, *self);
  if (!mrb->exc) mrb->exc = exc;
}

#ifndef MRB_FUNCALL_ARGC_MAX
#define MRB_FUNCALL_ARGC_MAX 16
#endif

mrb_value
mrb_funcall(mrb_state *mrb, mrb_value self, const char *name, int argc, ...)
{
  mrb_sym mid = mrb_intern(mrb, name);
  va_list ap;
  int i;

  if (argc == 0) {
    return mrb_funcall_argv(mrb, self, mid, 0, 0);
  }
  else if (argc == 1) {
    mrb_value v;

    va_start(ap, argc);
    v = va_arg(ap, mrb_value);
    va_end(ap);
    return mrb_funcall_argv(mrb, self, mid, 1, &v);
  }
  else {
    mrb_value argv[MRB_FUNCALL_ARGC_MAX];

    if (argc > MRB_FUNCALL_ARGC_MAX) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "Too long arguments. (limit=%d)", MRB_FUNCALL_ARGC_MAX);
    }

    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
      argv[i] = va_arg(ap, mrb_value);
    }
    va_end(ap);
    return mrb_funcall_argv(mrb, self, mid, argc, argv);
  }
}

mrb_value
mrb_funcall_with_block(mrb_state *mrb, mrb_value self, mrb_sym mid, int argc, mrb_value *argv, mrb_value blk)
{
  struct RProc *p;
  struct RClass *c;
  mrb_sym undef = 0;
  mrb_callinfo *ci;
  int n;
  mrb_value val;

  if (!mrb->jmp) {
    jmp_buf c_jmp;

    if (setjmp(c_jmp) != 0) {	/* error */
      mrb->jmp = 0;
      return mrb_nil_value();
    }
    mrb->jmp = &c_jmp;
    /* recursive call */
    val = mrb_funcall_with_block(mrb, self, mid, argc, argv, blk);
    mrb->jmp = 0;
    return val;
  }

  if (!mrb->stack) {
    stack_init(mrb);
  }
  n = mrb->ci->nregs;
  if (argc < 0) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "negative argc for funcall (%d)", argc);
  }
  c = mrb_class(mrb, self);
  p = mrb_method_search_vm(mrb, &c, mid);
  if (!p) {
    undef = mid;
    mid = mrb_intern(mrb, "method_missing");
    p = mrb_method_search_vm(mrb, &c, mid);
    n++; argc++;
  }
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->argc = argc;
  ci->target_class = p->target_class;
  if (MRB_PROC_CFUNC_P(p)) {
    ci->nregs = argc + 2;
  }
  else {
    ci->nregs = p->body.irep->nregs + 2;
  }
  ci->acc = -1;
  mrb->stack = mrb->stack + n;

  stack_extend(mrb, ci->nregs, 0);
  mrb->stack[0] = self;
  if (undef) {
    mrb->stack[1] = mrb_symbol_value(undef);
    stack_copy(mrb->stack+2, argv, argc-1);
  }
  else if (argc > 0) {
    stack_copy(mrb->stack+1, argv, argc);
  }
  mrb->stack[argc+1] = blk;

  if (MRB_PROC_CFUNC_P(p)) {
    int ai = mrb_gc_arena_save(mrb);
    val = p->body.func(mrb, self);
    mrb_gc_arena_restore(mrb, ai);
    mrb_gc_protect(mrb, val);
    mrb->stack = mrb->stbase + mrb->ci->stackidx;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }
  return val;
}

mrb_value
mrb_funcall_argv(mrb_state *mrb, mrb_value self, mrb_sym mid, int argc, mrb_value *argv)
{
  return mrb_funcall_with_block(mrb, self, mid, argc, argv, mrb_nil_value());
}

mrb_value
mrb_yield_internal(mrb_state *mrb, mrb_value b, int argc, mrb_value *argv, mrb_value self, struct RClass *c)
{
  struct RProc *p;
  mrb_sym mid = mrb->ci->mid;
  mrb_callinfo *ci;
  int n = mrb->ci->nregs;
  mrb_value val;

  p = mrb_proc_ptr(b);
  ci = cipush(mrb);
  ci->mid = mid;
  ci->proc = p;
  ci->stackidx = mrb->stack - mrb->stbase;
  ci->argc = argc;
  ci->target_class = c;
  if (MRB_PROC_CFUNC_P(p)) {
    ci->nregs = argc + 2;
  }
  else {
    ci->nregs = p->body.irep->nregs + 2;
  }
  ci->acc = -1;
  mrb->stack = mrb->stack + n;

  stack_extend(mrb, ci->nregs, 0);
  mrb->stack[0] = self;
  if (argc > 0) {
    stack_copy(mrb->stack+1, argv, argc);
  }
  mrb->stack[argc+1] = mrb_nil_value();

  if (MRB_PROC_CFUNC_P(p)) {
    val = p->body.func(mrb, self);
    mrb->stack = mrb->stbase + mrb->ci->stackidx;
    cipop(mrb);
  }
  else {
    val = mrb_run(mrb, p, self);
  }
  return val;
}

mrb_value
mrb_yield_argv(mrb_state *mrb, mrb_value b, int argc, mrb_value *argv)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_internal(mrb, b, argc, argv, mrb->stack[0], p->target_class);
}

mrb_value
mrb_yield(mrb_state *mrb, mrb_value b, mrb_value v)
{
  struct RProc *p = mrb_proc_ptr(b);

  return mrb_yield_internal(mrb, b, 1, &v, mrb->stack[0], p->target_class);
}

static void
localjump_error(mrb_state *mrb, const char *kind)
{
  char buf[256];
  int len;
  mrb_value exc;

  len = snprintf(buf, sizeof(buf), "unexpected %s", kind);
  exc = mrb_exc_new(mrb, E_LOCALJUMP_ERROR, buf, len);
  mrb->exc = (struct RObject*)mrb_object(exc);
}

static void
argnum_error(mrb_state *mrb, int num)
{
  char buf[256];
  int len;
  mrb_value exc;

  if (mrb->ci->mid) {
    len = snprintf(buf, sizeof(buf), "'%s': wrong number of arguments (%d for %d)",
		   mrb_sym2name(mrb, mrb->ci->mid),
		   mrb->ci->argc, num);
  }
  else {
    len = snprintf(buf, sizeof(buf), "wrong number of arguments (%d for %d)",
		   mrb->ci->argc, num);
  }
  exc = mrb_exc_new(mrb, E_ARGUMENT_ERROR, buf, len);
  mrb->exc = (struct RObject*)mrb_object(exc);
}

#ifdef __GNUC__
#define DIRECT_THREADED
#endif

#ifndef DIRECT_THREADED

#define INIT_DISPATCH for (;;) { i = *pc; switch (GET_OPCODE(i)) {
#define CASE(op) case op:
#define NEXT pc++; break
#define JUMP break
#define END_DISPATCH }}

#else

#define INIT_DISPATCH JUMP; return mrb_nil_value();
#define CASE(op) L_ ## op:
#define NEXT i=*++pc; goto *optable[GET_OPCODE(i)]
#define JUMP i=*pc; goto *optable[GET_OPCODE(i)]

#define END_DISPATCH

#endif

mrb_value mrb_gv_val_get(mrb_state *mrb, mrb_sym sym);
void mrb_gv_val_set(mrb_state *mrb, mrb_sym sym, mrb_value val);

#define CALL_MAXARGS 127

mrb_value
mrb_run(mrb_state *mrb, struct RProc *proc, mrb_value self)
{
  /* assert(mrb_proc_cfunc_p(proc)) */
  mrb_irep *irep = proc->body.irep;
  mrb_code *pc = irep->iseq;
  mrb_value *pool = irep->pool;
  mrb_sym *syms = irep->syms;
  mrb_value *regs = NULL;
  mrb_code i;
  int ai = mrb_gc_arena_save(mrb);
  jmp_buf *prev_jmp = (jmp_buf *)mrb->jmp;
  jmp_buf c_jmp;

#ifdef DIRECT_THREADED
  static void *optable[] = {
    &&L_OP_NOP, &&L_OP_MOVE,
    &&L_OP_LOADL, &&L_OP_LOADI, &&L_OP_LOADSYM, &&L_OP_LOADNIL,
    &&L_OP_LOADSELF, &&L_OP_LOADT, &&L_OP_LOADF,
    &&L_OP_GETGLOBAL, &&L_OP_SETGLOBAL, &&L_OP_GETSPECIAL, &&L_OP_SETSPECIAL,
    &&L_OP_GETIV, &&L_OP_SETIV, &&L_OP_GETCV, &&L_OP_SETCV,
    &&L_OP_GETCONST, &&L_OP_SETCONST, &&L_OP_GETMCNST, &&L_OP_SETMCNST,
    &&L_OP_GETUPVAR, &&L_OP_SETUPVAR,
    &&L_OP_JMP, &&L_OP_JMPIF, &&L_OP_JMPNOT,
    &&L_OP_ONERR, &&L_OP_RESCUE, &&L_OP_POPERR, &&L_OP_RAISE, &&L_OP_EPUSH, &&L_OP_EPOP,
    &&L_OP_SEND, &&L_OP_SENDB, &&L_OP_FSEND,
    &&L_OP_CALL, &&L_OP_SUPER, &&L_OP_ARGARY, &&L_OP_ENTER,
    &&L_OP_KARG, &&L_OP_KDICT, &&L_OP_RETURN, &&L_OP_TAILCALL, &&L_OP_BLKPUSH,
    &&L_OP_ADD, &&L_OP_ADDI, &&L_OP_SUB, &&L_OP_SUBI, &&L_OP_MUL, &&L_OP_DIV,
    &&L_OP_EQ, &&L_OP_LT, &&L_OP_LE, &&L_OP_GT, &&L_OP_GE,
    &&L_OP_ARRAY, &&L_OP_ARYCAT, &&L_OP_ARYPUSH, &&L_OP_AREF, &&L_OP_ASET, &&L_OP_APOST,
    &&L_OP_STRING, &&L_OP_STRCAT, &&L_OP_HASH,
    &&L_OP_LAMBDA, &&L_OP_RANGE, &&L_OP_OCLASS,
    &&L_OP_CLASS, &&L_OP_MODULE, &&L_OP_EXEC,
    &&L_OP_METHOD, &&L_OP_SCLASS, &&L_OP_TCLASS,
    &&L_OP_DEBUG, &&L_OP_STOP, &&L_OP_ERR,
  };
#endif


  if (setjmp(c_jmp) == 0) {
    mrb->jmp = &c_jmp;
  }
  else {
    goto L_RAISE;
  }
  if (!mrb->stack) {
    stack_init(mrb);
  }
  mrb->ci->proc = proc;
  mrb->ci->nregs = irep->nregs + 2;
  regs = mrb->stack;
  regs[0] = self;

  INIT_DISPATCH {
    CASE(OP_NOP) {
      /* do nothing */
      NEXT;
    }

    CASE(OP_MOVE) {
      /* A B    R(A) := R(B) */
      regs[GETARG_A(i)] = regs[GETARG_B(i)];
      NEXT;
    }

    CASE(OP_LOADL) {
      /* A Bx   R(A) := Pool(Bx) */
      regs[GETARG_A(i)] = pool[GETARG_Bx(i)];
      NEXT;
    }

    CASE(OP_LOADI) {
      /* A Bx   R(A) := sBx */
      SET_INT_VALUE(regs[GETARG_A(i)], GETARG_sBx(i));
      NEXT;
    }

    CASE(OP_LOADSYM) {
      /* A B    R(A) := Sym(B) */
      SET_SYM_VALUE(regs[GETARG_A(i)], syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_LOADSELF) {
      /* A      R(A) := self */
      regs[GETARG_A(i)] = regs[0];
      NEXT;
    }

    CASE(OP_LOADT) {
      /* A      R(A) := true */
      SET_TRUE_VALUE(regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_LOADF) {
      /* A      R(A) := false */
      SET_FALSE_VALUE(regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETGLOBAL) {
      /* A B    R(A) := getglobal(Sym(B)) */
      regs[GETARG_A(i)] = mrb_gv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETGLOBAL) {
      /* setglobal(Sym(b), R(A)) */
      mrb_gv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETSPECIAL) {
      /* A Bx   R(A) := Special[Bx] */
      regs[GETARG_A(i)] = mrb_vm_special_get(mrb, GETARG_Bx(i));
      NEXT;
    }

    CASE(OP_SETSPECIAL) {
      /* A Bx   Special[Bx] := R(A) */
      mrb_vm_special_set(mrb, GETARG_Bx(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETIV) {
      /* A Bx   R(A) := ivget(Bx) */
      regs[GETARG_A(i)] = mrb_vm_iv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETIV) {
      /* ivset(Sym(B),R(A)) */
      mrb_vm_iv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCV) {
      /* A B    R(A) := ivget(Sym(B)) */
      regs[GETARG_A(i)] = mrb_vm_cv_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETCV) {
      /* ivset(Sym(B),R(A)) */
      mrb_vm_cv_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETCONST) {
      /* A B    R(A) := constget(Sym(B)) */
      regs[GETARG_A(i)] = mrb_vm_const_get(mrb, syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETCONST) {
      /* A B    constset(Sym(B),R(A)) */
      mrb_vm_const_set(mrb, syms[GETARG_Bx(i)], regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_GETMCNST) {
      /* A B C  R(A) := R(C)::Sym(B) */
      int a = GETARG_A(i);

      regs[a] = mrb_const_get(mrb, regs[a], syms[GETARG_Bx(i)]);
      NEXT;
    }

    CASE(OP_SETMCNST) {
      /* A B C  R(A+1)::Sym(B) := R(A) */
      int a = GETARG_A(i);

      mrb_const_set(mrb, regs[a+1], syms[GETARG_Bx(i)], regs[a]);
      NEXT;
    }

    CASE(OP_GETUPVAR) {
      /* A B C  R(A) := uvget(B,C) */

      regs[GETARG_A(i)] = uvget(mrb, GETARG_C(i), GETARG_B(i));
      NEXT;
    }

    CASE(OP_SETUPVAR) {
      /* A B C  uvset(B,C,R(A)) */
      uvset(mrb, GETARG_C(i), GETARG_B(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_JMP) {
      /* sBx    pc+=sBx */
      pc += GETARG_sBx(i);
      JUMP;
    }

    CASE(OP_JMPIF) {
      /* A sBx  if R(A) pc+=sBx */
      if (mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_JMPNOT) {
      /* A sBx  if R(A) pc+=sBx */
      if (!mrb_test(regs[GETARG_A(i)])) {
        pc += GETARG_sBx(i);
        JUMP;
      }
      NEXT;
    }

    CASE(OP_ONERR) {
      /* sBx    pc+=sBx on exception */
      if (mrb->rsize <= mrb->ci->ridx) {
        if (mrb->rsize == 0) mrb->rsize = 16;
        else mrb->rsize *= 2;
        mrb->rescue = (mrb_code **)mrb_realloc(mrb, mrb->rescue, sizeof(mrb_code*) * mrb->rsize);
      }
      mrb->rescue[mrb->ci->ridx++] = pc + GETARG_sBx(i);
      NEXT;
    }

    CASE(OP_RESCUE) {
      /* A      R(A) := exc; clear(exc) */
      SET_OBJ_VALUE(regs[GETARG_A(i)], mrb->exc);
      mrb->exc = 0;
      NEXT;
    }

    CASE(OP_POPERR) {
      int a = GETARG_A(i);

      while (a--) {
        mrb->ci->ridx--;
      }
      NEXT;
    }

    CASE(OP_RAISE) {
      /* A      raise(R(A)) */
      mrb->exc = (struct RObject*)mrb_object(regs[GETARG_A(i)]);
      goto L_RAISE;
    }

    CASE(OP_EPUSH) {
      /* Bx     ensure_push(SEQ[Bx]) */
      struct RProc *p;

      p = mrb_closure_new(mrb, mrb->irep[irep->idx+GETARG_Bx(i)]);
      /* push ensure_stack */
      if (mrb->esize <= mrb->ci->eidx) {
        if (mrb->esize == 0) mrb->esize = 16;
        else mrb->esize *= 2;
        mrb->ensure = (struct RProc **)mrb_realloc(mrb, mrb->ensure, sizeof(struct RProc*) * mrb->esize);
      }
      mrb->ensure[mrb->ci->eidx++] = p;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_EPOP) {
      /* A      A.times{ensure_pop().call} */
      int n;
      int a = GETARG_A(i);

      for (n=0; n<a; n++) {
        ecall(mrb, --mrb->ci->eidx);
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_LOADNIL) {
      /* A B    R(A) := nil */
      int a = GETARG_A(i);

      SET_NIL_VALUE(regs[a]);
      NEXT;
    }

    CASE(OP_SENDB) {
      /* fall through */
    };

  L_SEND:
    CASE(OP_SEND) {
      /* A B C  R(A) := call(R(A),Sym(B),R(A+1),... ,R(A+C-1)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv, result;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      if (GET_OPCODE(i) != OP_SENDB) {
	if (n == CALL_MAXARGS) {
	  SET_NIL_VALUE(regs[a+2]);
	}
	else {
	  SET_NIL_VALUE(regs[a+n+1]);
	}
      }
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
	  regs[a+1] = sym;
          n++;
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;
      ci->target_class = c;
      ci->pc = pc + 1;
      ci->acc = a;

      /* prepare stack */
      mrb->stack += a;

      if (MRB_PROC_CFUNC_P(m)) {
        if (n == CALL_MAXARGS) {
          ci->nregs = 3;
        }
        else {
          ci->nregs = n + 2;
        }
        result = m->body.func(mrb, recv);
        mrb->stack[0] = result;
        mrb_gc_arena_restore(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        regs = mrb->stack = mrb->stbase + mrb->ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* setup environment for calling method */
        proc = mrb->ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_FSEND) {
      /* A B C  R(A) := fcall(R(A),Sym(B),R(A+1),... ,R(A+C)) */
      NEXT;
    }

    CASE(OP_CALL) {
      /* A      R(A) := self.call(frame.argc, frame.argv) */
      mrb_callinfo *ci;
      mrb_value recv = mrb->stack[0];
      struct RProc *m = mrb_proc_ptr(recv);

      /* replace callinfo */
      ci = mrb->ci;
      ci->target_class = m->target_class;
      ci->proc = m;
      if (m->env) {
	if (m->env->mid) {
	  ci->mid = m->env->mid;
	}
        if (!m->env->stack) {
          m->env->stack = mrb->stack;
        }
      }

      /* prepare stack */
      if (MRB_PROC_CFUNC_P(m)) {
	recv = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
	ci = mrb->ci;
        regs = mrb->stack = mrb->stbase + ci->stackidx;
	regs[ci->acc] = recv;
	pc = ci->pc;
        cipop(mrb);
        irep = mrb->ci->proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        JUMP;
      }
      else {
        /* setup environment for calling method */
        proc = m;
        irep = m->body.irep;
	if (!irep) {
	  mrb->stack[0] = mrb_nil_value();
	  goto L_RETURN;
	}
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        regs[0] = m->env->stack[0];
        pc = m->body.irep->iseq;
        JUMP;
      }
    }

    CASE(OP_SUPER) {
      /* A B C  R(A) := super(R(A+1),... ,R(A+C-1)) */
      mrb_value recv;
      mrb_callinfo *ci = mrb->ci;
      struct RProc *m;
      struct RClass *c;
      mrb_sym mid = ci->mid;
      int a = GETARG_A(i);
      int n = GETARG_C(i);

      recv = regs[0];
      c = mrb->ci->target_class->super;
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], mrb_symbol_value(ci->mid));
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
	  SET_SYM_VALUE(regs[a+1], ci->mid);
          n++;
        }
      }

      /* push callinfo */
      ci = cipush(mrb);
      ci->mid = mid;
      ci->proc = m;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;
      ci->target_class = m->target_class;
      ci->pc = pc + 1;

      /* prepare stack */
      mrb->stack += a;
      mrb->stack[0] = recv;

      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        regs = mrb->stack = mrb->stbase + mrb->ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        /* fill callinfo */
        ci->acc = a;

        /* setup environment for calling method */
        ci->proc = m;
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        ci->nregs = irep->nregs;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_ARGARY) {
      /* A Bx   R(A) := argument array (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
        if (!e) {
          mrb_value exc;
          static const char m[] = "super called outside of method";
          exc = mrb_exc_new(mrb, E_NOMETHOD_ERROR, m, sizeof(m) - 1);
          mrb->exc = (struct RObject*)mrb_object(exc);
          goto L_RAISE;
        }
        stack = e->stack + 1;
      }
      if (r == 0) {
        regs[a] = mrb_ary_new_elts(mrb, m1+m2, stack);
      }
      else {
        mrb_value *pp = NULL;
        struct RArray *rest;
        int len = 0;

        if (mrb_array_p(stack[m1])) {
          struct RArray *ary = mrb_ary_ptr(stack[m1]);

          pp = ary->ptr;
          len = ary->len;
        }
        regs[a] = mrb_ary_new_capa(mrb, m1+len+m2);
        rest = mrb_ary_ptr(regs[a]);
        stack_copy(rest->ptr, stack, m1);
        if (len > 0) {
          stack_copy(rest->ptr+m1, pp, len);
        }
        if (m2 > 0) {
          stack_copy(rest->ptr+m1+len, stack+m1+1, m2);
        }
        rest->len = m1+len+m2;
      }
      regs[a+1] = stack[m1+r+m2];
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ENTER) {
      /* Ax             arg setup according to flags (24=5:5:1:5:5:1:1) */
      /* number of optional arguments times OP_JMP should follow */
      int ax = GETARG_Ax(i);
      int m1 = (ax>>18)&0x1f;
      int o  = (ax>>13)&0x1f;
      int r  = (ax>>12)&0x1;
      int m2 = (ax>>7)&0x1f;
      /* unused
      int k  = (ax>>2)&0x1f;
      int kd = (ax>>1)&0x1;
      int b  = (ax>>0)& 0x1;
      */
      int argc = mrb->ci->argc;
      mrb_value *argv = regs+1;
      mrb_value *argv0 = argv;
      int len = m1 + o + r + m2;
      mrb_value *blk = &argv[argc < 0 ? 1 : argc];

      if (argc < 0) {
        struct RArray *ary = mrb_ary_ptr(regs[1]);
        argv = ary->ptr;
        argc = ary->len;
	mrb_gc_protect(mrb, regs[1]);
      }
      if (mrb->ci->proc && MRB_PROC_STRICT_P(mrb->ci->proc)) {
        if (argc >= 0) {
          if (argc < m1 + m2 || (r == 0 && argc > len)) {
	    argnum_error(mrb, m1+m2);
	    goto L_RAISE;
          }
        }
      }
      else if (len > 1 && argc == 1 && mrb_array_p(argv[0])) {
        argc = mrb_ary_ptr(argv[0])->len;
        argv = mrb_ary_ptr(argv[0])->ptr;
      }
      mrb->ci->argc = len;
      if (argc < len) {
        regs[len+1] = *blk; /* move block */
        if (argv0 != argv) {
          memmove(&regs[1], argv, sizeof(mrb_value)*(argc-m2)); /* m1 + o */
        }
        if (m2) {
          memmove(&regs[len-m2+1], &argv[argc-m2], sizeof(mrb_value)*m2); /* m2 */
        }
        if (r) {                  /* r */
          regs[m1+o+1] = mrb_ary_new_capa(mrb, 0);
        }
	if (o == 0) pc++;
	else
	  pc += argc - m1 - m2 + 1;
      }
      else {
        if (argv0 != argv) {
          memmove(&regs[1], argv, sizeof(mrb_value)*(m1+o)); /* m1 + o */
        }
        if (r) {                  /* r */
          regs[m1+o+1] = mrb_ary_new_elts(mrb, argc-m1-o-m2, argv+m1+o);
        }
        if (m2) {
          memmove(&regs[m1+o+r+1], &argv[argc-m2], sizeof(mrb_value)*m2);
        }
        regs[len+1] = *blk; /* move block */
        pc += o + 1;
      }
      JUMP;
    }

    CASE(OP_KARG) {
      /* A B C          R(A) := kdict[Sym(B)]; if C kdict.rm(Sym(B)) */
      /* if C == 2; raise unless kdict.empty? */
      /* OP_JMP should follow to skip init code */
      NEXT;
    }

    CASE(OP_KDICT) {
      /* A C            R(A) := kdict */
      NEXT;
    }

    CASE(OP_RETURN) {
      /* A      return R(A) */
    L_RETURN:
      if (mrb->exc) {
        mrb_callinfo *ci;
        int eidx;

      L_RAISE:
	ci = mrb->ci;
	mrb_obj_iv_ifnone(mrb, mrb->exc, mrb_intern(mrb, "lastpc"), mrb_voidp_value(pc));
	mrb_obj_iv_set(mrb, mrb->exc, mrb_intern(mrb, "ciidx"), mrb_fixnum_value(ci - mrb->cibase));
	eidx = ci->eidx;
        if (ci == mrb->cibase) {
	  if (ci->ridx == 0) goto L_STOP;
	  goto L_RESCUE;
	}
        while (ci[0].ridx == ci[-1].ridx) {
          cipop(mrb);
          ci = mrb->ci;
	  if (ci[1].acc < 0 && prev_jmp) {
	    mrb->jmp = prev_jmp;
	    longjmp(*(jmp_buf*)mrb->jmp, 1);
	  }
	  while (eidx > mrb->ci->eidx) {
	    ecall(mrb, --eidx);
	  }
          if (ci == mrb->cibase) {
            if (ci->ridx == 0) {
	      regs = mrb->stack = mrb->stbase;
	      goto L_STOP;
	    }
            break;
          }
        }
      L_RESCUE:
        irep = ci->proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        regs = mrb->stack = mrb->stbase + ci[1].stackidx;
        pc = mrb->rescue[--ci->ridx];
      }
      else {
        mrb_callinfo *ci = mrb->ci;
        int acc, eidx = mrb->ci->eidx;
        mrb_value v = regs[GETARG_A(i)];

        switch (GETARG_B(i)) {
        case OP_R_RETURN:
          // Fall through to OP_R_NORMAL otherwise
          if (proc->env && !MRB_PROC_STRICT_P(proc)) {
            struct REnv *e = top_env(mrb, proc);

            if (e->cioff < 0) {
              localjump_error(mrb, "return");
              goto L_RAISE;
            }
            ci = mrb->cibase + e->cioff;
	    if (ci == mrb->cibase) {
              localjump_error(mrb, "return");
              goto L_RAISE;
	    }
	    mrb->ci = ci;
            break;
          }
        case OP_R_NORMAL:
          if (ci == mrb->cibase) {
            localjump_error(mrb, "return");
            goto L_RAISE;
          }
          ci = mrb->ci;
          break;
        case OP_R_BREAK:
          if (proc->env->cioff < 0) {
            localjump_error(mrb, "break");
            goto L_RAISE;
          }
          ci = mrb->ci = mrb->cibase + proc->env->cioff + 1;
          break;
        default:
          /* cannot happen */
          break;
        }
        cipop(mrb);
        acc = ci->acc;
        pc = ci->pc;
        regs = mrb->stack = mrb->stbase + ci->stackidx;
        while (eidx > mrb->ci->eidx) {
          ecall(mrb, --eidx);
        }
        if (acc < 0) {
          mrb->jmp = prev_jmp;
          return v;
        }
        DEBUG(printf("from :%s\n", mrb_sym2name(mrb, ci->mid)));
        proc = mrb->ci->proc;
        irep = proc->body.irep;
        pool = irep->pool;
        syms = irep->syms;

        regs[acc] = v;
      }
      JUMP;
    }

    CASE(OP_TAILCALL) {
      /* A B C  return call(R(A),Sym(B),R(A+1),... ,R(A+C-1)) */
      int a = GETARG_A(i);
      int n = GETARG_C(i);
      struct RProc *m;
      struct RClass *c;
      mrb_callinfo *ci;
      mrb_value recv;
      mrb_sym mid = syms[GETARG_B(i)];

      recv = regs[a];
      c = mrb_class(mrb, recv);
      m = mrb_method_search_vm(mrb, &c, mid);
      if (!m) {
        mrb_value sym = mrb_symbol_value(mid);

        mid = mrb_intern(mrb, "method_missing");
        m = mrb_method_search_vm(mrb, &c, mid);
        if (n == CALL_MAXARGS) {
          mrb_ary_unshift(mrb, regs[a+1], sym);
        }
        else {
          memmove(regs+a+2, regs+a+1, sizeof(mrb_value)*(n+1));
          regs[a+1] = sym;
          n++;
        }
      }


      /* replace callinfo */
      ci = mrb->ci;
      ci->mid = mid;
      ci->target_class = m->target_class;
      ci->argc = n;
      if (ci->argc == CALL_MAXARGS) ci->argc = -1;

      /* move stack */
      memmove(mrb->stack, &regs[a], (ci->argc+1)*sizeof(mrb_value));

      if (MRB_PROC_CFUNC_P(m)) {
        mrb->stack[0] = m->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        goto L_RETURN;
      }
      else {
        /* setup environment for calling method */
        irep = m->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        if (ci->argc < 0) {
          stack_extend(mrb, (irep->nregs < 3) ? 3 : irep->nregs, 3);
        }
        else {
          stack_extend(mrb, irep->nregs,  ci->argc+2);
        }
        regs = mrb->stack;
        pc = irep->iseq;
      }
      JUMP;
    }

    CASE(OP_BLKPUSH) {
      /* A Bx   R(A) := block (16=6:1:5:4) */
      int a = GETARG_A(i);
      int bx = GETARG_Bx(i);
      int m1 = (bx>>10)&0x3f;
      int r  = (bx>>9)&0x1;
      int m2 = (bx>>4)&0x1f;
      int lv = (bx>>0)&0xf;
      mrb_value *stack;

      if (lv == 0) stack = regs + 1;
      else {
        struct REnv *e = uvenv(mrb, lv-1);
	if (!e) {
	  localjump_error(mrb, "yield");
	  goto L_RAISE;
	}
        stack = e->stack + 1;
      }
      regs[a] = stack[m1+r+m2];
      NEXT;
    }

#define attr_i value.i
#ifdef MRB_NAN_BOXING
#define attr_f f
#else
#define attr_f value.f
#endif

#define TYPES2(a,b) (((((int)(a))<<8)|((int)(b)))&0xffff)
#define OP_MATH_BODY(op,v1,v2) do {\
  regs[a].v1 = regs[a].v1 op regs[a+1].v2;\
} while(0)

    CASE(OP_ADD) {
      /* A B C  R(A) := R(A)+R(A+1) (Syms[B]=:+,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
	{
	  mrb_int x, y, z;

	  x = mrb_fixnum(regs[a]);
	  y = mrb_fixnum(regs[a+1]);
	  z = x + y;
	  if (((x < 0) ^ (y < 0)) == 0 && (x < 0) != (z < 0)) {
	    /* integer overflow */
	    SET_FLT_VALUE(regs[a], (mrb_float)x + (mrb_float)y);
	    break;
	  }
	  SET_INT_VALUE(regs[a], z);
	}
	break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
	{
	  mrb_int x = mrb_fixnum(regs[a]);
	  mrb_float y = mrb_float(regs[a+1]);
	  SET_FLT_VALUE(regs[a], (mrb_float)x + y);
	}
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
	OP_MATH_BODY(+,attr_f,attr_i);
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
	OP_MATH_BODY(+,attr_f,attr_f);
	break;
      case TYPES2(MRB_TT_STRING,MRB_TT_STRING):
	regs[a] = mrb_str_plus(mrb, regs[a], regs[a+1]);
	break;
      default:
	goto L_SEND;
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_SUB) {
      /* A B C  R(A) := R(A)-R(A+1) (Syms[B]=:-,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
	{
	  mrb_int x, y, z;

	  x = mrb_fixnum(regs[a]);
	  y = mrb_fixnum(regs[a+1]);
	  z = x - y;
	  if (((x < 0) ^ (y < 0)) != 0 && (x < 0) != (z < 0)) {
	    /* integer overflow */
	    SET_FLT_VALUE(regs[a], (mrb_float)x - (mrb_float)y);
	    break;
	  }
	  SET_INT_VALUE(regs[a], z);
	}
	break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
	{
	  mrb_int x = mrb_fixnum(regs[a]);
	  mrb_float y = mrb_float(regs[a+1]);
	  SET_FLT_VALUE(regs[a], (mrb_float)x - y);
	}
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
	OP_MATH_BODY(-,attr_f,attr_i);
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
	OP_MATH_BODY(-,attr_f,attr_f);
	break;
      default:
	goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_MUL) {
      /* A B C  R(A) := R(A)*R(A+1) (Syms[B]=:*,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
	{
	  mrb_int x, y, z;

	  x = mrb_fixnum(regs[a]);
	  y = mrb_fixnum(regs[a+1]);
	  z = x * y;
	  if (x != 0 && z/x != y) {
	    SET_FLT_VALUE(regs[a], (mrb_float)x * (mrb_float)y);
	  }
	  else {
	    SET_INT_VALUE(regs[a], z);
	  }
	}
	break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
	{
	  mrb_int x = mrb_fixnum(regs[a]);
	  mrb_float y = mrb_float(regs[a+1]);
	  SET_FLT_VALUE(regs[a], (mrb_float)x * y);
	}
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
	OP_MATH_BODY(*,attr_f,attr_i);
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
	OP_MATH_BODY(*,attr_f,attr_f);
	break;
      default:
	goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_DIV) {
      /* A B C  R(A) := R(A)/R(A+1) (Syms[B]=:/,C=1)*/
      int a = GETARG_A(i);

      /* need to check if op is overridden */
      switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):
	{
	  mrb_int x = mrb_fixnum(regs[a]);
	  mrb_int y = mrb_fixnum(regs[a+1]);
	  SET_FLT_VALUE(regs[a], (mrb_float)x / (mrb_float)y);
	}
	break;
      case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):
	{
	  mrb_int x = mrb_fixnum(regs[a]);
	  mrb_float y = mrb_float(regs[a+1]);
	  SET_FLT_VALUE(regs[a], (mrb_float)x / y);
	}
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):
	OP_MATH_BODY(/,attr_f,attr_i);
	break;
      case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):
	OP_MATH_BODY(/,attr_f,attr_f);
	break;
      default:
	goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_ADDI) {
      /* A B C  R(A) := R(A)+C (Syms[B]=:+)*/
      int a = GETARG_A(i);

      /* need to check if + is overridden */
      switch (mrb_type(regs[a])) {
      case MRB_TT_FIXNUM:
	{
	  mrb_int x = regs[a].attr_i;
	  mrb_int y = GETARG_C(i);
	  mrb_int z = x + y;

	  if (((x < 0) ^ (y < 0)) == 0 && (x < 0) != (z < 0)) {
	    /* integer overflow */
	    SET_FLT_VALUE(regs[a], (mrb_float)x + (mrb_float)y);
	    break;
	  }
	  regs[a].attr_i = z;
	}
        break;
      case MRB_TT_FLOAT:
        regs[a].attr_f += GETARG_C(i);
        break;
      default:
        SET_INT_VALUE(regs[a+1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

    CASE(OP_SUBI) {
      /* A B C  R(A) := R(A)-C (Syms[B]=:+)*/
      int a = GETARG_A(i);

      /* need to check if + is overridden */
      switch (mrb_type(regs[a])) {
      case MRB_TT_FIXNUM:
	{
	  mrb_int x = regs[a].attr_i;
	  mrb_int y = GETARG_C(i);
	  mrb_int z = x - y;

	  if (((x < 0) ^ (y < 0)) != 0 && (x < 0) != (z < 0)) {
	    /* integer overflow */
	    SET_FLT_VALUE(regs[a], (mrb_float)x - (mrb_float)y);
	    break;
	  }
	  regs[a].attr_i = z;
	}
        break;
      case MRB_TT_FLOAT:
        regs[a].attr_f -= GETARG_C(i);
        break;
      default:
        SET_INT_VALUE(regs[a+1], GETARG_C(i));
        i = MKOP_ABC(OP_SEND, a, GETARG_B(i), 1);
        goto L_SEND;
      }
      NEXT;
    }

#define OP_CMP_BODY(op,v1,v2) do {\
  if (regs[a].v1 op regs[a+1].v2) {\
    SET_TRUE_VALUE(regs[a]);\
  }\
  else {\
    SET_FALSE_VALUE(regs[a]);\
  }\
} while(0)

#define OP_CMP(op) do {\
  int a = GETARG_A(i);\
  /* need to check if - is overridden */\
  switch (TYPES2(mrb_type(regs[a]),mrb_type(regs[a+1]))) {\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FIXNUM):\
    OP_CMP_BODY(op,attr_i,attr_i);\
    break;\
  case TYPES2(MRB_TT_FIXNUM,MRB_TT_FLOAT):\
    OP_CMP_BODY(op,attr_i,attr_f);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FIXNUM):\
    OP_CMP_BODY(op,attr_f,attr_i);\
    break;\
  case TYPES2(MRB_TT_FLOAT,MRB_TT_FLOAT):\
    OP_CMP_BODY(op,attr_f,attr_f);\
    break;\
  default:\
    goto L_SEND;\
  }\
} while (0)

    CASE(OP_EQ) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      int a = GETARG_A(i);
      if (mrb_obj_eq(mrb, regs[a], regs[a+1])) {
	SET_TRUE_VALUE(regs[a]);
      }
      else {
	OP_CMP(==);
      }
      NEXT;
    }

    CASE(OP_LT) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(<);
      NEXT;
    }

    CASE(OP_LE) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(<=);
      NEXT;
    }

    CASE(OP_GT) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(>);
      NEXT;
    }

    CASE(OP_GE) {
      /* A B C  R(A) := R(A)<R(A+1) (Syms[B]=:<,C=1)*/
      OP_CMP(>=);
      NEXT;
    }

    CASE(OP_ARRAY) {
      /* A B C          R(A) := ary_new(R(B),R(B+1)..R(B+C)) */
      regs[GETARG_A(i)] = mrb_ary_new_from_values(mrb, GETARG_C(i), &regs[GETARG_B(i)]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYCAT) {
      /* A B            mrb_ary_concat(R(A),R(B)) */
      mrb_ary_concat(mrb, regs[GETARG_A(i)],
                     mrb_ary_splat(mrb, regs[GETARG_B(i)]));
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_ARYPUSH) {
      /* A B            R(A).push(R(B)) */
      mrb_ary_push(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_AREF) {
      /* A B C          R(A) := R(B)[C] */
      int a = GETARG_A(i);
      int c = GETARG_C(i);
      mrb_value v = regs[GETARG_B(i)];

      if (!mrb_array_p(v)) {
        if (c == 0) {
          regs[GETARG_A(i)] = v;
        }
        else {
          SET_NIL_VALUE(regs[a]);
        }
      }
      else {
	regs[GETARG_A(i)] = mrb_ary_ref(mrb, v, c);
      }
      NEXT;
    }

    CASE(OP_ASET) {
      /* A B C          R(B)[C] := R(A) */
      mrb_ary_set(mrb, regs[GETARG_B(i)], GETARG_C(i), regs[GETARG_A(i)]);
      NEXT;
    }

    CASE(OP_APOST) {
      /* A B C  *R(A),R(A+1)..R(A+C) := R(A) */
      int a = GETARG_A(i);
      mrb_value v = regs[a];
      int pre  = GETARG_B(i);
      int post = GETARG_C(i);

      if (!mrb_array_p(v)) {
        regs[a++] = mrb_ary_new_capa(mrb, 0);
        while (post--) {
          SET_NIL_VALUE(regs[a]);
          a++;
        }
      }
      else {
        struct RArray *ary = mrb_ary_ptr(v);
        int len = ary->len;
        int i;

        if (len > pre + post) {
          regs[a++] = mrb_ary_new_elts(mrb, len - pre - post, ary->ptr+pre);
          while (post--) {
	    regs[a++] = ary->ptr[len-post-1];
          }
        }
        else {
	  regs[a++] = mrb_ary_new_capa(mrb, 0);
          for (i=0; i+pre<len; i++) {
	    regs[a+i] = ary->ptr[pre+i];
          }
          while (i < post) {
            SET_NIL_VALUE(regs[a+i]);
            i++;
          }
        }
      }
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_STRING) {
      /* A Bx           R(A) := str_new(Lit(Bx)) */
      regs[GETARG_A(i)] = mrb_str_literal(mrb, pool[GETARG_Bx(i)]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_STRCAT) {
      /* A B    R(A).concat(R(B)) */
      mrb_str_concat(mrb, regs[GETARG_A(i)], regs[GETARG_B(i)]);
      NEXT;
    }

    CASE(OP_HASH) {
      /* A B C   R(A) := hash_new(R(B),R(B+1)..R(B+C)) */
      int b = GETARG_B(i);
      int c = GETARG_C(i);
      int lim = b+c*2;
      mrb_value hash = mrb_hash_new_capa(mrb, c);

      while (b < lim) {
        mrb_hash_set(mrb, hash, regs[b], regs[b+1]);
        b+=2;
      }
      regs[GETARG_A(i)] = hash;
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_LAMBDA) {
      /* A b c  R(A) := lambda(SEQ[b],c) (b:c = 14:2) */
      struct RProc *p;
      int c = GETARG_c(i);

      if (c & OP_L_CAPTURE) {
        p = mrb_closure_new(mrb, mrb->irep[irep->idx+GETARG_b(i)]);
      }
      else {
        p = mrb_proc_new(mrb, mrb->irep[irep->idx+GETARG_b(i)]);
      }
      if (c & OP_L_STRICT) p->flags |= MRB_PROC_STRICT;
      regs[GETARG_A(i)] = mrb_obj_value(p);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_OCLASS) {
      /* A      R(A) := ::Object */
      regs[GETARG_A(i)] = mrb_obj_value(mrb->object_class);
      NEXT;
    }

    CASE(OP_CLASS) {
      /* A B    R(A) := newclass(R(A),Sym(B),R(A+1)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base, super;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      super = regs[a+1];
      if (mrb_nil_p(base)) {
        base = mrb_obj_value(mrb->ci->target_class);
      }
      c = mrb_vm_define_class(mrb, base, super, id);
      regs[a] = mrb_obj_value(c);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_MODULE) {
      /* A B            R(A) := newmodule(R(A),Sym(B)) */
      struct RClass *c = 0;
      int a = GETARG_A(i);
      mrb_value base;
      mrb_sym id = syms[GETARG_B(i)];

      base = regs[a];
      if (mrb_nil_p(base)) {
	base = mrb_obj_value(mrb->ci->target_class);
      }
      c = mrb_vm_define_module(mrb, base, id);
      regs[a] = mrb_obj_value(c);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_EXEC) {
      /* A Bx   R(A) := blockexec(R(A),SEQ[Bx]) */
      int a = GETARG_A(i);
      mrb_callinfo *ci;
      mrb_value recv = regs[a];
      struct RProc *p;

      /* prepare stack */
      ci = cipush(mrb);
      ci->pc = pc + 1;
      ci->acc = a;
      ci->mid = 0;
      ci->stackidx = mrb->stack - mrb->stbase;
      ci->argc = 0;
      ci->target_class = mrb_class_ptr(recv);

      /* prepare stack */
      mrb->stack += a;

      p = mrb_proc_new(mrb, mrb->irep[irep->idx+GETARG_Bx(i)]);
      p->target_class = ci->target_class;
      ci->proc = p;

      if (MRB_PROC_CFUNC_P(p)) {
        mrb->stack[0] = p->body.func(mrb, recv);
        mrb_gc_arena_restore(mrb, ai);
        if (mrb->exc) goto L_RAISE;
        /* pop stackpos */
        regs = mrb->stack = mrb->stbase + mrb->ci->stackidx;
        cipop(mrb);
        NEXT;
      }
      else {
        irep = p->body.irep;
        pool = irep->pool;
        syms = irep->syms;
        stack_extend(mrb, irep->nregs, 1);
	ci->nregs = irep->nregs;
        regs = mrb->stack;
        pc = irep->iseq;
        JUMP;
      }
    }

    CASE(OP_METHOD) {
      /* A B            R(A).newmethod(Sym(B),R(A+1)) */
      int a = GETARG_A(i);
      struct RClass *c = mrb_class_ptr(regs[a]);

      mrb_define_method_vm(mrb, c, syms[GETARG_B(i)], regs[a+1]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_SCLASS) {
      /* A B    R(A) := R(B).singleton_class */
      regs[GETARG_A(i)] = mrb_singleton_class(mrb, regs[GETARG_B(i)]);
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_TCLASS) {
      /* A B    R(A) := target_class */
      if (!mrb->ci->target_class) {
        static const char msg[] = "no target class or module";
        mrb_value exc = mrb_exc_new(mrb, E_TYPE_ERROR, msg, sizeof(msg) - 1);
        mrb->exc = (struct RObject*)mrb_object(exc);
        goto L_RAISE;
      }
      regs[GETARG_A(i)] = mrb_obj_value(mrb->ci->target_class);
      NEXT;
    }

    CASE(OP_RANGE) {
      /* A B C  R(A) := range_new(R(B),R(B+1),C) */
      int b = GETARG_B(i);
      regs[GETARG_A(i)] = mrb_range_new(mrb, regs[b], regs[b+1], GETARG_C(i));
      mrb_gc_arena_restore(mrb, ai);
      NEXT;
    }

    CASE(OP_DEBUG) {
      /* A      debug print R(A),R(B),R(C) */
#ifdef ENABLE_STDIO
      printf("OP_DEBUG %d %d %d\n", GETARG_A(i), GETARG_B(i), GETARG_C(i));
#else
      abort();
#endif
      NEXT;
    }

    CASE(OP_STOP) {
      /*        stop VM */
    L_STOP:
      {
	int n = mrb->ci->eidx;

	while (n--) {
	  ecall(mrb, n);
	}
      }
      mrb->jmp = prev_jmp;
      if (mrb->exc) {
	return mrb_obj_value(mrb->exc);
      }
      return regs[irep->nlocals];
    }

    CASE(OP_ERR) {
      /* Bx     raise RuntimeError with message Lit(Bx) */
      mrb_value msg = pool[GETARG_Bx(i)];
      mrb_value exc;

      if (GETARG_A(i) == 0) {
	exc = mrb_exc_new3(mrb, E_RUNTIME_ERROR, msg);
      }
      else {
	exc = mrb_exc_new3(mrb, E_LOCALJUMP_ERROR, msg);
      }
      mrb->exc = (struct RObject*)mrb_object(exc);
      goto L_RAISE;
    }
  }
  END_DISPATCH;
}
