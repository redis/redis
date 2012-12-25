/*
** codegen.c - mruby code generator
**
** See Copyright Notice in mruby.h
*/

#undef CODEGEN_TEST
#define CODEGEN_DUMP

#include "mruby.h"
#include "mruby/string.h"
#include "mruby/irep.h"
#include "mruby/compile.h"
#include "mruby/numeric.h"
#include "opcode.h"
#include "node.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef mrb_ast_node node;
typedef struct mrb_parser_state parser_state;

enum looptype {
  LOOP_NORMAL,
  LOOP_BLOCK,
  LOOP_FOR,
  LOOP_BEGIN,
  LOOP_RESCUE,
} type;

struct loopinfo {
  enum looptype type;
  int pc1, pc2, pc3, acc;
  int ensure_level;
  struct loopinfo *prev;
};

typedef struct scope {
  mrb_state *mrb;
  mrb_pool *mpool;
  jmp_buf jmp;

  struct scope *prev;

  node *lv;

  int sp;
  int pc;
  int lastlabel;
  int ainfo:15;
  int mscope:1;

  struct loopinfo *loop;
  int ensure_level;
  char *filename;
  short lineno;

  mrb_code *iseq;
  short *lines;
  int icapa;

  mrb_irep *irep;
  int pcapa;
  int scapa;

  int nlocals;
  int nregs;
  int ai;

  int idx;
} codegen_scope;

static codegen_scope* scope_new(mrb_state *mrb, codegen_scope *prev, node *lv);
static void scope_finish(codegen_scope *s);
static struct loopinfo *loop_push(codegen_scope *s, enum looptype t);
static void loop_break(codegen_scope *s, node *tree);
static void loop_pop(codegen_scope *s, int val);

static void gen_assignment(codegen_scope *s, node *node, int sp, int val);
static void gen_vmassignment(codegen_scope *s, node *tree, int rhs, int val);

static void codegen(codegen_scope *s, node *tree, int val);

static void
codegen_error(codegen_scope *s, const char *message)
{
  if (!s) return;
  while (s->prev) {
    mrb_pool_close(s->mpool);
    s = s->prev;
  }
  mrb_pool_close(s->mpool);
#ifdef ENABLE_STDIO
  if (s->filename && s->lineno) {
    fprintf(stderr, "codegen error:%s:%d: %s\n", s->filename, s->lineno, message);
  }
  else {
    fprintf(stderr, "codegen error: %s\n", message);
  }
#endif
  longjmp(s->jmp, 1);
}

static void*
codegen_palloc(codegen_scope *s, size_t len)
{
  void *p = mrb_pool_alloc(s->mpool, len);

  if (!p) codegen_error(s, "pool memory allocation");
  return p;
}

void*
codegen_malloc(codegen_scope *s, size_t len)
{
  void *p = mrb_malloc(s->mrb, len);

  if (!p) codegen_error(s, "mrb_malloc");
  return p;
}

void*
codegen_realloc(codegen_scope *s, void *p, size_t len)
{
  p = mrb_realloc(s->mrb, p, len);

  if (!p && len > 0) codegen_error(s, "mrb_realloc");
  return p;
}

static int
new_label(codegen_scope *s)
{
  s->lastlabel = s->pc;
  return s->pc;
}

static inline void
genop(codegen_scope *s, mrb_code i)
{
  if (s->pc == s->icapa) {
    s->icapa *= 2;
    s->iseq = (mrb_code *)codegen_realloc(s, s->iseq, sizeof(mrb_code)*s->icapa);
    if (s->lines) {
      s->lines = (short*)codegen_realloc(s, s->lines, sizeof(short)*s->icapa);
    }
  }
  s->iseq[s->pc] = i;
  if (s->lines) {
    s->lines[s->pc] = s->lineno;
  }
  s->pc++;
}

#define NOVAL  0
#define VAL    1

static void
genop_peep(codegen_scope *s, mrb_code i, int val)
{
  /* peephole optimization */
  if (s->lastlabel != s->pc && s->pc > 0) {
    mrb_code i0 = s->iseq[s->pc-1];
    int c1 = GET_OPCODE(i);
    int c0 = GET_OPCODE(i0);

    switch (c1) {
    case OP_MOVE:
      if (GETARG_A(i) == GETARG_B(i)) {
	/* skip useless OP_MOVE */
	return;
      }
      if (val) break;
      switch (c0) {
      case OP_MOVE:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i) == GETARG_B(i0) && GETARG_A(i) >= s->nlocals) {
          /* skip swapping OP_MOVE */
          return;
        }
	if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
	  s->iseq[s->pc-1] = MKOP_AB(OP_MOVE, GETARG_A(i), GETARG_B(i0));
	  return;
	}
        break;
      case OP_LOADI:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_AsBx(OP_LOADI, GETARG_A(i), GETARG_sBx(i0));
          return;
        }
        break;
      case OP_ARRAY:
      case OP_HASH:
      case OP_RANGE:
      case OP_AREF:
      case OP_GETUPVAR:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_ABC(c0, GETARG_A(i), GETARG_B(i0), GETARG_C(i0));
          return;
        }
        break;
      case OP_LOADSYM:
      case OP_GETGLOBAL:
      case OP_GETIV:
      case OP_GETCV:
      case OP_GETCONST:
      case OP_GETSPECIAL:
      case OP_LOADL:
      case OP_STRING:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_ABx(c0, GETARG_A(i), GETARG_Bx(i0));
          return;
        }
        break;
      case OP_SCLASS:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_AB(c0, GETARG_A(i), GETARG_B(i0));
          return;
        }
        break;
      case OP_LOADNIL:
      case OP_LOADSELF:
      case OP_LOADT:
      case OP_LOADF:
      case OP_OCLASS:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_A(c0, GETARG_A(i));
          return;
        }
        break;
      default:
	break;
      }
      break;
    case OP_SETIV:
    case OP_SETCV:
    case OP_SETCONST:
    case OP_SETMCNST:
    case OP_SETGLOBAL:
      if (val) break;
      if (c0 == OP_MOVE) {
        if (GETARG_A(i) == GETARG_A(i0)) {
          s->iseq[s->pc-1] = MKOP_ABx(c1, GETARG_B(i0), GETARG_Bx(i));
          return;
        }
      }
      break;
    case OP_SETUPVAR:
      if (val) break;
      if (c0 == OP_MOVE) {
        if (GETARG_A(i) == GETARG_A(i0)) {
          s->iseq[s->pc-1] = MKOP_ABC(c1, GETARG_B(i0), GETARG_B(i), GETARG_C(i));
          return;
        }
      }
      break;
    case OP_EPOP:
      if (c0 == OP_EPOP) {
        s->iseq[s->pc-1] = MKOP_A(OP_EPOP, GETARG_A(i0)+GETARG_A(i));
        return;
      }
      break;
    case OP_POPERR:
      if (c0 == OP_POPERR) {
        s->iseq[s->pc-1] = MKOP_A(OP_POPERR, GETARG_A(i0)+GETARG_A(i));
        return;
      }
      break;
    case OP_RETURN:
      switch (c0) {
      case OP_RETURN:
	return;
      case OP_MOVE:
	s->iseq[s->pc-1] = MKOP_AB(OP_RETURN, GETARG_B(i0), OP_R_NORMAL);
	return;
      case OP_LOADI:
	s->iseq[s->pc-1] = MKOP_AsBx(OP_LOADI, 0, GETARG_sBx(i0));
	genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
	return;
      case OP_ARRAY:
      case OP_HASH:
      case OP_RANGE:
      case OP_AREF:
      case OP_GETUPVAR:
	s->iseq[s->pc-1] = MKOP_ABC(c0, 0, GETARG_B(i0), GETARG_C(i0));
	genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
	return;
      case OP_SETIV:
      case OP_SETCV:
      case OP_SETCONST:
      case OP_SETMCNST:
      case OP_SETUPVAR:
      case OP_SETGLOBAL:
	s->pc--;
	genop_peep(s, i0, NOVAL);
	i0 = s->iseq[s->pc-1];
	genop(s, MKOP_AB(OP_RETURN, GETARG_A(i0), OP_R_NORMAL));
	return;
      case OP_LOADSYM:
      case OP_GETGLOBAL:
      case OP_GETIV:
      case OP_GETCV:
      case OP_GETCONST:
      case OP_GETSPECIAL:
      case OP_LOADL:
      case OP_STRING:
	s->iseq[s->pc-1] = MKOP_ABx(c0, 0, GETARG_Bx(i0));
	genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
	return;
      case OP_SCLASS:
	s->iseq[s->pc-1] = MKOP_AB(c0, GETARG_A(i), GETARG_B(i0));
	genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
	return;
      case OP_LOADNIL:
      case OP_LOADSELF:
      case OP_LOADT:
      case OP_LOADF:
      case OP_OCLASS:
	s->iseq[s->pc-1] = MKOP_A(c0, 0);
	genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
	return;
      default:
	break;
      }
      break;
    case OP_ADD:
    case OP_SUB:
      if (c0 == OP_LOADI) {
	int c = GETARG_sBx(i0);
	
	if (c1 == OP_SUB) c = -c;
	if (c > 127 || c < -127) break;
	if (0 <= c) 
	  s->iseq[s->pc-1] = MKOP_ABC(OP_ADDI, GETARG_A(i), GETARG_B(i), c);
	else
	  s->iseq[s->pc-1] = MKOP_ABC(OP_SUBI, GETARG_A(i), GETARG_B(i), -c);
	return;
      }
    default:
      break;
    }
  }
  genop(s, i);
}

static void
scope_error(codegen_scope *s)
{
  exit(1);
}

static inline void
dispatch(codegen_scope *s, int pc)
{
  int diff = s->pc - pc;
  mrb_code i = s->iseq[pc];
  int c = GET_OPCODE(i);

  s->lastlabel = s->pc;
  switch (c) {
  case OP_JMP:
  case OP_JMPIF:
  case OP_JMPNOT:
  case OP_ONERR:
    break;
  default:
#ifdef ENABLE_STDIO
    fprintf(stderr, "bug: dispatch on non JMP op\n");
#endif
    scope_error(s);
  }
  s->iseq[pc] = MKOP_AsBx(c, GETARG_A(i), diff);
}

static void
dispatch_linked(codegen_scope *s, int pc)
{
  mrb_code i;
  int pos;

  if (!pc) return;
  for (;;) {
    i = s->iseq[pc];
    pos = GETARG_sBx(i);
    dispatch(s, pc);
    if (!pos) break;
    pc = pos;
  }
}

#define nregs_update do {if (s->sp > s->nregs) s->nregs = s->sp;} while (0)
static void
push_(codegen_scope *s)
{
  if (s->sp > 511) {
    codegen_error(s, "too complex expression");
  }
  s->sp++;
  nregs_update;
}

#define push() push_(s)
#define pop_(s) ((s)->sp--)
#define pop() pop_(s)
#define pop_n(n) (s->sp-=(n))
#define cursp() (s->sp)

static inline int
new_lit(codegen_scope *s, mrb_value val)
{
  int i;

  for (i=0; i<s->irep->plen; i++) {
    if (mrb_obj_equal(s->mrb, s->irep->pool[i], val)) return i;
  }
  if (s->irep->plen == s->pcapa) {
    s->pcapa *= 2;
    s->irep->pool = (mrb_value *)codegen_realloc(s, s->irep->pool, sizeof(mrb_value)*s->pcapa);
  }
  s->irep->pool[s->irep->plen] = val;
  i = s->irep->plen++;
  
  return i;
}

static inline int
new_msym(codegen_scope *s, mrb_sym sym)
{
  int i, len;

  len = s->irep->slen;
  if (len > 255) len = 255;
  for (i=0; i<len; i++) {
    if (s->irep->syms[i] == sym) return i;
    if (s->irep->syms[i] == 0) break;
  }
  if (i > 255) {
    codegen_error(s, "too many symbols (max 256)");
  }
  s->irep->syms[i] = sym;
  if (i == s->irep->slen) s->irep->slen++;
  return i;
}

static inline int
new_sym(codegen_scope *s, mrb_sym sym)
{
  int i;

  for (i=0; i<s->irep->slen; i++) {
    if (s->irep->syms[i] == sym) return i;
  }
  if (s->irep->slen > 125 && s->irep->slen < 256) {
    s->irep->syms = (mrb_sym *)codegen_realloc(s, s->irep->syms, sizeof(mrb_sym)*65536);
    for (i = 0; i < 256 - s->irep->slen; i++) {
      static const mrb_sym mrb_sym_zero = { 0 };
      s->irep->syms[i + s->irep->slen] = mrb_sym_zero;
    }
    s->irep->slen = 256;
  }
  s->irep->syms[s->irep->slen] = sym;
  return s->irep->slen++;
}

static int
node_len(node *tree)
{
  int n = 0;

  while (tree) {
    n++;
    tree = tree->cdr;
  }
  return n;
}

#define sym(x) ((mrb_sym)(intptr_t)(x))
#define lv_name(lv) sym((lv)->car)
static int
lv_idx(codegen_scope *s, mrb_sym id)
{
  node *lv = s->lv;
  int n = 1;

  while (lv) {
    if (lv_name(lv) == id) return n;
    n++;
    lv = lv->cdr;
  }
  return 0;
}

static void
for_body(codegen_scope *s, node *tree)
{
  codegen_scope *prev = s;
  int idx, base = s->idx;
  struct loopinfo *lp;
  node *n2;
  mrb_code c;

  // generate receiver
  codegen(s, tree->cdr->car, VAL);
  // generate loop-block
  s = scope_new(s->mrb, s, tree->car);
  idx = s->idx;

  lp = loop_push(s, LOOP_FOR);
  lp->pc1 = new_label(s);

  // generate loop variable
  n2 = tree->car;
  if (n2->car && !n2->car->cdr && !n2->cdr) {
    genop(s, MKOP_Ax(OP_ENTER, 1<<18));
    gen_assignment(s, n2->car->car, 1, NOVAL);
  }
  else {
    genop(s, MKOP_Ax(OP_ENTER, 1<<18));
    gen_vmassignment(s, n2, 1, VAL);
  }
  codegen(s, tree->cdr->cdr->car, VAL);
  pop();
  if (s->pc > 0) {
    c = s->iseq[s->pc-1];
    if (GET_OPCODE(c) != OP_RETURN || GETARG_B(c) != OP_R_NORMAL || s->pc == s->lastlabel)
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
  }
  loop_pop(s, NOVAL);
  scope_finish(s);
  s = prev;
  genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx - base, OP_L_BLOCK));
  pop();
  idx = new_msym(s, mrb_intern(s->mrb, "each"));
  genop(s, MKOP_ABC(OP_SENDB, cursp(), idx, 0));
}

static int
lambda_body(codegen_scope *s, node *tree, int blk)
{
  int idx, base = s->idx;
  mrb_code c;

  s = scope_new(s->mrb, s, tree->car);
  idx = s->idx;
  s->mscope = !blk;

  if (blk) {
    struct loopinfo *lp = loop_push(s, LOOP_BLOCK);
    lp->pc1 = new_label(s);
  }
  tree = tree->cdr;
  if (tree->car) {
    int ma, oa, ra, pa, ka, kd, ba, a;
    int pos, i;
    node *n, *opt;

    ma = node_len(tree->car->car);
    n = tree->car->car;
    while (n) {
      n = n->cdr;
    }
    oa = node_len(tree->car->cdr->car);
    ra = tree->car->cdr->cdr->car ? 1 : 0;
    pa = node_len(tree->car->cdr->cdr->cdr->car);
    ka = kd = 0;
    ba = tree->car->cdr->cdr->cdr->cdr ? 1 : 0;

    a = ((ma & 0x1f) << 18)
      | ((oa & 0x1f) << 13)
      | ((ra & 1) << 12)
      | ((pa & 0x1f) << 7)
      | ((ka & 0x1f) << 2)
      | ((kd & 1)<< 1)
      | (ba & 1);
    s->ainfo = (((ma+oa) & 0x3f) << 6) /* (12bits = 6:1:5) */
      | ((ra & 1) << 5)
      | (pa & 0x1f);
    genop(s, MKOP_Ax(OP_ENTER, a));
    pos = new_label(s);
    for (i=0; i<oa; i++) {
      new_label(s);
      genop(s, MKOP_sBx(OP_JMP, 0));
    }
    if (oa > 0) {
      genop(s, MKOP_sBx(OP_JMP, 0));
    }
    opt = tree->car->cdr->car;
    i = 0;
    while (opt) {
      int idx;

      dispatch(s, pos+i);
      codegen(s, opt->car->cdr, VAL);
      idx = lv_idx(s, (mrb_sym)(intptr_t)opt->car->car);
      pop();
      genop_peep(s, MKOP_AB(OP_MOVE, idx, cursp()), NOVAL);
      i++;
      opt = opt->cdr;
    }
    if (oa > 0) {
      dispatch(s, pos+i);
    }
  }
  codegen(s, tree->cdr->car, VAL);
  pop();
  if (s->pc > 0) {
    c = s->iseq[s->pc-1];
    if (GET_OPCODE(c) != OP_RETURN || GETARG_B(c) != OP_R_NORMAL || s->pc == s->lastlabel) {
      if (s->nregs == 0) {
        genop(s, MKOP_A(OP_LOADNIL, 0));
        genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
      }
      else {
        genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
      }
    }
  }
  if (blk) {
    loop_pop(s, NOVAL);
  }
  scope_finish(s);

  return idx - base;
}

static int
scope_body(codegen_scope *s, node *tree)
{
  codegen_scope *scope = scope_new(s->mrb, s, tree->car);
  int idx = scope->idx;

  codegen(scope, tree->cdr, VAL);
  if (!s->iseq) {
    genop(scope, MKOP_A(OP_STOP, 0));
  }
  else {
    if (scope->nregs == 0) {
      genop(scope, MKOP_A(OP_LOADNIL, 0));
      genop(scope, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
    }
    else {
      genop_peep(scope, MKOP_AB(OP_RETURN, scope->sp, OP_R_NORMAL), NOVAL);
    }
  }
  scope_finish(scope);

  return idx - s->idx;
}

static int
nosplat(node *t)
{
  while (t) {
    if ((intptr_t)t->car->car == NODE_SPLAT) return FALSE;
    t = t->cdr;
  }
  return TRUE;
}

static mrb_sym
attrsym(codegen_scope *s, mrb_sym a)
{
  const char *name;
  int len;
  char *name2;

  name = mrb_sym2name_len(s->mrb, a, &len);
  name2 = (char *)codegen_palloc(s, len+1);
  memcpy(name2, name, len);
  name2[len] = '=';
  name2[len+1] = '\0';

  return mrb_intern2(s->mrb, name2, len+1);
}

static int
gen_values(codegen_scope *s, node *t)
{
  int n = 0;

  while (t) {
    if ((intptr_t)t->car->car == NODE_SPLAT) { // splat mode
      pop_n(n);
      genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), n));
      push();
      codegen(s, t->car, VAL);
      pop(); pop();
      genop(s, MKOP_AB(OP_ARYCAT, cursp(), cursp()+1));
      t = t->cdr;
      while (t) {
        push();
        codegen(s, t->car, VAL);
        pop(); pop();
        if ((intptr_t)t->car->car == NODE_SPLAT) {
          genop(s, MKOP_AB(OP_ARYCAT, cursp(), cursp()+1));
        }
        else {
          genop(s, MKOP_AB(OP_ARYPUSH, cursp(), cursp()+1));
        }
        t = t->cdr;
      }
      return -1;
    }
    // normal (no splat) mode
    codegen(s, t->car, VAL);
    n++;
    t = t->cdr;
  }
  return n;
}

#define CALL_MAXARGS 127

static void
gen_call(codegen_scope *s, node *tree, mrb_sym name, int sp, int val)
{
  mrb_sym sym = name ? name : sym(tree->cdr->car);
  int idx;
  int n = 0, noop = 0, sendv = 0, blk = 0;

  codegen(s, tree->car, VAL); /* receiver */
  idx = new_msym(s, sym);
  tree = tree->cdr->cdr->car;
  if (tree) {
    n = gen_values(s, tree->car);
    if (n < 0) {
      n = noop = sendv = 1;
      push();
    }
  }
  if (sp) {
    if (sendv) {
      pop();
      genop(s, MKOP_AB(OP_ARYPUSH, cursp(), sp));
      push();
    }
    else {
      genop(s, MKOP_AB(OP_MOVE, cursp(), sp));
      push();
      n++;
    }
  }
  if (tree && tree->cdr) {
    noop = 1;
    codegen(s, tree->cdr, VAL);
    pop();
  }
  else {
    blk = cursp();
  }
  pop_n(n+1);
  {
    int len;
    const char *name = mrb_sym2name_len(s->mrb, sym, &len);

    if (!noop && len == 1 && name[0] == '+')  {
      genop_peep(s, MKOP_ABC(OP_ADD, cursp(), idx, n), val);
    }
    else if (!noop && len == 1 && name[0] == '-')  {
      genop_peep(s, MKOP_ABC(OP_SUB, cursp(), idx, n), val);
    }
    else if (!noop && len == 1 && name[0] == '*')  {
      genop(s, MKOP_ABC(OP_MUL, cursp(), idx, n));
    }
    else if (!noop && len == 1 && name[0] == '/')  {
      genop(s, MKOP_ABC(OP_DIV, cursp(), idx, n));
    }
    else if (!noop && len == 1 && name[0] == '<')  {
      genop(s, MKOP_ABC(OP_LT, cursp(), idx, n));
    }
    else if (!noop && len == 2 && name[0] == '<' && name[1] == '=')  {
      genop(s, MKOP_ABC(OP_LE, cursp(), idx, n));
    }
    else if (!noop && len == 1 && name[0] == '>')  {
      genop(s, MKOP_ABC(OP_GT, cursp(), idx, n));
    }
    else if (!noop && len == 2 && name[0] == '>' && name[1] == '=')  {
      genop(s, MKOP_ABC(OP_GE, cursp(), idx, n));
    }
    else if (!noop && len == 2 && name[0] == '=' && name[1] == '=')  {
      genop(s, MKOP_ABC(OP_EQ, cursp(), idx, n));
    }
    else {
      if (sendv) n = CALL_MAXARGS;
      if (blk > 0) {		   /* no block */
	genop(s, MKOP_ABC(OP_SEND, cursp(), idx, n));
      }
      else {
	genop(s, MKOP_ABC(OP_SENDB, cursp(), idx, n));
      }
    }
  }
  if (val) {
    push();
  }
}

static void
gen_assignment(codegen_scope *s, node *node, int sp, int val)
{
  int idx;
  int type = (intptr_t)node->car;

  node = node->cdr;
  switch ((intptr_t)type) {
  case NODE_GVAR:
    idx = new_sym(s, sym(node));
    genop_peep(s, MKOP_ABx(OP_SETGLOBAL, sp, idx), val);
    break;
  case NODE_LVAR:
    idx = lv_idx(s, sym(node));
    if (idx > 0) {
      if (idx != sp) {
        genop_peep(s, MKOP_AB(OP_MOVE, idx, sp), val);
      }
      break;
    }
    else {                      /* upvar */
      int lv = 0;
      codegen_scope *up = s->prev;

      while (up) {
        idx = lv_idx(up, sym(node));
        if (idx > 0) {
          genop_peep(s, MKOP_ABC(OP_SETUPVAR, sp, idx, lv), val);
          break;
        }
        lv++;
        up = up->prev;
      }
      //      assert(up!=0);
    }
    break;
  case NODE_IVAR:
    idx = new_sym(s, sym(node));
    genop_peep(s, MKOP_ABx(OP_SETIV, sp, idx), val);
    break;
  case NODE_CVAR:
    idx = new_sym(s, sym(node));
    genop_peep(s, MKOP_ABx(OP_SETCV, sp, idx), val);
    break;
  case NODE_CONST:
    idx = new_sym(s, sym(node));
    genop_peep(s, MKOP_ABx(OP_SETCONST, sp, idx), val);
    break;
  case NODE_COLON2:
    idx = new_sym(s, sym(node->cdr));
    genop_peep(s, MKOP_AB(OP_MOVE, cursp(), sp), NOVAL);
    push();
    codegen(s, node->car, VAL);
    pop_n(2);
    genop_peep(s, MKOP_ABx(OP_SETMCNST, cursp(), idx), val);
    break;

  case NODE_CALL:
    push();
    gen_call(s, node, attrsym(s, sym(node->cdr->car)), sp, NOVAL);
    pop();
    if (val) {
      genop_peep(s, MKOP_AB(OP_MOVE, cursp(), sp), val);
    }
    break;

  default:
#ifdef ENABLE_STDIO
    printf("unknown lhs %d\n", type);
#endif
    break;
  }
  if (val) push();
}

static void
gen_vmassignment(codegen_scope *s, node *tree, int rhs, int val)
{
  int n = 0, post = 0;
  node *t, *p;

  if (tree->car) {              /* pre */
    t = tree->car;
    n = 0;
    while (t) {
      genop(s, MKOP_ABC(OP_AREF, cursp(), rhs, n));
      gen_assignment(s, t->car, cursp(), NOVAL);
      n++;
      t = t->cdr;
    }
  }
  t = tree->cdr;
  if (t) {
    if (t->cdr) {               /* post count */
      p = t->cdr->car;
      while (p) {
        post++;
        p = p->cdr;
      }
    }
    if (val) {
      genop(s, MKOP_AB(OP_MOVE, cursp(), rhs));
      push();
    }
    pop();
    genop(s, MKOP_ABC(OP_APOST, cursp(), n, post));
    n = 1;
    if (t->car) {               /* rest */
      gen_assignment(s, t->car, cursp(), NOVAL);
    }
    if (t->cdr && t->cdr->car) {
      t = t->cdr->car;
      while (t) {
        gen_assignment(s, t->car, cursp()+n, NOVAL);
        t = t->cdr;
        n++;
      }
    }
  }
}

static void
raise_error(codegen_scope *s, const char *msg)
{
  int idx = new_lit(s, mrb_str_new_cstr(s->mrb, msg));

  genop(s, MKOP_ABx(OP_ERR, 1, idx));
}

static double
readint_float(codegen_scope *s, const char *p, int base)
{
  const char *e = p + strlen(p);
  double f = 0;
  int n;

  if (*p == '+') p++;
  while (p < e) {
    char c = *p;
    c = tolower((unsigned char)c);
    for (n=0; n<base; n++) {
      if (mrb_digitmap[n] == c) {
	f *= base;
	f += n;
	break;
      }
    }
    if (n == base) {
      codegen_error(s, "malformed readint input");
    }
    p++;
  }
  return f;
}

static void
codegen(codegen_scope *s, node *tree, int val)
{
  int nt;

  if (!tree) return;
  nt = (intptr_t)tree->car;
  s->lineno = tree->lineno;
  tree = tree->cdr;
  switch (nt) {
  case NODE_BEGIN:
    if (val && !tree) {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      push();
    }
    while (tree) {
      codegen(s, tree->car, tree->cdr ? NOVAL : val);
      tree = tree->cdr;
    }
    break;

  case NODE_RESCUE:
    {
      int onerr, noexc, exend, pos1, pos2, tmp;
      struct loopinfo *lp;

      onerr = new_label(s);
      genop(s, MKOP_Bx(OP_ONERR, 0));
      lp = loop_push(s, LOOP_BEGIN);
      lp->pc1 = onerr;
      if (tree->car) {
        codegen(s, tree->car, val);
	if (val) pop();
      }
      lp->type = LOOP_RESCUE;
      noexc = new_label(s);
      genop(s, MKOP_Bx(OP_JMP, 0));
      dispatch(s, onerr);
      tree = tree->cdr;
      exend = 0;
      pos1 = 0;
      if (tree->car) {
        node *n2 = tree->car;
        int exc = cursp();

        genop(s, MKOP_A(OP_RESCUE, exc));
        push();
        while (n2) {
          node *n3 = n2->car;
          node *n4 = n3->car;

          if (pos1) dispatch(s, pos1);
          pos2 = 0;
          do {
            if (n4) {
              codegen(s, n4->car, VAL);
            }
            else {
              genop(s, MKOP_ABx(OP_GETCONST, cursp(), new_msym(s, mrb_intern(s->mrb, "StandardError"))));
              push();
            }
            genop(s, MKOP_AB(OP_MOVE, cursp(), exc));
            pop();
            genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern(s->mrb, "===")), 1));
            tmp = new_label(s);
            genop(s, MKOP_AsBx(OP_JMPIF, cursp(), pos2));
            pos2 = tmp;
            if (n4) {
              n4 = n4->cdr;
            }
          } while (n4);
          pos1 = new_label(s);
          genop(s, MKOP_sBx(OP_JMP, 0));
          dispatch_linked(s, pos2);

          pop();
          if (n3->cdr->car) {
            gen_assignment(s, n3->cdr->car, exc, NOVAL);
          }
          if (n3->cdr->cdr->car) {
            codegen(s, n3->cdr->cdr->car, val);
          }
          tmp = new_label(s);
          genop(s, MKOP_sBx(OP_JMP, exend));
          exend = tmp;
          n2 = n2->cdr;
          push();
        }
        if (pos1) {
          dispatch(s, pos1);
          genop(s, MKOP_A(OP_RAISE, exc));
        }
      }
      pop();
      tree = tree->cdr;
      dispatch(s, noexc);
      genop(s, MKOP_A(OP_POPERR, 1));
      if (tree->car) {
        codegen(s, tree->car, val);
      }
      dispatch_linked(s, exend);
      loop_pop(s, NOVAL);
    }
    break;

  case NODE_ENSURE:
    {
      int idx;
      int epush = s->pc;

      genop(s, MKOP_Bx(OP_EPUSH, 0));
      s->ensure_level++;
      codegen(s, tree->car, val);
      idx = scope_body(s, tree->cdr);
      s->iseq[epush] = MKOP_Bx(OP_EPUSH, idx);
      s->ensure_level--;
      genop_peep(s, MKOP_A(OP_EPOP, 1), NOVAL);
    }
    break;

  case NODE_LAMBDA:
    {
      int idx = lambda_body(s, tree, 1);

      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_LAMBDA));
      push();
    }
    break;

  case NODE_BLOCK:
    {
      int idx = lambda_body(s, tree, 1);

      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_BLOCK));
      push();
    }
    break;

  case NODE_IF:
    {
      int pos1, pos2;
      node *e = tree->cdr->cdr->car;

      codegen(s, tree->car, VAL);
      pop();
      pos1 = new_label(s);
      genop(s, MKOP_AsBx(OP_JMPNOT, cursp(), 0));

      codegen(s, tree->cdr->car, val);
      if (e) {
        if (val) pop();
        pos2 = new_label(s);
        genop(s, MKOP_sBx(OP_JMP, 0));
        dispatch(s, pos1);
        codegen(s, e, val);
        dispatch(s, pos2);
      }
      else {
        if (val) {
          pop();
          genop(s, MKOP_A(OP_LOADNIL, cursp()));
          push();
        }
        dispatch(s, pos1);
      }
    }
    break;

  case NODE_AND:
    {
      int pos;

      codegen(s, tree->car, VAL);
      pos = new_label(s);
      pop();
      genop(s, MKOP_AsBx(OP_JMPNOT, cursp(), 0));
      codegen(s, tree->cdr, val);
      dispatch(s, pos);
    }
    break;

  case NODE_OR:
    {
      int pos;

      codegen(s, tree->car, VAL);
      pos = new_label(s);
      pop();
      genop(s, MKOP_AsBx(OP_JMPIF, cursp(), 0));
      codegen(s, tree->cdr, val);
      dispatch(s, pos);
    }
    break;

  case NODE_WHILE:
    {
      struct loopinfo *lp = loop_push(s, LOOP_NORMAL);

      lp->pc1 = new_label(s);
      genop(s, MKOP_sBx(OP_JMP, 0));
      lp->pc2 = new_label(s);
      codegen(s, tree->cdr, NOVAL);
      dispatch(s, lp->pc1);
      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AsBx(OP_JMPIF, cursp(), lp->pc2 - s->pc));

      loop_pop(s, val);
    }
    break;

  case NODE_UNTIL:
    {
      struct loopinfo *lp = loop_push(s, LOOP_NORMAL);

      lp->pc1 = new_label(s);
      genop(s, MKOP_sBx(OP_JMP, 0));
      lp->pc2 = new_label(s);
      codegen(s, tree->cdr, NOVAL);
      dispatch(s, lp->pc1);
      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AsBx(OP_JMPNOT, cursp(), lp->pc2 - s->pc));

      loop_pop(s, val);
    }
    break;

  case NODE_FOR:
    for_body(s, tree);
    if (val) push();
    break;

  case NODE_CASE:
    {
      int head = 0;
      int pos1, pos2, pos3, tmp;
      node *n;

      pos3 = 0;
      if (tree->car) {
        head = cursp();
        codegen(s, tree->car, VAL);
      }
      tree = tree->cdr;
      while (tree) {
        n = tree->car->car;
        pos1 = pos2 = 0;
        while (n) {
          codegen(s, n->car, VAL);
          if (head) {
            genop(s, MKOP_AB(OP_MOVE, cursp(), head));
            pop();
            genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern(s->mrb, "===")), 1));
          }
          tmp = new_label(s);
          genop(s, MKOP_AsBx(OP_JMPIF, cursp(), pos2));
          pos2 = tmp;
          n = n->cdr;
        }
        if (tree->car->car) {
          pos1 = new_label(s);
          genop(s, MKOP_sBx(OP_JMP, 0));
          dispatch_linked(s, pos2);
        }
	pop();			/* pop HEAD */
        codegen(s, tree->car->cdr, val);
	if (val) pop();
        tmp = new_label(s);
        genop(s, MKOP_sBx(OP_JMP, pos3));
        pos3 = tmp;
        if (pos1) dispatch(s, pos1);
        tree = tree->cdr;
	push();			/* push HEAD */
      }
      pop();			/* pop HEAD */
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      if (val) push();
      if (pos3) dispatch_linked(s, pos3);
    }
    break;

  case NODE_SCOPE:
    scope_body(s, tree);
    break;

  case NODE_FCALL:
  case NODE_CALL:
    gen_call(s, tree, 0, 0, val);
    break;

  case NODE_DOT2:
    codegen(s, tree->car, VAL);
    codegen(s, tree->cdr, VAL);
    pop(); pop();
    if (val) {
      genop(s, MKOP_ABC(OP_RANGE, cursp(), cursp(), 0));
      push();
    }
    break;

  case NODE_DOT3:
    codegen(s, tree->car, VAL);
    codegen(s, tree->cdr, VAL);
    pop(); pop();
    if (val) {
      genop(s, MKOP_ABC(OP_RANGE, cursp(), cursp(), 1));
      push();
    }
    break;

  case NODE_COLON2:
    {
      int sym = new_sym(s, sym(tree->cdr));

      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
    }
    break;

  case NODE_COLON3:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
    }
    break;

  case NODE_ARRAY:
    {
      int n;

      n = gen_values(s, tree);
      if (n >= 0) {
        pop_n(n);
        if (val) {
          genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), n));
          push();
        }
      }
      else if (val) {
        push();
      }
    }
    break;

  case NODE_HASH:
    {
      int len = 0;

      while (tree) {
        codegen(s, tree->car->car, VAL);
        codegen(s, tree->car->cdr, VAL);
        len++;
        tree = tree->cdr;
      }
      pop_n(len*2);
      if (val) {
        genop(s, MKOP_ABC(OP_HASH, cursp(), cursp(), len));
        push();
      }
    }
    break;

  case NODE_SPLAT:
    codegen(s, tree, VAL);
    break;

  case NODE_ASGN:
    codegen(s, tree->cdr, VAL);
    pop();
    gen_assignment(s, tree->car, cursp(), val);
    break;

  case NODE_MASGN:
    {
      int len = 0, n = 0, post = 0;
      node *t = tree->cdr, *p;
      int rhs = cursp();

      if ((intptr_t)t->car == NODE_ARRAY && nosplat(t->cdr)) {
        // fixed rhs
        t = t->cdr;
        while (t) {
          codegen(s, t->car, VAL);
          len++;
          t = t->cdr;
        }
        tree = tree->car;
        if (tree->car) {                /* pre */
          t = tree->car;
          n = 0;
          while (t) {
            gen_assignment(s, t->car, rhs+n, NOVAL);
            n++;
            t = t->cdr;
          }
        }
        t = tree->cdr;
        if (t) {
          if (t->cdr) {         /* post count */
            p = t->cdr->car;
            while (p) {
              post++;
              p = p->cdr;
            }
          }
          if (t->car) {         /* rest (len - pre - post) */
            int rn = len - post - n;

            genop(s, MKOP_ABC(OP_ARRAY, cursp(), rhs+n, rn));
            gen_assignment(s, t->car, cursp(), NOVAL);
            n += rn;
          }
          if (t->cdr && t->cdr->car) {
            t = t->cdr->car;
            while (n<len) {
              gen_assignment(s, t->car, rhs+n, NOVAL);
              t = t->cdr;
              n++;
            }
          }
        }
        pop_n(len);
        if (val) {
          genop(s, MKOP_ABC(OP_ARRAY, rhs, rhs, len));
          push();
        }
      }
      else {
        // variable rhs
        codegen(s, t, VAL);
        gen_vmassignment(s, tree->car, rhs, val);
        if (!val) pop();
      }
    }
    break;

  case NODE_OP_ASGN:
    {
      mrb_sym sym = sym(tree->cdr->car);
      int len;
      const char *name = mrb_sym2name_len(s->mrb, sym, &len);
      int idx;

      codegen(s, tree->car, VAL);
      if (len == 2 &&
	  ((name[0] == '|' && name[1] == '|') ||
	   (name[0] == '&' && name[1] == '&'))) {
	int pos;

	pop();
	pos = new_label(s);
	genop(s, MKOP_AsBx(name[0] == '|' ? OP_JMPIF : OP_JMPNOT, cursp(), 0));
	codegen(s, tree->cdr->cdr->car, VAL);
	pop();
	gen_assignment(s, tree->car, cursp(), val);
	dispatch(s, pos);
	break;
      }
      codegen(s, tree->cdr->cdr->car, VAL);
      pop(); pop();

      idx = new_msym(s, sym);
      if (len == 1 && name[0] == '+')  {
        genop_peep(s, MKOP_ABC(OP_ADD, cursp(), idx, 1), val);
      }
      else if (len == 1 && name[0] == '-')  {
        genop_peep(s, MKOP_ABC(OP_SUB, cursp(), idx, 1), val);
      }
      else if (len == 1 && name[0] == '<')  {
        genop(s, MKOP_ABC(OP_LT, cursp(), idx, 1));
      }
      else if (len == 2 && name[0] == '<' && name[1] == '=')  {
        genop(s, MKOP_ABC(OP_LE, cursp(), idx, 1));
      }
      else if (len == 1 && name[0] == '>')  {
        genop(s, MKOP_ABC(OP_GT, cursp(), idx, 1));
      }
      else if (len == 2 && name[0] == '>' && name[1] == '=')  {
        genop(s, MKOP_ABC(OP_GE, cursp(), idx, 1));
      }
      else {
	genop(s, MKOP_ABC(OP_SEND, cursp(), idx, 1));
      }
    }
    gen_assignment(s, tree->car, cursp(), val);
    break;

  case NODE_SUPER:
    {
      int n = 0, noop = 0, sendv = 0;

      push();			/* room for receiver */
      if (tree) {
        node *args = tree->car;
	if (args) {
	  n = gen_values(s, args);
	  if (n < 0) {
	    n = noop = sendv = 1;
	    push();
	  }
	}
      }
      if (tree && tree->cdr) {
        codegen(s, tree->cdr, VAL);
        pop();
      }
      else {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
      }
      pop_n(n+1);
      if (sendv) n = CALL_MAXARGS;
      genop(s, MKOP_ABC(OP_SUPER, cursp(), 0, n));
      if (val) push();
    }
    break;

  case NODE_ZSUPER:
    {
      codegen_scope *s2 = s;
      int lv = 0, ainfo = 0;

      push(); 			/* room for receiver */
      while (!s2->mscope) {
        lv++;
        s2 = s2->prev;
        if (!s2) break;
      }
      if (s2) ainfo = s2->ainfo;
      genop(s, MKOP_ABx(OP_ARGARY, cursp(), (ainfo<<4)|(lv & 0xf)));
      if (tree && tree->cdr) {
	push();
        codegen(s, tree->cdr, VAL);
        pop_n(2);
      }
      pop();
      genop(s, MKOP_ABC(OP_SUPER, cursp(), 0, CALL_MAXARGS));
      if (val) push();
    }
    break;

  case NODE_RETURN:
    codegen(s, tree, VAL);
    pop();
    if (s->loop) {
      genop(s, MKOP_AB(OP_RETURN, cursp(), OP_R_RETURN));
    }
    else {
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
    }
    push();
    break;

  case NODE_YIELD:
    {
      codegen_scope *s2 = s;
      int lv = 0, ainfo = 0;
      int n = 0, sendv = 0;

      while (!s2->mscope) {
        lv++;
        s2 = s2->prev;
        if (!s2) break;
      }
      if (s2) ainfo = s2->ainfo;
      genop(s, MKOP_ABx(OP_BLKPUSH, cursp(), (ainfo<<4)|(lv & 0xf)));
      push();
      if (tree) {
        n = gen_values(s, tree);
        if (n < 0) {
          n = sendv = 1;
          push();
        }
      }
      pop_n(n+1);
      if (sendv) n = CALL_MAXARGS;
      genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern(s->mrb, "call")), n));
      if (val) push();
    }
    break;

  case NODE_BREAK:
    loop_break(s, tree);
    if (val) push();
    break;

  case NODE_NEXT:
    if (!s->loop) {
      raise_error(s, "unexpected next");
    }
    else if (s->loop->type == LOOP_NORMAL) {
      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      codegen(s, tree, NOVAL);
      genop(s, MKOP_sBx(OP_JMP, s->loop->pc1 - s->pc));
    }
    else {
      if (tree) {
        codegen(s, tree, VAL);
        pop();
      }
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
    }
    if (val) push();
    break;

  case NODE_REDO:
    if (!s->loop) {
      raise_error(s, "unexpected redo");
    }
    else {
      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      genop(s, MKOP_sBx(OP_JMP, s->loop->pc2 - s->pc));
    }
    break;

  case NODE_RETRY:
    {
      const char *msg = "unexpected retry";

      if (!s->loop) {
        raise_error(s, msg);
      }
      else {
        struct loopinfo *lp = s->loop;
        int n = 0;

        while (lp && lp->type != LOOP_RESCUE) {
          if (lp->type == LOOP_BEGIN) {
            n++;
          }
          lp = lp->prev;
        }
        if (!lp) {
          raise_error(s, msg);
        }
        else {
          if (n > 0) {
            while (n--) {
              genop_peep(s, MKOP_A(OP_POPERR, 1), NOVAL);
            }
          }
          if (s->ensure_level > lp->ensure_level) {
            genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - lp->ensure_level), NOVAL);
          }
          genop(s, MKOP_sBx(OP_JMP, lp->pc1 - s->pc));
        }
      }
    }
    break;

  case NODE_LVAR:
    if (val) {
      int idx = lv_idx(s, sym(tree));

      if (idx > 0) {
        genop(s, MKOP_AB(OP_MOVE, cursp(), idx));
      }
      else {
        int lv = 0;
        codegen_scope *up = s->prev;

        while (up) {
          idx = lv_idx(up, sym(tree));
          if (idx > 0) {
            genop(s, MKOP_ABC(OP_GETUPVAR, cursp(), idx, lv));
            break;
          }
          lv++;
          up = up->prev;
        }
      }
      push();
    }
    break;

  case NODE_GVAR:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_IVAR:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETIV, cursp(), sym));
      push();
    }
    break;

  case NODE_CVAR:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETCV, cursp(), sym));
      push();
    }
    break;

  case NODE_CONST:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETCONST, cursp(), sym));
      push();
    }
    break;

  case NODE_DEFINED:
    codegen(s, tree, VAL);
    break;

  case NODE_BACK_REF:
    {
      char buf[4];
      int len;
      int sym;

      len = snprintf(buf, sizeof(buf), "$%c", (int)(intptr_t)tree);
      sym = new_sym(s, mrb_intern2(s->mrb, buf, len));
      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_NTH_REF:
    {
      char buf[4];
      int len;
      int sym;

      len = snprintf(buf, sizeof(buf), "$%d", (int)(intptr_t)tree);
      sym = new_sym(s, mrb_intern2(s->mrb, buf, len));
      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_ARG:
    // should not happen
    break;

  case NODE_BLOCK_ARG:
    codegen(s, tree, VAL);
    break;

  case NODE_INT:
    if (val) {
      char *p = (char*)tree->car;
      int base = (intptr_t)tree->cdr->car;
      double f;
      mrb_int i;
      mrb_code co;

      f = readint_float(s, p, base);
      if (!FIXABLE(f)) {
	int off = new_lit(s, mrb_float_value(f));

	genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
      }
      else {
	i = (mrb_int)f;
	if (i < MAXARG_sBx && i > -MAXARG_sBx) {
	  co = MKOP_AsBx(OP_LOADI, cursp(), i);
	}
	else {
	  int off = new_lit(s, mrb_fixnum_value(i));
	  co = MKOP_ABx(OP_LOADL, cursp(), off);
	}
	genop(s, co);
      }
      push();
    }
    break;

  case NODE_FLOAT:
    if (val) {
      char *p = (char*)tree;
      mrb_float f = str_to_mrb_float(p);
      int off = new_lit(s, mrb_float_value(f));

      genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
      push();
    }
    break;

  case NODE_NEGATE:
    {
      nt = (intptr_t)tree->car;
      tree = tree->cdr;
      switch (nt) {
      case NODE_FLOAT:
        {
          char *p = (char*)tree;
          mrb_float f = str_to_mrb_float(p);
          int off = new_lit(s, mrb_float_value(-f));

          genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
          push();
        }
        break;

      case NODE_INT:
        {
          char *p = (char*)tree->car;
          int base = (intptr_t)tree->cdr->car;
	  mrb_float f;
          mrb_int i;
          mrb_code co;

	  f = readint_float(s, p, base);
	  if (!FIXABLE(f)) {
	    int off = new_lit(s, mrb_float_value(-f));
	    
	    genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
	  }
	  else {
	    i = (mrb_int)-f;
	    if (i < MAXARG_sBx && i > -MAXARG_sBx) {
	      co = MKOP_AsBx(OP_LOADI, cursp(), i);
	    }
	    else {
	      int off = new_lit(s, mrb_fixnum_value(i));
	      co = MKOP_ABx(OP_LOADL, cursp(), off);
	    }
	    genop(s, co);
	  }
          push();
        }
        break;

      default:
        {
          int sym = new_msym(s, mrb_intern(s->mrb, "-"));

          genop(s, MKOP_ABx(OP_LOADI, cursp(), 0));
          push();
          codegen(s, tree, VAL);
          pop(); pop();
          genop(s, MKOP_ABC(OP_SUB, cursp(), sym, 2));
        }
        break;
      }
    }
    break;

  case NODE_STR:
    if (val) {
      char *p = (char*)tree->car;
      size_t len = (intptr_t)tree->cdr;
      int ai = mrb_gc_arena_save(s->mrb);
      int off = new_lit(s, mrb_str_new(s->mrb, p, len));

      mrb_gc_arena_restore(s->mrb, ai);
      genop(s, MKOP_ABx(OP_STRING, cursp(), off));
      push();
    }
    break;

  case NODE_DSTR:
    if (val) {
      node *n = tree;

      codegen(s, n->car, VAL);
      n = n->cdr;
      while (n) {
        codegen(s, n->car, VAL);
        pop(); pop();
        genop(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1));
        push();
        n = n->cdr;
      }
    }
    else {
      node *n = tree;

      while (n) {
        if ((intptr_t)n->car->car != NODE_STR) {
          codegen(s, n->car, NOVAL);
        }
        n = n->cdr;
      }
    }
    break;

  case NODE_SYM:
    if (val) {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), sym));
      push();
    }
    break;

  case NODE_DSYM:
    codegen(s, tree, val);
    if (val) {
      pop();
      genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern(s->mrb, "intern")), 0));
      push();
    }
    break;

  case NODE_SELF:
    if (val) {
      genop(s, MKOP_A(OP_LOADSELF, cursp()));
      push();
    }
    break;

  case NODE_NIL:
    if (val) {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      push();
    }
    break;

  case NODE_TRUE:
    if (val) {
      genop(s, MKOP_A(OP_LOADT, cursp()));
      push();
    }
    break;

  case NODE_FALSE:
    if (val) {
      genop(s, MKOP_A(OP_LOADF, cursp()));
      push();
    }
    break;

  case NODE_ALIAS:
    {
      int a = new_msym(s, sym(tree->car));
      int b = new_msym(s, sym(tree->cdr));
      int c = new_msym(s, mrb_intern(s->mrb, "alias_method"));

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), a));
      push();
      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), b));
      push();
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      pop_n(3);
      genop(s, MKOP_ABC(OP_SEND, cursp(), c, 2));
      if (val) {
        push();
      }
    }
   break;

  case NODE_UNDEF:
    {
      int sym = new_msym(s, sym(tree));
      int undef = new_msym(s, mrb_intern(s->mrb, "undef_method"));

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), sym));
      push();
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      pop_n(2);
      genop(s, MKOP_ABC(OP_SEND, cursp(), undef, 2));
      if (val) {
        push();
      }
    }
    break;

  case NODE_CLASS:
    {
      int idx;

      if (tree->car->car == (node*)0) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      else if (tree->car->car == (node*)1) {
        genop(s, MKOP_A(OP_OCLASS, cursp()));
        push();
      }
      else {
        codegen(s, tree->car->car, VAL);
      }
      if (tree->cdr->car) {
        codegen(s, tree->cdr->car, VAL);
      }
      else {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      pop(); pop();
      idx = new_msym(s, sym(tree->car->cdr));
      genop(s, MKOP_AB(OP_CLASS, cursp(), idx));
      idx = scope_body(s, tree->cdr->cdr->car);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_MODULE:
    {
      int idx;

      if (tree->car->car == (node*)0) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      else if (tree->car->car == (node*)1) {
        genop(s, MKOP_A(OP_OCLASS, cursp()));
        push();
      }
      else {
        codegen(s, tree->car->car, VAL);
      }
      pop();
      idx = new_msym(s, sym(tree->car->cdr));
      genop(s, MKOP_AB(OP_MODULE, cursp(), idx));
      idx = scope_body(s, tree->cdr->car);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_SCLASS:
    {
      int idx;

      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AB(OP_SCLASS, cursp(), cursp()));
      idx = scope_body(s, tree->cdr->car);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_DEF:
    {
      int sym = new_msym(s, sym(tree->car));
      int idx = lambda_body(s, tree->cdr, 0);

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_METHOD));
      pop();
      genop(s, MKOP_AB(OP_METHOD, cursp(), sym));
      if (val) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
      }
    }
    break;

  case NODE_SDEF:
    {
      node *recv = tree->car;
      int sym = new_msym(s, sym(tree->cdr->car));
      int idx = lambda_body(s, tree->cdr->cdr, 0);

      codegen(s, recv, VAL);
      pop();
      genop(s, MKOP_AB(OP_SCLASS, cursp(), cursp()));
      push();
      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_METHOD));
      pop();
      genop(s, MKOP_AB(OP_METHOD, cursp(), sym));
      if (val) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
      }
    }
    break;

  case NODE_POSTEXE:
    codegen(s, tree, NOVAL);
    break;

  default:
    break;
  }
}

static codegen_scope*
scope_new(mrb_state *mrb, codegen_scope *prev, node *lv)
{
  static const codegen_scope codegen_scope_zero = { 0 };
  mrb_pool *pool = mrb_pool_open(mrb);
  codegen_scope *p = (codegen_scope *)mrb_pool_alloc(pool, sizeof(codegen_scope));

  if (!p) return 0;
  *p = codegen_scope_zero;
  p->mrb = mrb;
  p->mpool = pool;
  if (!prev) return p;
  p->prev = prev;
  p->ainfo = -1;
  p->mscope = 0;

  p->irep = mrb_add_irep(mrb);
  p->idx = p->irep->idx;

  p->icapa = 1024;
  p->iseq = (mrb_code*)mrb_malloc(mrb, sizeof(mrb_code)*p->icapa);

  p->pcapa = 32;
  p->irep->pool = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value)*p->pcapa);
  p->irep->plen = 0;

  p->scapa = 256;
  p->irep->syms = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym)*256);
  p->irep->slen = 0;

  p->lv = lv;
  p->sp += node_len(lv)+1;	/* add self */
  p->nlocals = p->sp;
  p->ai = mrb_gc_arena_save(mrb);

  p->filename = prev->filename;
  if (p->filename) {
    p->lines = (short*)mrb_malloc(mrb, sizeof(short)*p->icapa);
  }
  p->lineno = prev->lineno;
  return p;
}

static void
scope_finish(codegen_scope *s)
{
  mrb_state *mrb = s->mrb;
  mrb_irep *irep = s->irep;
    
  irep->flags = 0;
  if (s->iseq) {
    irep->iseq = (mrb_code *)codegen_realloc(s, s->iseq, sizeof(mrb_code)*s->pc);
    irep->ilen = s->pc;
    if (s->lines) {
      irep->lines = (short *)codegen_realloc(s, s->lines, sizeof(short)*s->pc);
    }
    else {
      irep->lines = 0;
    }
  }
  irep->pool = (mrb_value *)codegen_realloc(s, irep->pool, sizeof(mrb_value)*irep->plen);
  irep->syms = (mrb_sym *)codegen_realloc(s, irep->syms, sizeof(mrb_sym)*irep->slen);
  if (s->filename) {
    irep->filename = s->filename;
  }

  irep->nlocals = s->nlocals;
  irep->nregs = s->nregs;

  mrb_gc_arena_restore(mrb, s->ai);
  mrb_pool_close(s->mpool);
}

static struct loopinfo*
loop_push(codegen_scope *s, enum looptype t)
{
  struct loopinfo *p = (struct loopinfo *)codegen_palloc(s, sizeof(struct loopinfo));

  p->type = t;
  p->pc1 = p->pc2 = p->pc3 = 0;
  p->prev = s->loop;
  p->ensure_level = s->ensure_level;
  p->acc = cursp();
  s->loop = p;

  return p;
}

static void
loop_break(codegen_scope *s, node *tree)
{
  if (!s->loop) {
    codegen(s, tree, NOVAL);
    raise_error(s, "unexpected break");
  }
  else {
    struct loopinfo *loop;

    if (tree) {
      codegen(s, tree, VAL);
      pop();
    }

    loop = s->loop;
    while (loop->type == LOOP_BEGIN) {
      genop_peep(s, MKOP_A(OP_POPERR, 1), NOVAL);
      loop = loop->prev;
    }
    while (loop->type == LOOP_RESCUE) {
      loop = loop->prev;
    }
    if (loop->type == LOOP_NORMAL) {
      int tmp;

      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      if (tree) {
        genop_peep(s, MKOP_AB(OP_MOVE, loop->acc, cursp()), NOVAL);
      }
      tmp = new_label(s);
      genop(s, MKOP_sBx(OP_JMP, loop->pc3));
      loop->pc3 = tmp;
    }
    else {
      genop(s, MKOP_AB(OP_RETURN, cursp(), OP_R_BREAK));
    }
  }
}

static void
loop_pop(codegen_scope *s, int val)
{
  if (val) {
    genop(s, MKOP_A(OP_LOADNIL, cursp()));
  }
  dispatch_linked(s, s->loop->pc3);
  s->loop = s->loop->prev;
  if (val) push();
}

static void
codedump(mrb_state *mrb, int n)
{
#ifdef ENABLE_STDIO
  mrb_irep *irep = mrb->irep[n];
  int i;
  mrb_code c;

  if (!irep) return;
  printf("irep %d nregs=%d nlocals=%d pools=%d syms=%d\n", n,
         irep->nregs, irep->nlocals, irep->plen, irep->slen);
  for (i=0; i<irep->ilen; i++) {
    printf("%03d ", i);
    c = irep->iseq[i];
    switch (GET_OPCODE(c)) {
    case OP_NOP:
      printf("OP_NOP\n");
      break;
    case OP_MOVE:
      printf("OP_MOVE\tR%d\tR%d\n", GETARG_A(c), GETARG_B(c));
      break;
    case OP_LOADL:
      printf("OP_LOADL\tR%d\tL(%d)\n", GETARG_A(c), GETARG_Bx(c));
      break;
    case OP_LOADI:
      printf("OP_LOADI\tR%d\t%d\n", GETARG_A(c), GETARG_sBx(c));
      break;
    case OP_LOADSYM:
      printf("OP_LOADSYM\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_LOADNIL:
      printf("OP_LOADNIL\tR%d\n", GETARG_A(c));
      break;
    case OP_LOADSELF:
      printf("OP_LOADSELF\tR%d\n", GETARG_A(c));
      break;
    case OP_LOADT:
      printf("OP_LOADT\tR%d\n", GETARG_A(c));
      break;
    case OP_LOADF:
      printf("OP_LOADF\tR%d\n", GETARG_A(c));
      break;
    case OP_GETGLOBAL:
      printf("OP_GETGLOBAL\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_SETGLOBAL:
      printf("OP_SETGLOBAL\t:%s\tR%d\n",
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
             GETARG_A(c));
      break;
    case OP_GETCONST:
      printf("OP_GETCONST\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_SETCONST:
      printf("OP_SETCONST\t:%s\tR%d\n",
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
             GETARG_A(c));
      break;
    case OP_GETMCNST:
      printf("OP_GETMCNST\tR%d\tR%d::%s\n", GETARG_A(c), GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_SETMCNST:
      printf("OP_SETMCNST\tR%d::%s\tR%d\n", GETARG_A(c)+1,
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
             GETARG_A(c));
      break;
    case OP_GETIV:
      printf("OP_GETIV\tR%d\t%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_SETIV:
      printf("OP_SETIV\t%s\tR%d\n",
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
             GETARG_A(c));
      break;
    case OP_GETUPVAR:
      printf("OP_GETUPVAR\tR%d\t%d\t%d\n",
             GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_SETUPVAR:
      printf("OP_SETUPVAR\tR%d\t%d\t%d\n",
             GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_GETCV:
      printf("OP_GETCV\tR%d\t%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
      break;
    case OP_SETCV:
      printf("OP_SETCV\t%s\tR%d\n",
             mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
             GETARG_A(c));
      break;
    case OP_JMP:
      printf("OP_JMP\t\t%03d\n", i+GETARG_sBx(c));
      break;
    case OP_JMPIF:
      printf("OP_JMPIF\tR%d\t%03d\n", GETARG_A(c), i+GETARG_sBx(c));
      break;
    case OP_JMPNOT:
      printf("OP_JMPNOT\tR%d\t%03d\n", GETARG_A(c), i+GETARG_sBx(c));
      break;
    case OP_SEND:
      printf("OP_SEND\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_SENDB:
      printf("OP_SENDB\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_TAILCALL:
      printf("OP_TAILCALL\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_SUPER:
      printf("OP_SUPER\tR%d\t%d\n", GETARG_A(c),
             GETARG_C(c));
      break;
    case OP_ARGARY:
      printf("OP_ARGARY\tR%d\t%d:%d:%d:%d\n", GETARG_A(c),
             (GETARG_Bx(c)>>10)&0x3f,
             (GETARG_Bx(c)>>9)&0x1,
             (GETARG_Bx(c)>>4)&0x1f,
             (GETARG_Bx(c)>>0)&0xf);
      break;

    case OP_ENTER:
      printf("OP_ENTER\t%d:%d:%d:%d:%d:%d:%d\n",
             (GETARG_Ax(c)>>18)&0x1f,
             (GETARG_Ax(c)>>13)&0x1f,
             (GETARG_Ax(c)>>12)&0x1,
             (GETARG_Ax(c)>>7)&0x1f,
             (GETARG_Ax(c)>>2)&0x1f,
             (GETARG_Ax(c)>>1)&0x1,
             GETARG_Ax(c) & 0x1);
      break;
    case OP_RETURN:
      printf("OP_RETURN\tR%d", GETARG_A(c));
      switch (GETARG_B(c)) {
      case OP_R_NORMAL:
        printf("\n"); break;
      case OP_R_RETURN:
        printf("\treturn\n"); break;
      case OP_R_BREAK:
        printf("\tbreak\n"); break;
      default:
        printf("\tbroken\n"); break;
        break;
      }
      break;
    case OP_BLKPUSH:
      printf("OP_BLKPUSH\tR%d\t%d:%d:%d:%d\n", GETARG_A(c),
             (GETARG_Bx(c)>>10)&0x3f,
             (GETARG_Bx(c)>>9)&0x1,
             (GETARG_Bx(c)>>4)&0x1f,
             (GETARG_Bx(c)>>0)&0xf);
      break;

    case OP_LAMBDA:
      printf("OP_LAMBDA\tR%d\tI(%+d)\t%d\n", GETARG_A(c), GETARG_b(c), GETARG_c(c));
      break;
    case OP_RANGE:
      printf("OP_RANGE\tR%d\tR%d\t%d\n", GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_METHOD:
      printf("OP_METHOD\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
      break;

    case OP_ADD:
      printf("OP_ADD\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_ADDI:
      printf("OP_ADDI\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_SUB:
      printf("OP_SUB\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_SUBI:
      printf("OP_SUBI\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_MUL:
      printf("OP_MUL\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_DIV:
      printf("OP_DIV\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_LT:
      printf("OP_LT\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_LE:
      printf("OP_LE\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_GT:
      printf("OP_GT\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_GE:
      printf("OP_GE\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;
    case OP_EQ:
      printf("OP_EQ\tR%d\t:%s\t%d\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
             GETARG_C(c));
      break;

    case OP_STOP:
      printf("OP_STOP\n");
      break;

    case OP_ARRAY:
      printf("OP_ARRAY\tR%d\tR%d\t%d\n", GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_ARYCAT:
      printf("OP_ARYCAT\tR%d\tR%d\n", GETARG_A(c), GETARG_B(c));
      break;
    case OP_ARYPUSH:
      printf("OP_ARYPUSH\tR%d\tR%d\n", GETARG_A(c), GETARG_B(c));
      break;
    case OP_AREF:
      printf("OP_AREF\tR%d\tR%d\t%d\n", GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_APOST:
      printf("OP_APOST\tR%d\t%d\t%d\n", GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    case OP_STRING:
      {
	mrb_value s = irep->pool[GETARG_Bx(c)];
	
	s = mrb_str_dump(mrb, s);
	printf("OP_STRING\tR%d\t%s\n", GETARG_A(c), RSTRING_PTR(s));
      }
      break;
    case OP_STRCAT:
      printf("OP_STRCAT\tR%d\tR%d\n", GETARG_A(c), GETARG_B(c));
      break;
    case OP_HASH:
      printf("OP_HASH\tR%d\tR%d\t%d\n", GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;

    case OP_OCLASS:
      printf("OP_OCLASS\tR%d\n", GETARG_A(c));
      break;
    case OP_CLASS:
      printf("OP_CLASS\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
      break;
    case OP_MODULE:
      printf("OP_MODULE\tR%d\t:%s\n", GETARG_A(c),
             mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
      break;
    case OP_EXEC:
      printf("OP_EXEC\tR%d\tI(%d)\n", GETARG_A(c), n+GETARG_Bx(c));
      break;
    case OP_SCLASS:
      printf("OP_SCLASS\tR%d\tR%d\n", GETARG_A(c), GETARG_B(c));
      break;
    case OP_TCLASS:
      printf("OP_TCLASS\tR%d\n", GETARG_A(c));
      break;
    case OP_ERR:
      printf("OP_ERR\tL(%d)\n", GETARG_Bx(c));
      break;
    case OP_EPUSH:
      printf("OP_EPUSH\t:I(%d)\n", n+GETARG_Bx(c));
      break;
    case OP_ONERR:
      printf("OP_ONERR\t%03d\n", i+GETARG_sBx(c));
      break;
    case OP_RESCUE:
      printf("OP_RESCUE\tR%d\n", GETARG_A(c));
      break;
    case OP_RAISE:
      printf("OP_RAISE\tR%d\n", GETARG_A(c));
      break;
    case OP_POPERR:
      printf("OP_POPERR\t%d\n", GETARG_A(c));
      break;
    case OP_EPOP:
      printf("OP_EPOP\t%d\n", GETARG_A(c));
      break;

    default:
      printf("OP_unknown %d\t%d\t%d\t%d\n", GET_OPCODE(c),
             GETARG_A(c), GETARG_B(c), GETARG_C(c));
      break;
    }
  }
  printf("\n");
#endif
}

void
codedump_all(mrb_state *mrb, int start)
{
  int i;

  for (i=start; i<mrb->irep_len; i++) {
    codedump(mrb, i);
  }
}

static int
codegen_start(mrb_state *mrb, parser_state *p)
{
  codegen_scope *scope = scope_new(mrb, 0, 0);

  if (!scope) {
    return -1;
  }
  scope->mrb = mrb;
  if (p->filename) {
    scope->filename = p->filename;
  }
  if (setjmp(scope->jmp) != 0) {
    return -1;
  }
  // prepare irep
  codegen(scope, p->tree, NOVAL);
  mrb_pool_close(scope->mpool);
  return 0;
}

int
mrb_generate_code(mrb_state *mrb, parser_state *p)
{
  int start = mrb->irep_len;
  int n;

  n = codegen_start(mrb, p);
  if (n < 0) return n;

  return start;
}

#ifdef CODEGEN_TEST
int
main()
{
  mrb_state *mrb = mrb_open();
  int n;

#if 1
  n = mrb_compile_string(mrb, "p(__FILE__)\np(__LINE__)");
#else
  n = mrb_compile_string(mrb, "\
def fib(n)\n\
  if n<2\n\
    n\n\
  else\n\
    fib(n-2)+fib(n-1)\n\
  end\n\
end\n\
p(fib(30), \"\\n\")\n\
");
#endif
  printf("ret: %d\n", n);
#ifdef CODEGEN_DUMP
  codedump_all(mrb, n);
#endif
  mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[0]), mrb_nil_value());
  mrb_close(mrb);

  return 0;
}
#endif
