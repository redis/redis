/**********************************************************************
  regexec.c -  Oniguruma (regular expression library)
**********************************************************************/
/*-
 * Copyright (c) 2002-2008  K.Kosako  <sndgk393 AT ybb DOT ne DOT jp>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "mruby.h"
#ifdef ENABLE_REGEXP
#include <string.h>
#include "regint.h"

/* #define USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE */

#ifdef USE_CRNL_AS_LINE_TERMINATOR
#define ONIGENC_IS_MBC_CRNL(enc,p,end) \
  (ONIGENC_MBC_TO_CODE(enc,p,end) == 13 && \
   ONIGENC_IS_MBC_NEWLINE(enc,(p+enclen(enc,p)),end))
#endif

#ifdef USE_CAPTURE_HISTORY
static void history_tree_free(OnigCaptureTreeNode* node);

static void
history_tree_clear(OnigCaptureTreeNode* node)
{
  int i;

  if (IS_NOT_NULL(node)) {
    for (i = 0; i < node->num_childs; i++) {
      if (IS_NOT_NULL(node->childs[i])) {
        history_tree_free(node->childs[i]);
      }
    }
    for (i = 0; i < node->allocated; i++) {
      node->childs[i] = (OnigCaptureTreeNode* )0;
    }
    node->num_childs = 0;
    node->beg = ONIG_REGION_NOTPOS;
    node->end = ONIG_REGION_NOTPOS;
    node->group = -1;
  }
}

static void
history_tree_free(OnigCaptureTreeNode* node)
{
  history_tree_clear(node);
  xfree(node);
}

static void
history_root_free(OnigRegion* r)
{
  if (IS_NOT_NULL(r->history_root)) {
    history_tree_free(r->history_root);
    r->history_root = (OnigCaptureTreeNode* )0;
  }
}

static OnigCaptureTreeNode*
history_node_new(void)
{
  OnigCaptureTreeNode* node;

  node = (OnigCaptureTreeNode* )xmalloc(sizeof(OnigCaptureTreeNode));
  CHECK_NULL_RETURN(node);
  node->childs     = (OnigCaptureTreeNode** )0;
  node->allocated  = 0;
  node->num_childs = 0;
  node->group      = -1;
  node->beg        = ONIG_REGION_NOTPOS;
  node->end        = ONIG_REGION_NOTPOS;

  return node;
}

static int
history_tree_add_child(OnigCaptureTreeNode* parent, OnigCaptureTreeNode* child)
{
#define HISTORY_TREE_INIT_ALLOC_SIZE  8

  if (parent->num_childs >= parent->allocated) {
    int n, i;

    if (IS_NULL(parent->childs)) {
      n = HISTORY_TREE_INIT_ALLOC_SIZE;
      parent->childs =
        (OnigCaptureTreeNode** )xmalloc(sizeof(OnigCaptureTreeNode*) * n);
    }
    else {
      n = parent->allocated * 2;
      parent->childs =
        (OnigCaptureTreeNode** )xrealloc(parent->childs,
                                         sizeof(OnigCaptureTreeNode*) * n);
    }
    CHECK_NULL_RETURN_MEMERR(parent->childs);
    for (i = parent->allocated; i < n; i++) {
      parent->childs[i] = (OnigCaptureTreeNode* )0;
    }
    parent->allocated = n;
  }

  parent->childs[parent->num_childs] = child;
  parent->num_childs++;
  return 0;
}

static OnigCaptureTreeNode*
history_tree_clone(OnigCaptureTreeNode* node)
{
  int i;
  OnigCaptureTreeNode *clone, *child;

  clone = history_node_new();
  CHECK_NULL_RETURN(clone);

  clone->beg = node->beg;
  clone->end = node->end;
  for (i = 0; i < node->num_childs; i++) {
    child = history_tree_clone(node->childs[i]);
    if (IS_NULL(child)) {
      history_tree_free(clone);
      return (OnigCaptureTreeNode* )0;
    }
    history_tree_add_child(clone, child);
  }

  return clone;
}

extern  OnigCaptureTreeNode*
onig_get_capture_tree(OnigRegion* region)
{
  return region->history_root;
}
#endif /* USE_CAPTURE_HISTORY */

extern void
onig_region_clear(OnigRegion* region)
{
  int i;

  for (i = 0; i < region->num_regs; i++) {
    region->beg[i] = region->end[i] = ONIG_REGION_NOTPOS;
  }
#ifdef USE_CAPTURE_HISTORY
  history_root_free(region);
#endif
}

extern int
onig_region_resize(OnigRegion* region, int n)
{
  region->num_regs = n;

  if (n < ONIG_NREGION)
    n = ONIG_NREGION;

  if (region->allocated == 0) {
    region->beg = (int* )xmalloc(n * sizeof(int));
    if (region->beg == 0)
      return ONIGERR_MEMORY;

    region->end = (int* )xmalloc(n * sizeof(int));
    if (region->end == 0) {
      xfree(region->beg);
      return ONIGERR_MEMORY;
    }

    region->allocated = n;
  }
  else if (region->allocated < n) {
    int *tmp;

    region->allocated = 0;
    tmp = (int* )xrealloc(region->beg, n * sizeof(int));
    if (tmp == 0) {
      xfree(region->beg);
      xfree(region->end);
      return ONIGERR_MEMORY;
    }
    region->beg = tmp;
    tmp = (int* )xrealloc(region->end, n * sizeof(int));
    if (tmp == 0) {
      xfree(region->beg);
      return ONIGERR_MEMORY;
    }
    region->end = tmp;

    if (region->beg == 0 || region->end == 0)
      return ONIGERR_MEMORY;

    region->allocated = n;
  }

  return 0;
}

static int
onig_region_resize_clear(OnigRegion* region, int n)
{
  int r;

  r = onig_region_resize(region, n);
  if (r != 0) return r;
  onig_region_clear(region);
  return 0;
}

extern int
onig_region_set(OnigRegion* region, int at, int beg, int end)
{
  if (at < 0) return ONIGERR_INVALID_ARGUMENT;

  if (at >= region->allocated) {
    int r = onig_region_resize(region, at + 1);
    if (r < 0) return r;
  }

  region->beg[at] = beg;
  region->end[at] = end;
  return 0;
}

extern void
onig_region_init(OnigRegion* region)
{
  region->num_regs     = 0;
  region->allocated    = 0;
  region->beg          = (int* )0;
  region->end          = (int* )0;
  region->history_root = (OnigCaptureTreeNode* )0;
}

extern OnigRegion*
onig_region_new(void)
{
  OnigRegion* r;

  r = (OnigRegion* )xmalloc(sizeof(OnigRegion));
  if (r)
    onig_region_init(r);
  return r;
}

extern void
onig_region_free(OnigRegion* r, int free_self)
{
  if (r) {
    if (r->allocated > 0) {
      if (r->beg) xfree(r->beg);
      if (r->end) xfree(r->end);
      r->allocated = 0;
    }
#ifdef USE_CAPTURE_HISTORY
    history_root_free(r);
#endif
    if (free_self) xfree(r);
  }
}

extern void
onig_region_copy(OnigRegion* to, OnigRegion* from)
{
#define RREGC_SIZE   (sizeof(int) * from->num_regs)
  int i;

  if (to == from) return;

  onig_region_resize(to, from->num_regs);
  for (i = 0; i < from->num_regs; i++) {
    to->beg[i] = from->beg[i];
    to->end[i] = from->end[i];
  }
  to->num_regs = from->num_regs;

#ifdef USE_CAPTURE_HISTORY
  history_root_free(to);

  if (IS_NOT_NULL(from->history_root)) {
    to->history_root = history_tree_clone(from->history_root);
  }
#endif
}


/** stack **/
#define INVALID_STACK_INDEX   -1

/* stack type */
/* used by normal-POP */
#define STK_ALT                    0x0001
#define STK_LOOK_BEHIND_NOT        0x0002
#define STK_POS_NOT                0x0003
/* handled by normal-POP */
#define STK_MEM_START              0x0100
#define STK_MEM_END                0x8200
#define STK_REPEAT_INC             0x0300
#define STK_STATE_CHECK_MARK       0x1000
/* avoided by normal-POP */
#define STK_NULL_CHECK_START       0x3000
#define STK_NULL_CHECK_END         0x5000  /* for recursive call */
#define STK_MEM_END_MARK           0x8400
#define STK_POS                    0x0500  /* used when POP-POS */
#define STK_STOP_BT                0x0600  /* mark for "(?>...)" */
#define STK_REPEAT                 0x0700
#define STK_CALL_FRAME             0x0800
#define STK_RETURN                 0x0900
#define STK_VOID                   0x0a00  /* for fill a blank */

/* stack type check mask */
#define STK_MASK_POP_USED          0x00ff
#define STK_MASK_TO_VOID_TARGET    0x10ff
#define STK_MASK_MEM_END_OR_MARK   0x8000  /* MEM_END or MEM_END_MARK */

#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
#define MATCH_ARG_INIT(msa, arg_option, arg_region, arg_start) do {\
  (msa).stack_p  = (void* )0;\
  (msa).options  = (arg_option);\
  (msa).region   = (arg_region);\
  (msa).start    = (arg_start);\
  (msa).best_len = ONIG_MISMATCH;\
} while(0)
#else
#define MATCH_ARG_INIT(msa, arg_option, arg_region, arg_start) do {\
  (msa).stack_p  = (void* )0;\
  (msa).options  = (arg_option);\
  (msa).region   = (arg_region);\
  (msa).start    = (arg_start);\
} while(0)
#endif

#ifdef USE_COMBINATION_EXPLOSION_CHECK

#define STATE_CHECK_BUFF_MALLOC_THRESHOLD_SIZE  16

#define STATE_CHECK_BUFF_INIT(msa, str_len, offset, state_num) do {     \
  if ((state_num) > 0 && str_len >= STATE_CHECK_STRING_THRESHOLD_LEN) {\
    unsigned int size = (unsigned int )(((str_len) + 1) * (state_num) + 7) >> 3;\
    offset = ((offset) * (state_num)) >> 3;\
    if (size > 0 && offset < size && size < STATE_CHECK_BUFF_MAX_SIZE) {\
      if (size >= STATE_CHECK_BUFF_MALLOC_THRESHOLD_SIZE) {\
        (msa).state_check_buff = (void* )xmalloc(size);\
        CHECK_NULL_RETURN_MEMERR((msa).state_check_buff);\
      }\
      else \
        (msa).state_check_buff = (void* )xalloca(size);\
      xmemset(((char* )((msa).state_check_buff)+(offset)), 0, \
              (size_t )(size - (offset))); \
      (msa).state_check_buff_size = size;\
    }\
    else {\
      (msa).state_check_buff = (void* )0;\
      (msa).state_check_buff_size = 0;\
    }\
  }\
  else {\
    (msa).state_check_buff = (void* )0;\
    (msa).state_check_buff_size = 0;\
  }\
  } while(0)

#define MATCH_ARG_FREE(msa) do {\
  if ((msa).stack_p) xfree((msa).stack_p);\
  if ((msa).state_check_buff_size >= STATE_CHECK_BUFF_MALLOC_THRESHOLD_SIZE) { \
    if ((msa).state_check_buff) xfree((msa).state_check_buff);\
  }\
} while(0)
#else
#define MATCH_ARG_FREE(msa)  if ((msa).stack_p) xfree((msa).stack_p)
#endif



#define STACK_INIT(alloc_addr, ptr_num, stack_num)  do {\
  if (msa->stack_p) {\
    alloc_addr = (char* )xalloca(sizeof(char*) * (ptr_num));\
    stk_alloc  = (OnigStackType* )(msa->stack_p);\
    stk_base   = stk_alloc;\
    stk        = stk_base;\
    stk_end    = stk_base + msa->stack_n;\
  }\
  else {\
    alloc_addr = (char* )xalloca(sizeof(char*) * (ptr_num)\
                       + sizeof(OnigStackType) * (stack_num));\
    stk_alloc  = (OnigStackType* )(alloc_addr + sizeof(char*) * (ptr_num));\
    stk_base   = stk_alloc;\
    stk        = stk_base;\
    stk_end    = stk_base + (stack_num);\
  }\
} while(0)

#define STACK_SAVE do{\
  if (stk_base != stk_alloc) {\
    msa->stack_p = stk_base;\
    msa->stack_n = stk_end - stk_base; /* TODO: check overflow */\
  };\
} while(0)

static unsigned int MatchStackLimitSize = DEFAULT_MATCH_STACK_LIMIT_SIZE;

extern unsigned int
onig_get_match_stack_limit_size(void)
{
  return MatchStackLimitSize;
}

extern int
onig_set_match_stack_limit_size(unsigned int size)
{
  MatchStackLimitSize = size;
  return 0;
}

static int
stack_double(OnigStackType** arg_stk_base, OnigStackType** arg_stk_end,
             OnigStackType** arg_stk, OnigStackType* stk_alloc, OnigMatchArg* msa)
{
  size_t n;
  OnigStackType *x, *stk_base, *stk_end, *stk;

  stk_base = *arg_stk_base;
  stk_end  = *arg_stk_end;
  stk      = *arg_stk;

  n = stk_end - stk_base;
  if (stk_base == stk_alloc && IS_NULL(msa->stack_p)) {
    x = (OnigStackType* )xmalloc(sizeof(OnigStackType) * n * 2);
    if (IS_NULL(x)) {
      STACK_SAVE;
      return ONIGERR_MEMORY;
    }
    xmemcpy(x, stk_base, n * sizeof(OnigStackType));
    n *= 2;
  }
  else {
    unsigned int limit_size = MatchStackLimitSize;
    n *= 2;
    if (limit_size != 0 && n > limit_size) {
      if ((unsigned int )(stk_end - stk_base) == limit_size)
        return ONIGERR_MATCH_STACK_LIMIT_OVER;
      else
        n = limit_size;
    }
    x = (OnigStackType* )xrealloc(stk_base, sizeof(OnigStackType) * n);
    if (IS_NULL(x)) {
      STACK_SAVE;
      return ONIGERR_MEMORY;
    }
  }
  *arg_stk      = x + (stk - stk_base);
  *arg_stk_base = x;
  *arg_stk_end  = x + n;
  return 0;
}

#define STACK_ENSURE(n) do {\
  if (stk_end - stk < (n)) {\
    int r = stack_double(&stk_base, &stk_end, &stk, stk_alloc, msa);\
    if (r != 0) { STACK_SAVE; return r; } \
  }\
} while(0)

#define STACK_AT(index)        (stk_base + (index))
#define GET_STACK_INDEX(stk)   ((stk) - stk_base)

#define STACK_PUSH_TYPE(stack_type) do {\
  STACK_ENSURE(1);\
  stk->type = (stack_type);\
  STACK_INC;\
} while(0)

#define IS_TO_VOID_TARGET(stk) (((stk)->type & STK_MASK_TO_VOID_TARGET) != 0)

#ifdef USE_COMBINATION_EXPLOSION_CHECK
#define STATE_CHECK_POS(s,snum) \
  (((s) - str) * num_comb_exp_check + ((snum) - 1))
#define STATE_CHECK_VAL(v,snum) do {\
  if (state_check_buff != NULL) {\
    int x = STATE_CHECK_POS(s,snum);\
    (v) = state_check_buff[x/8] & (1<<(x%8));\
  }\
  else (v) = 0;\
} while(0)


#define ELSE_IF_STATE_CHECK_MARK(stk) \
  else if ((stk)->type == STK_STATE_CHECK_MARK) { \
    int x = STATE_CHECK_POS(stk->u.state.pstr, stk->u.state.state_check);\
    state_check_buff[x/8] |= (1<<(x%8));                                \
  }

#define STACK_PUSH(stack_type,pat,s,sprev) do {\
  STACK_ENSURE(1);\
  stk->type = (stack_type);\
  stk->u.state.pcode     = (pat);\
  stk->u.state.pstr      = (s);\
  stk->u.state.pstr_prev = (sprev);\
  stk->u.state.state_check = 0;\
  STACK_INC;\
} while(0)

#define STACK_PUSH_ENSURED(stack_type,pat) do {\
  stk->type = (stack_type);\
  stk->u.state.pcode = (pat);\
  stk->u.state.state_check = 0;\
  STACK_INC;\
} while(0)

#define STACK_PUSH_ALT_WITH_STATE_CHECK(pat,s,sprev,snum) do {\
  STACK_ENSURE(1);\
  stk->type = STK_ALT;\
  stk->u.state.pcode     = (pat);\
  stk->u.state.pstr      = (s);\
  stk->u.state.pstr_prev = (sprev);\
  stk->u.state.state_check = ((state_check_buff != NULL) ? (snum) : 0);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_STATE_CHECK(s,snum) do {\
  if (state_check_buff != NULL) {\
    STACK_ENSURE(1);\
    stk->type = STK_STATE_CHECK_MARK;\
    stk->u.state.pstr = (s);\
    stk->u.state.state_check = (snum);\
    STACK_INC;\
  }\
} while(0)

#else /* USE_COMBINATION_EXPLOSION_CHECK */

#define ELSE_IF_STATE_CHECK_MARK(stk)

#define STACK_PUSH(stack_type,pat,s,sprev) do {\
  STACK_ENSURE(1);\
  stk->type = (stack_type);\
  stk->u.state.pcode     = (pat);\
  stk->u.state.pstr      = (s);\
  stk->u.state.pstr_prev = (sprev);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_ENSURED(stack_type,pat) do {\
  stk->type = (stack_type);\
  stk->u.state.pcode = (pat);\
  STACK_INC;\
} while(0)
#endif /* USE_COMBINATION_EXPLOSION_CHECK */

#define STACK_PUSH_ALT(pat,s,sprev)     STACK_PUSH(STK_ALT,pat,s,sprev)
#define STACK_PUSH_POS(s,sprev)         STACK_PUSH(STK_POS,NULL_UCHARP,s,sprev)
#define STACK_PUSH_POS_NOT(pat,s,sprev) STACK_PUSH(STK_POS_NOT,pat,s,sprev)
#define STACK_PUSH_STOP_BT              STACK_PUSH_TYPE(STK_STOP_BT)
#define STACK_PUSH_LOOK_BEHIND_NOT(pat,s,sprev) \
        STACK_PUSH(STK_LOOK_BEHIND_NOT,pat,s,sprev)

#define STACK_PUSH_REPEAT(id, pat) do {\
  STACK_ENSURE(1);\
  stk->type = STK_REPEAT;\
  stk->u.repeat.num    = (id);\
  stk->u.repeat.pcode  = (pat);\
  stk->u.repeat.count  = 0;\
  STACK_INC;\
} while(0)

#define STACK_PUSH_REPEAT_INC(sindex) do {\
  STACK_ENSURE(1);\
  stk->type = STK_REPEAT_INC;\
  stk->u.repeat_inc.si  = (sindex);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_MEM_START(mnum, s) do {\
  STACK_ENSURE(1);\
  stk->type = STK_MEM_START;\
  stk->u.mem.num      = (mnum);\
  stk->u.mem.pstr     = (s);\
  stk->u.mem.start    = mem_start_stk[mnum];\
  stk->u.mem.end      = mem_end_stk[mnum];\
  mem_start_stk[mnum] = GET_STACK_INDEX(stk);\
  mem_end_stk[mnum]   = INVALID_STACK_INDEX;\
  STACK_INC;\
} while(0)

#define STACK_PUSH_MEM_END(mnum, s) do {\
  STACK_ENSURE(1);\
  stk->type = STK_MEM_END;\
  stk->u.mem.num    = (mnum);\
  stk->u.mem.pstr   = (s);\
  stk->u.mem.start  = mem_start_stk[mnum];\
  stk->u.mem.end    = mem_end_stk[mnum];\
  mem_end_stk[mnum] = GET_STACK_INDEX(stk);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_MEM_END_MARK(mnum) do {\
  STACK_ENSURE(1);\
  stk->type = STK_MEM_END_MARK;\
  stk->u.mem.num = (mnum);\
  STACK_INC;\
} while(0)

#define STACK_GET_MEM_START(mnum, k) do {\
  int level = 0;\
  k = stk;\
  while (k > stk_base) {\
    k--;\
    if ((k->type & STK_MASK_MEM_END_OR_MARK) != 0 \
      && k->u.mem.num == (mnum)) {\
      level++;\
    }\
    else if (k->type == STK_MEM_START && k->u.mem.num == (mnum)) {\
      if (level == 0) break;\
      level--;\
    }\
  }\
} while(0)

#define STACK_GET_MEM_RANGE(k, mnum, start, end) do {\
  int level = 0;\
  while (k < stk) {\
    if (k->type == STK_MEM_START && k->u.mem.num == (mnum)) {\
      if (level == 0) (start) = k->u.mem.pstr;\
      level++;\
    }\
    else if (k->type == STK_MEM_END && k->u.mem.num == (mnum)) {\
      level--;\
      if (level == 0) {\
        (end) = k->u.mem.pstr;\
        break;\
      }\
    }\
    k++;\
  }\
} while(0)

#define STACK_PUSH_NULL_CHECK_START(cnum, s) do {\
  STACK_ENSURE(1);\
  stk->type = STK_NULL_CHECK_START;\
  stk->u.null_check.num  = (cnum);\
  stk->u.null_check.pstr = (s);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_NULL_CHECK_END(cnum) do {\
  STACK_ENSURE(1);\
  stk->type = STK_NULL_CHECK_END;\
  stk->u.null_check.num  = (cnum);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_CALL_FRAME(pat) do {\
  STACK_ENSURE(1);\
  stk->type = STK_CALL_FRAME;\
  stk->u.call_frame.ret_addr = (pat);\
  STACK_INC;\
} while(0)

#define STACK_PUSH_RETURN do {\
  STACK_ENSURE(1);\
  stk->type = STK_RETURN;\
  STACK_INC;\
} while(0)


#ifdef ONIG_DEBUG
#define STACK_BASE_CHECK(p, at) \
  if ((p) < stk_base) {\
    fprintf(stderr, "at %s\n", at);\
    goto stack_error;\
  }
#else
#define STACK_BASE_CHECK(p, at)
#endif

#define STACK_POP_ONE do {\
  stk--;\
  STACK_BASE_CHECK(stk, "STACK_POP_ONE"); \
} while(0)

#define STACK_POP  do {\
  switch (pop_level) {\
  case STACK_POP_LEVEL_FREE:\
    while (1) {\
      stk--;\
      STACK_BASE_CHECK(stk, "STACK_POP"); \
      if ((stk->type & STK_MASK_POP_USED) != 0)  break;\
      ELSE_IF_STATE_CHECK_MARK(stk);\
    }\
    break;\
  case STACK_POP_LEVEL_MEM_START:\
    while (1) {\
      stk--;\
      STACK_BASE_CHECK(stk, "STACK_POP 2"); \
      if ((stk->type & STK_MASK_POP_USED) != 0)  break;\
      else if (stk->type == STK_MEM_START) {\
        mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
        mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
      }\
      ELSE_IF_STATE_CHECK_MARK(stk);\
    }\
    break;\
  default:\
    while (1) {\
      stk--;\
      STACK_BASE_CHECK(stk, "STACK_POP 3"); \
      if ((stk->type & STK_MASK_POP_USED) != 0)  break;\
      else if (stk->type == STK_MEM_START) {\
        mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
        mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
      }\
      else if (stk->type == STK_REPEAT_INC) {\
        STACK_AT(stk->u.repeat_inc.si)->u.repeat.count--;\
      }\
      else if (stk->type == STK_MEM_END) {\
        mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
        mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
      }\
      ELSE_IF_STATE_CHECK_MARK(stk);\
    }\
    break;\
  }\
} while(0)

#define STACK_POP_TIL_POS_NOT  do {\
  while (1) {\
    stk--;\
    STACK_BASE_CHECK(stk, "STACK_POP_TIL_POS_NOT"); \
    if (stk->type == STK_POS_NOT) break;\
    else if (stk->type == STK_MEM_START) {\
      mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
      mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
    }\
    else if (stk->type == STK_REPEAT_INC) {\
      STACK_AT(stk->u.repeat_inc.si)->u.repeat.count--;\
    }\
    else if (stk->type == STK_MEM_END) {\
      mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
      mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
    }\
    ELSE_IF_STATE_CHECK_MARK(stk);\
  }\
} while(0)

#define STACK_POP_TIL_LOOK_BEHIND_NOT  do {\
  while (1) {\
    stk--;\
    STACK_BASE_CHECK(stk, "STACK_POP_TIL_LOOK_BEHIND_NOT"); \
    if (stk->type == STK_LOOK_BEHIND_NOT) break;\
    else if (stk->type == STK_MEM_START) {\
      mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
      mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
    }\
    else if (stk->type == STK_REPEAT_INC) {\
      STACK_AT(stk->u.repeat_inc.si)->u.repeat.count--;\
    }\
    else if (stk->type == STK_MEM_END) {\
      mem_start_stk[stk->u.mem.num] = stk->u.mem.start;\
      mem_end_stk[stk->u.mem.num]   = stk->u.mem.end;\
    }\
    ELSE_IF_STATE_CHECK_MARK(stk);\
  }\
} while(0)

#define STACK_POS_END(k) do {\
  k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_POS_END"); \
    if (IS_TO_VOID_TARGET(k)) {\
      k->type = STK_VOID;\
    }\
    else if (k->type == STK_POS) {\
      k->type = STK_VOID;\
      break;\
    }\
  }\
} while(0)

#define STACK_STOP_BT_END do {\
  OnigStackType *k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_STOP_BT_END"); \
    if (IS_TO_VOID_TARGET(k)) {\
      k->type = STK_VOID;\
    }\
    else if (k->type == STK_STOP_BT) {\
      k->type = STK_VOID;\
      break;\
    }\
  }\
} while(0)

#define STACK_NULL_CHECK(isnull,id,s) do {\
  OnigStackType* k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_NULL_CHECK"); \
    if (k->type == STK_NULL_CHECK_START) {\
      if (k->u.null_check.num == (id)) {\
        (isnull) = (k->u.null_check.pstr == (s));\
        break;\
      }\
    }\
  }\
} while(0)

#define STACK_NULL_CHECK_REC(isnull,id,s) do {\
  int level = 0;\
  OnigStackType* k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_NULL_CHECK_REC"); \
    if (k->type == STK_NULL_CHECK_START) {\
      if (k->u.null_check.num == (id)) {\
        if (level == 0) {\
          (isnull) = (k->u.null_check.pstr == (s));\
          break;\
        }\
        else level--;\
      }\
    }\
    else if (k->type == STK_NULL_CHECK_END) {\
      level++;\
    }\
  }\
} while(0)

#define STACK_NULL_CHECK_MEMST(isnull,id,s,reg) do {\
  OnigStackType* k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_NULL_CHECK_MEMST"); \
    if (k->type == STK_NULL_CHECK_START) {\
      if (k->u.null_check.num == (id)) {\
        if (k->u.null_check.pstr != (s)) {\
          (isnull) = 0;\
          break;\
        }\
        else {\
          UChar* endp;\
          (isnull) = 1;\
          while (k < stk) {\
            if (k->type == STK_MEM_START) {\
              if (k->u.mem.end == INVALID_STACK_INDEX) {\
                (isnull) = 0; break;\
              }\
              if (BIT_STATUS_AT(reg->bt_mem_end, k->u.mem.num))\
                endp = STACK_AT(k->u.mem.end)->u.mem.pstr;\
              else\
                endp = (UChar* )k->u.mem.end;\
              if (STACK_AT(k->u.mem.start)->u.mem.pstr != endp) {\
                (isnull) = 0; break;\
              }\
              else if (endp != s) {\
                (isnull) = -1; /* empty, but position changed */ \
              }\
            }\
            k++;\
          }\
          break;\
        }\
      }\
    }\
  }\
} while(0)

#define STACK_NULL_CHECK_MEMST_REC(isnull,id,s,reg) do {\
  int level = 0;\
  OnigStackType* k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_NULL_CHECK_MEMST_REC"); \
    if (k->type == STK_NULL_CHECK_START) {\
      if (k->u.null_check.num == (id)) {\
        if (level == 0) {\
          if (k->u.null_check.pstr != (s)) {\
            (isnull) = 0;\
            break;\
          }\
          else {\
            UChar* endp;\
            (isnull) = 1;\
            while (k < stk) {\
              if (k->type == STK_MEM_START) {\
                if (k->u.mem.end == INVALID_STACK_INDEX) {\
                  (isnull) = 0; break;\
                }\
                if (BIT_STATUS_AT(reg->bt_mem_end, k->u.mem.num))\
                  endp = STACK_AT(k->u.mem.end)->u.mem.pstr;\
                else\
                  endp = (UChar* )k->u.mem.end;\
                if (STACK_AT(k->u.mem.start)->u.mem.pstr != endp) {\
                  (isnull) = 0; break;\
                }\
                else if (endp != s) {\
                  (isnull) = -1; /* empty, but position changed */ \
                }\
              }\
              k++;\
            }\
            break;\
          }\
        }\
        else {\
          level--;\
        }\
      }\
    }\
    else if (k->type == STK_NULL_CHECK_END) {\
      if (k->u.null_check.num == (id)) level++;\
    }\
  }\
} while(0)

#define STACK_GET_REPEAT(id, k) do {\
  int level = 0;\
  k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_GET_REPEAT"); \
    if (k->type == STK_REPEAT) {\
      if (level == 0) {\
        if (k->u.repeat.num == (id)) {\
          break;\
        }\
      }\
    }\
    else if (k->type == STK_CALL_FRAME) level--;\
    else if (k->type == STK_RETURN)     level++;\
  }\
} while(0)

#define STACK_RETURN(addr)  do {\
  int level = 0;\
  OnigStackType* k = stk;\
  while (1) {\
    k--;\
    STACK_BASE_CHECK(k, "STACK_RETURN"); \
    if (k->type == STK_CALL_FRAME) {\
      if (level == 0) {\
        (addr) = k->u.call_frame.ret_addr;\
        break;\
      }\
      else level--;\
    }\
    else if (k->type == STK_RETURN)\
      level++;\
  }\
} while(0)


#define STRING_CMP(s1,s2,len) do {\
  while (len-- > 0) {\
    if (*s1++ != *s2++) goto fail;\
  }\
} while(0)

#define STRING_CMP_IC(case_fold_flag,s1,ps2,len,text_end) do {\
  if (string_cmp_ic(encode, case_fold_flag, s1, ps2, len, text_end) == 0) \
    goto fail; \
} while(0)

static int string_cmp_ic(OnigEncoding enc, int case_fold_flag,
                         UChar* s1, UChar** ps2, int mblen, const UChar* text_end)
{
  UChar buf1[ONIGENC_MBC_CASE_FOLD_MAXLEN];
  UChar buf2[ONIGENC_MBC_CASE_FOLD_MAXLEN];
  UChar *p1, *p2, *end1, *s2;
  int len1, len2;

  s2   = *ps2;
  end1 = s1 + mblen;
  while (s1 < end1) {
    len1 = ONIGENC_MBC_CASE_FOLD(enc, case_fold_flag, &s1, text_end, buf1);
    len2 = ONIGENC_MBC_CASE_FOLD(enc, case_fold_flag, &s2, text_end, buf2);
    if (len1 != len2) return 0;
    p1 = buf1;
    p2 = buf2;
    while (len1-- > 0) {
      if (*p1 != *p2) return 0;
      p1++;
      p2++;
    }
  }

  *ps2 = s2;
  return 1;
}

#define STRING_CMP_VALUE(s1,s2,len,is_fail) do {\
  is_fail = 0;\
  while (len-- > 0) {\
    if (*s1++ != *s2++) {\
      is_fail = 1; break;\
    }\
  }\
} while(0)

#define STRING_CMP_VALUE_IC(case_fold_flag,s1,ps2,len,text_end,is_fail) do {\
  if (string_cmp_ic(encode, case_fold_flag, s1, ps2, len, text_end) == 0) \
    is_fail = 1; \
  else \
    is_fail = 0; \
} while(0)


#define IS_EMPTY_STR           (str == end)
#define ON_STR_BEGIN(s)       ((s) == str)
#define ON_STR_END(s)         ((s) == end)
#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
#define DATA_ENSURE_CHECK1     (s < right_range)
#define DATA_ENSURE_CHECK(n)   (s + (n) <= right_range)
#define DATA_ENSURE(n)         if (s + (n) > right_range) goto fail
#else
#define DATA_ENSURE_CHECK1     (s < end)
#define DATA_ENSURE_CHECK(n)   (s + (n) <= end)
#define DATA_ENSURE(n)         if (s + (n) > end) goto fail
#endif /* USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE */


#ifdef USE_CAPTURE_HISTORY
static int
make_capture_history_tree(OnigCaptureTreeNode* node, OnigStackType** kp,
                          OnigStackType* stk_top, UChar* str, regex_t* reg)
{
  int n, r;
  OnigCaptureTreeNode* child;
  OnigStackType* k = *kp;

  while (k < stk_top) {
    if (k->type == STK_MEM_START) {
      n = k->u.mem.num;
      if (n <= ONIG_MAX_CAPTURE_HISTORY_GROUP &&
          BIT_STATUS_AT(reg->capture_history, n) != 0) {
        child = history_node_new();
        CHECK_NULL_RETURN_MEMERR(child);
        child->group = n;
        child->beg = (int )(k->u.mem.pstr - str);
        r = history_tree_add_child(node, child);
        if (r != 0) return r;
        *kp = (k + 1);
        r = make_capture_history_tree(child, kp, stk_top, str, reg);
        if (r != 0) return r;

        k = *kp;
        child->end = (int )(k->u.mem.pstr - str);
      }
    }
    else if (k->type == STK_MEM_END) {
      if (k->u.mem.num == node->group) {
        node->end = (int )(k->u.mem.pstr - str);
        *kp = k;
        return 0;
      }
    }
    k++;
  }

  return 1; /* 1: root node ending. */
}
#endif

#ifdef USE_BACKREF_WITH_LEVEL
static int mem_is_in_memp(int mem, int num, UChar* memp)
{
  int i;
  MemNumType m;

  for (i = 0; i < num; i++) {
    GET_MEMNUM_INC(m, memp);
    if (mem == (int )m) return 1;
  }
  return 0;
}

static int backref_match_at_nested_level(regex_t* reg
         , OnigStackType* top, OnigStackType* stk_base
         , int ignore_case, int case_fold_flag
         , int nest, int mem_num, UChar* memp, UChar** s, const UChar* send)
{
  UChar *ss, *p, *pstart, *pend = NULL_UCHARP;
  int level;
  OnigStackType* k;

  level = 0;
  k = top;
  k--;
  while (k >= stk_base) {
    if (k->type == STK_CALL_FRAME) {
      level--;
    }
    else if (k->type == STK_RETURN) {
      level++;
    }
    else if (level == nest) {
      if (k->type == STK_MEM_START) {
        if (mem_is_in_memp(k->u.mem.num, mem_num, memp)) {
          pstart = k->u.mem.pstr;
          if (pend != NULL_UCHARP) {
            if (pend - pstart > send - *s) return 0; /* or goto next_mem; */
            p  = pstart;
            ss = *s;

            if (ignore_case != 0) {
              if (string_cmp_ic(reg->enc, case_fold_flag,
                                pstart, &ss, (int )(pend - pstart), send) == 0)
                return 0; /* or goto next_mem; */
            }
            else {
              while (p < pend) {
                if (*p++ != *ss++) return 0; /* or goto next_mem; */
              }
            }

            *s = ss;
            return 1;
          }
        }
      }
      else if (k->type == STK_MEM_END) {
        if (mem_is_in_memp(k->u.mem.num, mem_num, memp)) {
          pend = k->u.mem.pstr;
        }
      }
    }
    k--;
  }

  return 0;
}
#endif /* USE_BACKREF_WITH_LEVEL */


#ifdef ONIG_DEBUG_STATISTICS

#define USE_TIMEOFDAY

#ifdef USE_TIMEOFDAY
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
static struct timeval ts, te;
#define GETTIME(t)        gettimeofday(&(t), (struct timezone* )0)
#define TIMEDIFF(te,ts)   (((te).tv_usec - (ts).tv_usec) + \
                           (((te).tv_sec - (ts).tv_sec)*1000000))
#else
#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif
static struct tms ts, te;
#define GETTIME(t)         times(&(t))
#define TIMEDIFF(te,ts)   ((te).tms_utime - (ts).tms_utime)
#endif

static int OpCounter[256];
static int OpPrevCounter[256];
static unsigned long OpTime[256];
static int OpCurr = OP_FINISH;
static int OpPrevTarget = OP_FAIL;
static int MaxStackDepth = 0;

#define MOP_IN(opcode) do {\
  if (opcode == OpPrevTarget) OpPrevCounter[OpCurr]++;\
  OpCurr = opcode;\
  OpCounter[opcode]++;\
  GETTIME(ts);\
} while(0)

#define MOP_OUT do {\
  GETTIME(te);\
  OpTime[OpCurr] += TIMEDIFF(te, ts);\
} while(0)

extern void
onig_statistics_init(void)
{
  int i;
  for (i = 0; i < 256; i++) {
    OpCounter[i] = OpPrevCounter[i] = 0; OpTime[i] = 0;
  }
  MaxStackDepth = 0;
}

extern void
onig_print_statistics(FILE* f)
{
  int i;
  fprintf(f, "   count      prev        time\n");
  for (i = 0; OnigOpInfo[i].opcode >= 0; i++) {
    fprintf(f, "%8d: %8d: %10ld: %s\n",
            OpCounter[i], OpPrevCounter[i], OpTime[i], OnigOpInfo[i].name);
  }
  fprintf(f, "\nmax stack depth: %d\n", MaxStackDepth);
}

#define STACK_INC do {\
  stk++;\
  if (stk - stk_base > MaxStackDepth) \
    MaxStackDepth = stk - stk_base;\
} while(0)

#else
#define STACK_INC     stk++

#define MOP_IN(opcode)
#define MOP_OUT
#endif


/* matching region of POSIX API */
typedef int regoff_t;

typedef struct {
  regoff_t  rm_so;
  regoff_t  rm_eo;
} posix_regmatch_t;

/* match data(str - end) from position (sstart). */
/* if sstart == str then set sprev to NULL. */
static long
match_at(regex_t* reg, const UChar* str, const UChar* end,
#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
         const UChar* right_range,
#endif
         const UChar* sstart, UChar* sprev, OnigMatchArg* msa)
{
  static const UChar FinishCode[] = { OP_FINISH };

  int i, n, num_mem, best_len, pop_level;
  LengthType tlen, tlen2;
  MemNumType mem;
  RelAddrType addr;
  OnigOptionType option = reg->options;
  OnigEncoding encode = reg->enc;
  OnigCaseFoldType case_fold_flag = reg->case_fold_flag;
  UChar *s, *q, *sbegin;
  UChar *p = reg->p;
  char *alloca_base;
  OnigStackType *stk_alloc, *stk_base, *stk, *stk_end;
  OnigStackType *stkp; /* used as any purpose. */
  OnigStackIndex si;
  OnigStackIndex *repeat_stk;
  OnigStackIndex *mem_start_stk, *mem_end_stk;
#ifdef USE_COMBINATION_EXPLOSION_CHECK
  int scv;
  unsigned char* state_check_buff = msa->state_check_buff;
  int num_comb_exp_check = reg->num_comb_exp_check;
#endif
  n = reg->num_repeat + reg->num_mem * 2;

  STACK_INIT(alloca_base, n, INIT_MATCH_STACK_SIZE);
  pop_level = reg->stack_pop_level;
  num_mem = reg->num_mem;
  repeat_stk = (OnigStackIndex* )alloca_base;

  mem_start_stk = (OnigStackIndex* )(repeat_stk + reg->num_repeat);
  mem_end_stk   = mem_start_stk + num_mem;
  mem_start_stk--; /* for index start from 1,
                      mem_start_stk[1]..mem_start_stk[num_mem] */
  mem_end_stk--;   /* for index start from 1,
                      mem_end_stk[1]..mem_end_stk[num_mem] */
  for (i = 1; i <= num_mem; i++) {
    mem_start_stk[i] = mem_end_stk[i] = INVALID_STACK_INDEX;
  }

#ifdef ONIG_DEBUG_MATCH
  fprintf(stderr, "match_at: str: %d, end: %d, start: %d, sprev: %d\n",
          (int )str, (int )end, (int )sstart, (int )sprev);
  fprintf(stderr, "size: %d, start offset: %d\n",
          (int )(end - str), (int )(sstart - str));
#endif

  STACK_PUSH_ENSURED(STK_ALT, (UChar*)FinishCode);  /* bottom stack */
  best_len = ONIG_MISMATCH;
  s = (UChar*)sstart;
  while (1) {
#ifdef ONIG_DEBUG_MATCH
    if (s) {
      UChar *q, *bp, buf[50];
      int len;
      fprintf(stderr, "%4d> \"", (int )(s - str));
      bp = buf;
      for (i = 0, q = s; i < 7 && q < end; i++) {
        len = enclen(encode, q, end);
        while (len-- > 0) *bp++ = *q++;
      }
      if (q < end) { xmemcpy(bp, "...\"", 4); bp += 4; }
      else         { xmemcpy(bp, "\"",    1); bp += 1; }
      *bp = 0;
      fputs((char* )buf, stderr);
      for (i = 0; i < 20 - (bp - buf); i++) fputc(' ', stderr);
      onig_print_compiled_byte_code(stderr, p, NULL, encode);
      fprintf(stderr, "\n");
    }
#endif

    sbegin = s;
    switch (*p++) {
    case OP_END:  MOP_IN(OP_END);
      n = s - sstart;
      if (n > best_len) {
        OnigRegion* region;
#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
        if (IS_FIND_LONGEST(option)) {
          if (n > msa->best_len) {
            msa->best_len = n;
            msa->best_s   = (UChar* )sstart;
          }
          else
            goto end_best_len;
        }
#endif
        best_len = n;
        region = msa->region;
        if (region) {
            region->beg[0] = sstart - str;
            region->end[0] = s      - str;
            for (i = 1; i <= num_mem; i++) {
              if (mem_end_stk[i] != INVALID_STACK_INDEX) {
                if (BIT_STATUS_AT(reg->bt_mem_start, i))
                  region->beg[i] = STACK_AT(mem_start_stk[i])->u.mem.pstr - str;
                else
                  region->beg[i] = (UChar* )((void* )mem_start_stk[i]) - str;

                region->end[i] = (BIT_STATUS_AT(reg->bt_mem_end, i)
                                  ? STACK_AT(mem_end_stk[i])->u.mem.pstr
                                  : (UChar* )((void* )mem_end_stk[i])) - str;
              }
              else {
                region->beg[i] = region->end[i] = ONIG_REGION_NOTPOS;
              }
            }

#ifdef USE_CAPTURE_HISTORY
            if (reg->capture_history != 0) {
              int r;
              OnigCaptureTreeNode* node;

              if (IS_NULL(region->history_root)) {
                region->history_root = node = history_node_new();
                CHECK_NULL_RETURN_MEMERR(node);
              }
              else {
                node = region->history_root;
                history_tree_clear(node);
              }

              node->group = 0;
              node->beg   = sstart - str;
              node->end   = s      - str;

              stkp = stk_base;
              r = make_capture_history_tree(region->history_root, &stkp,
                                            stk, (UChar* )str, reg);
              if (r < 0) {
                best_len = r; /* error code */
                goto finish;
              }
            }
#endif /* USE_CAPTURE_HISTORY */
        } /* if (region) */
      } /* n > best_len */

#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
    end_best_len:
#endif
      MOP_OUT;

      if (IS_FIND_CONDITION(option)) {
        if (IS_FIND_NOT_EMPTY(option) && s == sstart) {
          best_len = ONIG_MISMATCH;
          goto fail; /* for retry */
        }
        if (IS_FIND_LONGEST(option) && DATA_ENSURE_CHECK1) {
          goto fail; /* for retry */
        }
      }

      /* default behavior: return first-matching result. */
      goto finish;
      break;

    case OP_EXACT1:  MOP_IN(OP_EXACT1);
      if (*p != *s++) goto fail;
      DATA_ENSURE(0);
      p++;
      MOP_OUT;
      break;

    case OP_EXACT1_IC:  MOP_IN(OP_EXACT1_IC);
      {
        int len;
        UChar *q, lowbuf[ONIGENC_MBC_CASE_FOLD_MAXLEN];

        DATA_ENSURE(1);
        len = ONIGENC_MBC_CASE_FOLD(encode,
                    /* DISABLE_CASE_FOLD_MULTI_CHAR(case_fold_flag), */
                    case_fold_flag,
                    &s, end, lowbuf);
        DATA_ENSURE(0);
        q = lowbuf;
        while (len-- > 0) {
          if (*p != *q) {
            goto fail;
          }
          p++; q++;
        }
      }
      MOP_OUT;
      break;

    case OP_EXACT2:  MOP_IN(OP_EXACT2);
      DATA_ENSURE(2);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      sprev = s;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACT3:  MOP_IN(OP_EXACT3);
      DATA_ENSURE(3);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      sprev = s;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACT4:  MOP_IN(OP_EXACT4);
      DATA_ENSURE(4);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      sprev = s;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACT5:  MOP_IN(OP_EXACT5);
      DATA_ENSURE(5);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      sprev = s;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTN:  MOP_IN(OP_EXACTN);
      GET_LENGTH_INC(tlen, p);
      DATA_ENSURE(tlen);
      while (tlen-- > 0) {
        if (*p++ != *s++) goto fail;
      }
      sprev = s - 1;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTN_IC:  MOP_IN(OP_EXACTN_IC);
      {
        int len;
        UChar *q, *endp, lowbuf[ONIGENC_MBC_CASE_FOLD_MAXLEN];

        GET_LENGTH_INC(tlen, p);
        endp = p + tlen;

        while (p < endp) {
          sprev = s;
          DATA_ENSURE(1);
          len = ONIGENC_MBC_CASE_FOLD(encode,
                      /* DISABLE_CASE_FOLD_MULTI_CHAR(case_fold_flag), */
                      case_fold_flag,
                      &s, end, lowbuf);
          DATA_ENSURE(0);
          q = lowbuf;
          while (len-- > 0) {
            if (*p != *q) goto fail;
            p++; q++;
          }
        }
      }

      MOP_OUT;
      continue;
      break;

    case OP_EXACTMB2N1:  MOP_IN(OP_EXACTMB2N1);
      DATA_ENSURE(2);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      MOP_OUT;
      break;

    case OP_EXACTMB2N2:  MOP_IN(OP_EXACTMB2N2);
      DATA_ENSURE(4);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      sprev = s;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTMB2N3:  MOP_IN(OP_EXACTMB2N3);
      DATA_ENSURE(6);
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      sprev = s;
      if (*p != *s) goto fail;
      p++; s++;
      if (*p != *s) goto fail;
      p++; s++;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTMB2N:  MOP_IN(OP_EXACTMB2N);
      GET_LENGTH_INC(tlen, p);
      DATA_ENSURE(tlen * 2);
      while (tlen-- > 0) {
        if (*p != *s) goto fail;
        p++; s++;
        if (*p != *s) goto fail;
        p++; s++;
      }
      sprev = s - 2;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTMB3N:  MOP_IN(OP_EXACTMB3N);
      GET_LENGTH_INC(tlen, p);
      DATA_ENSURE(tlen * 3);
      while (tlen-- > 0) {
        if (*p != *s) goto fail;
        p++; s++;
        if (*p != *s) goto fail;
        p++; s++;
        if (*p != *s) goto fail;
        p++; s++;
      }
      sprev = s - 3;
      MOP_OUT;
      continue;
      break;

    case OP_EXACTMBN:  MOP_IN(OP_EXACTMBN);
      GET_LENGTH_INC(tlen,  p);  /* mb-len */
      GET_LENGTH_INC(tlen2, p);  /* string len */
      tlen2 *= tlen;
      DATA_ENSURE(tlen2);
      while (tlen2-- > 0) {
        if (*p != *s) goto fail;
        p++; s++;
      }
      sprev = s - tlen;
      MOP_OUT;
      continue;
      break;

    case OP_CCLASS:  MOP_IN(OP_CCLASS);
      DATA_ENSURE(1);
      if (BITSET_AT(((BitSetRef )p), *s) == 0) goto fail;
      p += SIZE_BITSET;
      s += enclen(encode, s, end);   /* OP_CCLASS can match mb-code. \D, \S */
      MOP_OUT;
      break;

    case OP_CCLASS_MB:  MOP_IN(OP_CCLASS_MB);
      if (! ONIGENC_IS_MBC_HEAD(encode, s, end)) goto fail;

    cclass_mb:
      GET_LENGTH_INC(tlen, p);
      {
        OnigCodePoint code;
        UChar *ss;
        int mb_len;

        DATA_ENSURE(1);
        mb_len = enclen(encode, s, end);
        DATA_ENSURE(mb_len);
        ss = s;
        s += mb_len;
        code = ONIGENC_MBC_TO_CODE(encode, ss, s);

#ifdef PLATFORM_UNALIGNED_WORD_ACCESS
        if (! onig_is_in_code_range(p, code)) goto fail;
#else
        q = p;
        ALIGNMENT_RIGHT(q);
        if (! onig_is_in_code_range(q, code)) goto fail;
#endif
      }
      p += tlen;
      MOP_OUT;
      break;

    case OP_CCLASS_MIX:  MOP_IN(OP_CCLASS_MIX);
      DATA_ENSURE(1);
      if (ONIGENC_IS_MBC_HEAD(encode, s, end)) {
        p += SIZE_BITSET;
        goto cclass_mb;
      }
      else {
        if (BITSET_AT(((BitSetRef )p), *s) == 0)
          goto fail;

        p += SIZE_BITSET;
        GET_LENGTH_INC(tlen, p);
        p += tlen;
        s++;
      }
      MOP_OUT;
      break;

    case OP_CCLASS_NOT:  MOP_IN(OP_CCLASS_NOT);
      DATA_ENSURE(1);
      if (BITSET_AT(((BitSetRef )p), *s) != 0) goto fail;
      p += SIZE_BITSET;
      s += enclen(encode, s, end);
      MOP_OUT;
      break;

    case OP_CCLASS_MB_NOT:  MOP_IN(OP_CCLASS_MB_NOT);
      DATA_ENSURE(1);
      if (! ONIGENC_IS_MBC_HEAD(encode, s, end)) {
        s++;
        GET_LENGTH_INC(tlen, p);
        p += tlen;
        goto cc_mb_not_success;
      }

    cclass_mb_not:
      GET_LENGTH_INC(tlen, p);
      {
        OnigCodePoint code;
        UChar *ss;
        int mb_len = enclen(encode, s, end);

        if (! DATA_ENSURE_CHECK(mb_len)) {
          DATA_ENSURE(1);
          s = (UChar* )end;
          p += tlen;
          goto cc_mb_not_success;
        }

        ss = s;
        s += mb_len;
        code = ONIGENC_MBC_TO_CODE(encode, ss, s);

#ifdef PLATFORM_UNALIGNED_WORD_ACCESS
        if (onig_is_in_code_range(p, code)) goto fail;
#else
        q = p;
        ALIGNMENT_RIGHT(q);
        if (onig_is_in_code_range(q, code)) goto fail;
#endif
      }
      p += tlen;

    cc_mb_not_success:
      MOP_OUT;
      break;

    case OP_CCLASS_MIX_NOT:  MOP_IN(OP_CCLASS_MIX_NOT);
      DATA_ENSURE(1);
      if (ONIGENC_IS_MBC_HEAD(encode, s, end)) {
        p += SIZE_BITSET;
        goto cclass_mb_not;
      }
      else {
        if (BITSET_AT(((BitSetRef )p), *s) != 0)
          goto fail;

        p += SIZE_BITSET;
        GET_LENGTH_INC(tlen, p);
        p += tlen;
        s++;
      }
      MOP_OUT;
      break;

    case OP_CCLASS_NODE:  MOP_IN(OP_CCLASS_NODE);
      {
        OnigCodePoint code;
        void *node;
        int mb_len;
        UChar *ss;

        DATA_ENSURE(1);
        GET_POINTER_INC(node, p);
        mb_len = enclen(encode, s, end);
        ss = s;
        s += mb_len;
        DATA_ENSURE(0);
        code = ONIGENC_MBC_TO_CODE(encode, ss, s);
        if (onig_is_code_in_cc_len(mb_len, code, node) == 0) goto fail;
      }
      MOP_OUT;
      break;

    case OP_ANYCHAR:  MOP_IN(OP_ANYCHAR);
      DATA_ENSURE(1);
      n = enclen(encode, s, end);
      DATA_ENSURE(n);
      if (ONIGENC_IS_MBC_NEWLINE(encode, s, end)) goto fail;
      s += n;
      MOP_OUT;
      break;

    case OP_ANYCHAR_ML:  MOP_IN(OP_ANYCHAR_ML);
      DATA_ENSURE(1);
      n = enclen(encode, s, end);
      DATA_ENSURE(n);
      s += n;
      MOP_OUT;
      break;

    case OP_ANYCHAR_STAR:  MOP_IN(OP_ANYCHAR_STAR);
      while (DATA_ENSURE_CHECK1) {
        STACK_PUSH_ALT(p, s, sprev);
        n = enclen(encode, s, end);
        DATA_ENSURE(n);
        if (ONIGENC_IS_MBC_NEWLINE(encode, s, end))  goto fail;
        sprev = s;
        s += n;
      }
      MOP_OUT;
      break;

    case OP_ANYCHAR_ML_STAR:  MOP_IN(OP_ANYCHAR_ML_STAR);
      while (DATA_ENSURE_CHECK1) {
        STACK_PUSH_ALT(p, s, sprev);
        n = enclen(encode, s, end);
        if (n > 1) {
          DATA_ENSURE(n);
          sprev = s;
          s += n;
        }
        else {
          sprev = s;
          s++;
        }
      }
      MOP_OUT;
      break;

    case OP_ANYCHAR_STAR_PEEK_NEXT:  MOP_IN(OP_ANYCHAR_STAR_PEEK_NEXT);
      while (DATA_ENSURE_CHECK1) {
        if (*p == *s) {
          STACK_PUSH_ALT(p + 1, s, sprev);
        }
        n = enclen(encode, s, end);
        DATA_ENSURE(n);
        if (ONIGENC_IS_MBC_NEWLINE(encode, s, end))  goto fail;
        sprev = s;
        s += n;
      }
      p++;
      MOP_OUT;
      break;

    case OP_ANYCHAR_ML_STAR_PEEK_NEXT:MOP_IN(OP_ANYCHAR_ML_STAR_PEEK_NEXT);
      while (DATA_ENSURE_CHECK1) {
        if (*p == *s) {
          STACK_PUSH_ALT(p + 1, s, sprev);
        }
        n = enclen(encode, s, end);
        if (n > 1) {
          DATA_ENSURE(n);
          sprev = s;
          s += n;
        }
        else {
          sprev = s;
          s++;
        }
      }
      p++;
      MOP_OUT;
      break;

#ifdef USE_COMBINATION_EXPLOSION_CHECK
    case OP_STATE_CHECK_ANYCHAR_STAR:  MOP_IN(OP_STATE_CHECK_ANYCHAR_STAR);
      GET_STATE_CHECK_NUM_INC(mem, p);
      while (DATA_ENSURE_CHECK1) {
        STATE_CHECK_VAL(scv, mem);
        if (scv) goto fail;

        STACK_PUSH_ALT_WITH_STATE_CHECK(p, s, sprev, mem);
        n = enclen(encode, s, end);
        DATA_ENSURE(n);
        if (ONIGENC_IS_MBC_NEWLINE(encode, s, end))  goto fail;
        sprev = s;
        s += n;
      }
      MOP_OUT;
      break;

    case OP_STATE_CHECK_ANYCHAR_ML_STAR:
      MOP_IN(OP_STATE_CHECK_ANYCHAR_ML_STAR);

      GET_STATE_CHECK_NUM_INC(mem, p);
      while (DATA_ENSURE_CHECK1) {
        STATE_CHECK_VAL(scv, mem);
        if (scv) goto fail;

        STACK_PUSH_ALT_WITH_STATE_CHECK(p, s, sprev, mem);
        n = enclen(encode, s, end);
        if (n > 1) {
          DATA_ENSURE(n);
          sprev = s;
          s += n;
        }
        else {
          sprev = s;
          s++;
        }
      }
      MOP_OUT;
      break;
#endif /* USE_COMBINATION_EXPLOSION_CHECK */

    case OP_WORD:  MOP_IN(OP_WORD);
      DATA_ENSURE(1);
      if (! ONIGENC_IS_MBC_WORD(encode, s, end))
        goto fail;

      s += enclen(encode, s, end);
      MOP_OUT;
      break;

    case OP_NOT_WORD:  MOP_IN(OP_NOT_WORD);
      DATA_ENSURE(1);
      if (ONIGENC_IS_MBC_WORD(encode, s, end))
        goto fail;

      s += enclen(encode, s, end);
      MOP_OUT;
      break;

    case OP_WORD_BOUND:  MOP_IN(OP_WORD_BOUND);
      if (ON_STR_BEGIN(s)) {
        DATA_ENSURE(1);
        if (! ONIGENC_IS_MBC_WORD(encode, s, end))
          goto fail;
      }
      else if (ON_STR_END(s)) {
        if (! ONIGENC_IS_MBC_WORD(encode, sprev, end))
          goto fail;
      }
      else {
        if (ONIGENC_IS_MBC_WORD(encode, s, end)
            == ONIGENC_IS_MBC_WORD(encode, sprev, end))
          goto fail;
      }
      MOP_OUT;
      continue;
      break;

    case OP_NOT_WORD_BOUND:  MOP_IN(OP_NOT_WORD_BOUND);
      if (ON_STR_BEGIN(s)) {
        if (DATA_ENSURE_CHECK1 && ONIGENC_IS_MBC_WORD(encode, s, end))
          goto fail;
      }
      else if (ON_STR_END(s)) {
        if (ONIGENC_IS_MBC_WORD(encode, sprev, end))
          goto fail;
      }
      else {
        if (ONIGENC_IS_MBC_WORD(encode, s, end)
            != ONIGENC_IS_MBC_WORD(encode, sprev, end))
          goto fail;
      }
      MOP_OUT;
      continue;
      break;

#ifdef USE_WORD_BEGIN_END
    case OP_WORD_BEGIN:  MOP_IN(OP_WORD_BEGIN);
      if (DATA_ENSURE_CHECK1 && ONIGENC_IS_MBC_WORD(encode, s, end)) {
        if (ON_STR_BEGIN(s) || !ONIGENC_IS_MBC_WORD(encode, sprev, end)) {
          MOP_OUT;
          continue;
        }
      }
      goto fail;
      break;

    case OP_WORD_END:  MOP_IN(OP_WORD_END);
      if (!ON_STR_BEGIN(s) && ONIGENC_IS_MBC_WORD(encode, sprev, end)) {
        if (ON_STR_END(s) || !ONIGENC_IS_MBC_WORD(encode, s, end)) {
          MOP_OUT;
          continue;
        }
      }
      goto fail;
      break;
#endif

    case OP_BEGIN_BUF:  MOP_IN(OP_BEGIN_BUF);
      if (! ON_STR_BEGIN(s)) goto fail;

      MOP_OUT;
      continue;
      break;

    case OP_END_BUF:  MOP_IN(OP_END_BUF);
      if (! ON_STR_END(s)) goto fail;

      MOP_OUT;
      continue;
      break;

    case OP_BEGIN_LINE:  MOP_IN(OP_BEGIN_LINE);
      if (ON_STR_BEGIN(s)) {
        if (IS_NOTBOL(msa->options)) goto fail;
        MOP_OUT;
        continue;
      }
      else if (ONIGENC_IS_MBC_NEWLINE(encode, sprev, end) && !ON_STR_END(s)) {
        MOP_OUT;
        continue;
      }
      goto fail;
      break;

    case OP_END_LINE:  MOP_IN(OP_END_LINE);
      if (ON_STR_END(s)) {
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
        if (IS_EMPTY_STR || !ONIGENC_IS_MBC_NEWLINE(encode, sprev, end)) {
#endif
          if (IS_NOTEOL(msa->options)) goto fail;
          MOP_OUT;
          continue;
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
        }
#endif
      }
      else if (ONIGENC_IS_MBC_NEWLINE(encode, s, end)) {
        MOP_OUT;
        continue;
      }
#ifdef USE_CRNL_AS_LINE_TERMINATOR
      else if (ONIGENC_IS_MBC_CRNL(encode, s, end)) {
        MOP_OUT;
        continue;
      }
#endif
      goto fail;
      break;

    case OP_SEMI_END_BUF:  MOP_IN(OP_SEMI_END_BUF);
      if (ON_STR_END(s)) {
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
        if (IS_EMPTY_STR || !ONIGENC_IS_MBC_NEWLINE(encode, sprev, end)) {
#endif
          if (IS_NOTEOL(msa->options)) goto fail;
          MOP_OUT;
          continue;
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
        }
#endif
      }
      else if (ONIGENC_IS_MBC_NEWLINE(encode, s, end) &&
               ON_STR_END(s + enclen(encode, s, end))) {
        MOP_OUT;
        continue;
      }
#ifdef USE_CRNL_AS_LINE_TERMINATOR
      else if (ONIGENC_IS_MBC_CRNL(encode, s, end)) {
        UChar* ss = s + enclen(encode, s);
        ss += enclen(encode, ss);
        if (ON_STR_END(ss)) {
          MOP_OUT;
          continue;
        }
      }
#endif
      goto fail;
      break;

    case OP_BEGIN_POSITION:  MOP_IN(OP_BEGIN_POSITION);
      if (s != msa->start)
        goto fail;

      MOP_OUT;
      continue;
      break;

    case OP_MEMORY_START_PUSH:  MOP_IN(OP_MEMORY_START_PUSH);
      GET_MEMNUM_INC(mem, p);
      STACK_PUSH_MEM_START(mem, s);
      MOP_OUT;
      continue;
      break;

    case OP_MEMORY_START:  MOP_IN(OP_MEMORY_START);
      GET_MEMNUM_INC(mem, p);
      mem_start_stk[mem] = (OnigStackIndex )((void* )s);
      MOP_OUT;
      continue;
      break;

    case OP_MEMORY_END_PUSH:  MOP_IN(OP_MEMORY_END_PUSH);
      GET_MEMNUM_INC(mem, p);
      STACK_PUSH_MEM_END(mem, s);
      MOP_OUT;
      continue;
      break;

    case OP_MEMORY_END:  MOP_IN(OP_MEMORY_END);
      GET_MEMNUM_INC(mem, p);
      mem_end_stk[mem] = (OnigStackIndex )((void* )s);
      MOP_OUT;
      continue;
      break;

#ifdef USE_SUBEXP_CALL
    case OP_MEMORY_END_PUSH_REC:  MOP_IN(OP_MEMORY_END_PUSH_REC);
      GET_MEMNUM_INC(mem, p);
      STACK_GET_MEM_START(mem, stkp); /* should be before push mem-end. */
      STACK_PUSH_MEM_END(mem, s);
      mem_start_stk[mem] = GET_STACK_INDEX(stkp);
      MOP_OUT;
      continue;
      break;

    case OP_MEMORY_END_REC:  MOP_IN(OP_MEMORY_END_REC);
      GET_MEMNUM_INC(mem, p);
      mem_end_stk[mem] = (OnigStackIndex )((void* )s);
      STACK_GET_MEM_START(mem, stkp);

      if (BIT_STATUS_AT(reg->bt_mem_start, mem))
        mem_start_stk[mem] = GET_STACK_INDEX(stkp);
      else
        mem_start_stk[mem] = (OnigStackIndex )((void* )stkp->u.mem.pstr);

      STACK_PUSH_MEM_END_MARK(mem);
      MOP_OUT;
      continue;
      break;
#endif

    case OP_BACKREF1:  MOP_IN(OP_BACKREF1);
      mem = 1;
      goto backref;
      break;

    case OP_BACKREF2:  MOP_IN(OP_BACKREF2);
      mem = 2;
      goto backref;
      break;

    case OP_BACKREFN:  MOP_IN(OP_BACKREFN);
      GET_MEMNUM_INC(mem, p);
    backref:
      {
        int len;
        UChar *pstart, *pend;

        /* if you want to remove following line,
           you should check in parse and compile time. */
        if (mem > num_mem) goto fail;
        if (mem_end_stk[mem]   == INVALID_STACK_INDEX) goto fail;
        if (mem_start_stk[mem] == INVALID_STACK_INDEX) goto fail;

        if (BIT_STATUS_AT(reg->bt_mem_start, mem))
          pstart = STACK_AT(mem_start_stk[mem])->u.mem.pstr;
        else
          pstart = (UChar* )((void* )mem_start_stk[mem]);

        pend = (BIT_STATUS_AT(reg->bt_mem_end, mem)
                ? STACK_AT(mem_end_stk[mem])->u.mem.pstr
                : (UChar* )((void* )mem_end_stk[mem]));
        n = pend - pstart;
        DATA_ENSURE(n);
        sprev = s;
        STRING_CMP(pstart, s, n);
        while (sprev + (len = enclen(encode, sprev, end)) < s)
          sprev += len;

        MOP_OUT;
        continue;
      }
      break;

    case OP_BACKREFN_IC:  MOP_IN(OP_BACKREFN_IC);
      GET_MEMNUM_INC(mem, p);
      {
        int len;
        UChar *pstart, *pend;

        /* if you want to remove following line,
           you should check in parse and compile time. */
        if (mem > num_mem) goto fail;
        if (mem_end_stk[mem]   == INVALID_STACK_INDEX) goto fail;
        if (mem_start_stk[mem] == INVALID_STACK_INDEX) goto fail;

        if (BIT_STATUS_AT(reg->bt_mem_start, mem))
          pstart = STACK_AT(mem_start_stk[mem])->u.mem.pstr;
        else
          pstart = (UChar* )((void* )mem_start_stk[mem]);

        pend = (BIT_STATUS_AT(reg->bt_mem_end, mem)
                ? STACK_AT(mem_end_stk[mem])->u.mem.pstr
                : (UChar* )((void* )mem_end_stk[mem]));
        n = pend - pstart;
        DATA_ENSURE(n);
        sprev = s;
        STRING_CMP_IC(case_fold_flag, pstart, &s, n, end);
        while (sprev + (len = enclen(encode, sprev, end)) < s)
          sprev += len;

        MOP_OUT;
        continue;
      }
      break;

    case OP_BACKREF_MULTI:  MOP_IN(OP_BACKREF_MULTI);
      {
        int len, is_fail;
        UChar *pstart, *pend, *swork;

        GET_LENGTH_INC(tlen, p);
        for (i = 0; i < tlen; i++) {
          GET_MEMNUM_INC(mem, p);

          if (mem_end_stk[mem]   == INVALID_STACK_INDEX) continue;
          if (mem_start_stk[mem] == INVALID_STACK_INDEX) continue;

          if (BIT_STATUS_AT(reg->bt_mem_start, mem))
            pstart = STACK_AT(mem_start_stk[mem])->u.mem.pstr;
          else
            pstart = (UChar* )((void* )mem_start_stk[mem]);

          pend = (BIT_STATUS_AT(reg->bt_mem_end, mem)
                  ? STACK_AT(mem_end_stk[mem])->u.mem.pstr
                  : (UChar* )((void* )mem_end_stk[mem]));
          n = pend - pstart;
          DATA_ENSURE(n);
          sprev = s;
          swork = s;
          STRING_CMP_VALUE(pstart, swork, n, is_fail);
          if (is_fail) continue;
          s = swork;
          while (sprev + (len = enclen(encode, sprev, end)) < s)
            sprev += len;

          p += (SIZE_MEMNUM * (tlen - i - 1));
          break; /* success */
        }
        if (i == tlen) goto fail;
        MOP_OUT;
        continue;
      }
      break;

    case OP_BACKREF_MULTI_IC:  MOP_IN(OP_BACKREF_MULTI_IC);
      {
        int len, is_fail;
        UChar *pstart, *pend, *swork;

        GET_LENGTH_INC(tlen, p);
        for (i = 0; i < tlen; i++) {
          GET_MEMNUM_INC(mem, p);

          if (mem_end_stk[mem]   == INVALID_STACK_INDEX) continue;
          if (mem_start_stk[mem] == INVALID_STACK_INDEX) continue;

          if (BIT_STATUS_AT(reg->bt_mem_start, mem))
            pstart = STACK_AT(mem_start_stk[mem])->u.mem.pstr;
          else
            pstart = (UChar* )((void* )mem_start_stk[mem]);

          pend = (BIT_STATUS_AT(reg->bt_mem_end, mem)
                  ? STACK_AT(mem_end_stk[mem])->u.mem.pstr
                  : (UChar* )((void* )mem_end_stk[mem]));
          n = pend - pstart;
          DATA_ENSURE(n);
          sprev = s;
          swork = s;
          STRING_CMP_VALUE_IC(case_fold_flag, pstart, &swork, n, end, is_fail);
          if (is_fail) continue;
          s = swork;
          while (sprev + (len = enclen(encode, sprev, end)) < s)
            sprev += len;

          p += (SIZE_MEMNUM * (tlen - i - 1));
          break; /* success */
        }
        if (i == tlen) goto fail;
        MOP_OUT;
        continue;
      }
      break;

#ifdef USE_BACKREF_WITH_LEVEL
    case OP_BACKREF_WITH_LEVEL:
      {
        int len;
        OnigOptionType ic;
        LengthType level;

        GET_OPTION_INC(ic,    p);
        GET_LENGTH_INC(level, p);
        GET_LENGTH_INC(tlen,  p);

        sprev = s;
        if (backref_match_at_nested_level(reg, stk, stk_base, ic
                  , case_fold_flag, (int )level, (int )tlen, p, &s, end)) {
          while (sprev + (len = enclen(encode, sprev, end)) < s)
            sprev += len;

          p += (SIZE_MEMNUM * tlen);
        }
        else
          goto fail;

        MOP_OUT;
        continue;
      }

      break;
#endif

    case OP_NULL_CHECK_START:  MOP_IN(OP_NULL_CHECK_START);
      GET_MEMNUM_INC(mem, p);    /* mem: null check id */
      STACK_PUSH_NULL_CHECK_START(mem, s);
      MOP_OUT;
      continue;
      break;

    case OP_NULL_CHECK_END:  MOP_IN(OP_NULL_CHECK_END);
      {
        int isnull;

        GET_MEMNUM_INC(mem, p); /* mem: null check id */
        STACK_NULL_CHECK(isnull, mem, s);
        if (isnull) {
#ifdef ONIG_DEBUG_MATCH
          fprintf(stderr, "NULL_CHECK_END: skip  id:%d, s:%d\n",
                  (int )mem, (int )s);
#endif
        null_check_found:
          /* empty loop founded, skip next instruction */
          switch (*p++) {
          case OP_JUMP:
          case OP_PUSH:
            p += SIZE_RELADDR;
            break;
          case OP_REPEAT_INC:
          case OP_REPEAT_INC_NG:
          case OP_REPEAT_INC_SG:
          case OP_REPEAT_INC_NG_SG:
            p += SIZE_MEMNUM;
            break;
          default:
            goto unexpected_bytecode_error;
            break;
          }
        }
      }
      MOP_OUT;
      continue;
      break;

#ifdef USE_MONOMANIAC_CHECK_CAPTURES_IN_ENDLESS_REPEAT
    case OP_NULL_CHECK_END_MEMST:  MOP_IN(OP_NULL_CHECK_END_MEMST);
      {
        int isnull;

        GET_MEMNUM_INC(mem, p); /* mem: null check id */
        STACK_NULL_CHECK_MEMST(isnull, mem, s, reg);
        if (isnull) {
#ifdef ONIG_DEBUG_MATCH
          fprintf(stderr, "NULL_CHECK_END_MEMST: skip  id:%d, s:%d\n",
                  (int )mem, (int )s);
#endif
          if (isnull == -1) goto fail;
          goto  null_check_found;
        }
      }
      MOP_OUT;
      continue;
      break;
#endif

#ifdef USE_SUBEXP_CALL
    case OP_NULL_CHECK_END_MEMST_PUSH:
      MOP_IN(OP_NULL_CHECK_END_MEMST_PUSH);
      {
        int isnull;

        GET_MEMNUM_INC(mem, p); /* mem: null check id */
#ifdef USE_MONOMANIAC_CHECK_CAPTURES_IN_ENDLESS_REPEAT
        STACK_NULL_CHECK_MEMST_REC(isnull, mem, s, reg);
#else
        STACK_NULL_CHECK_REC(isnull, mem, s);
#endif
        if (isnull) {
#ifdef ONIG_DEBUG_MATCH
          fprintf(stderr, "NULL_CHECK_END_MEMST_PUSH: skip  id:%d, s:%d\n",
                  (int )mem, (int )s);
#endif
          if (isnull == -1) goto fail;
          goto  null_check_found;
        }
        else {
          STACK_PUSH_NULL_CHECK_END(mem);
        }
      }
      MOP_OUT;
      continue;
      break;
#endif

    case OP_JUMP:  MOP_IN(OP_JUMP);
      GET_RELADDR_INC(addr, p);
      p += addr;
      MOP_OUT;
      CHECK_INTERRUPT_IN_MATCH_AT;
      continue;
      break;

    case OP_PUSH:  MOP_IN(OP_PUSH);
      GET_RELADDR_INC(addr, p);
      STACK_PUSH_ALT(p + addr, s, sprev);
      MOP_OUT;
      continue;
      break;

#ifdef USE_COMBINATION_EXPLOSION_CHECK
    case OP_STATE_CHECK_PUSH:  MOP_IN(OP_STATE_CHECK_PUSH);
      GET_STATE_CHECK_NUM_INC(mem, p);
      STATE_CHECK_VAL(scv, mem);
      if (scv) goto fail;

      GET_RELADDR_INC(addr, p);
      STACK_PUSH_ALT_WITH_STATE_CHECK(p + addr, s, sprev, mem);
      MOP_OUT;
      continue;
      break;

    case OP_STATE_CHECK_PUSH_OR_JUMP:  MOP_IN(OP_STATE_CHECK_PUSH_OR_JUMP);
      GET_STATE_CHECK_NUM_INC(mem, p);
      GET_RELADDR_INC(addr, p);
      STATE_CHECK_VAL(scv, mem);
      if (scv) {
        p += addr;
      }
      else {
        STACK_PUSH_ALT_WITH_STATE_CHECK(p + addr, s, sprev, mem);
      }
      MOP_OUT;
      continue;
      break;

    case OP_STATE_CHECK:  MOP_IN(OP_STATE_CHECK);
      GET_STATE_CHECK_NUM_INC(mem, p);
      STATE_CHECK_VAL(scv, mem);
      if (scv) goto fail;

      STACK_PUSH_STATE_CHECK(s, mem);
      MOP_OUT;
      continue;
      break;
#endif /* USE_COMBINATION_EXPLOSION_CHECK */

    case OP_POP:  MOP_IN(OP_POP);
      STACK_POP_ONE;
      MOP_OUT;
      continue;
      break;

    case OP_PUSH_OR_JUMP_EXACT1:  MOP_IN(OP_PUSH_OR_JUMP_EXACT1);
      GET_RELADDR_INC(addr, p);
      if (*p == *s && DATA_ENSURE_CHECK1) {
        p++;
        STACK_PUSH_ALT(p + addr, s, sprev);
        MOP_OUT;
        continue;
      }
      p += (addr + 1);
      MOP_OUT;
      continue;
      break;

    case OP_PUSH_IF_PEEK_NEXT:  MOP_IN(OP_PUSH_IF_PEEK_NEXT);
      GET_RELADDR_INC(addr, p);
      if (*p == *s) {
        p++;
        STACK_PUSH_ALT(p + addr, s, sprev);
        MOP_OUT;
        continue;
      }
      p++;
      MOP_OUT;
      continue;
      break;

    case OP_REPEAT:  MOP_IN(OP_REPEAT);
      {
        GET_MEMNUM_INC(mem, p);    /* mem: OP_REPEAT ID */
        GET_RELADDR_INC(addr, p);

        STACK_ENSURE(1);
        repeat_stk[mem] = GET_STACK_INDEX(stk);
        STACK_PUSH_REPEAT(mem, p);

        if (reg->repeat_range[mem].lower == 0) {
          STACK_PUSH_ALT(p + addr, s, sprev);
        }
      }
      MOP_OUT;
      continue;
      break;

    case OP_REPEAT_NG:  MOP_IN(OP_REPEAT_NG);
      {
        GET_MEMNUM_INC(mem, p);    /* mem: OP_REPEAT ID */
        GET_RELADDR_INC(addr, p);

        STACK_ENSURE(1);
        repeat_stk[mem] = GET_STACK_INDEX(stk);
        STACK_PUSH_REPEAT(mem, p);

        if (reg->repeat_range[mem].lower == 0) {
          STACK_PUSH_ALT(p, s, sprev);
          p += addr;
        }
      }
      MOP_OUT;
      continue;
      break;

    case OP_REPEAT_INC:  MOP_IN(OP_REPEAT_INC);
      GET_MEMNUM_INC(mem, p); /* mem: OP_REPEAT ID */
      si = repeat_stk[mem];
      stkp = STACK_AT(si);

    repeat_inc:
      stkp->u.repeat.count++;
      if (stkp->u.repeat.count >= reg->repeat_range[mem].upper) {
        /* end of repeat. Nothing to do. */
      }
      else if (stkp->u.repeat.count >= reg->repeat_range[mem].lower) {
        STACK_PUSH_ALT(p, s, sprev);
        p = STACK_AT(si)->u.repeat.pcode; /* Don't use stkp after PUSH. */
      }
      else {
        p = stkp->u.repeat.pcode;
      }
      STACK_PUSH_REPEAT_INC(si);
      MOP_OUT;
      CHECK_INTERRUPT_IN_MATCH_AT;
      continue;
      break;

    case OP_REPEAT_INC_SG:  MOP_IN(OP_REPEAT_INC_SG);
      GET_MEMNUM_INC(mem, p); /* mem: OP_REPEAT ID */
      STACK_GET_REPEAT(mem, stkp);
      si = GET_STACK_INDEX(stkp);
      goto repeat_inc;
      break;

    case OP_REPEAT_INC_NG:  MOP_IN(OP_REPEAT_INC_NG);
      GET_MEMNUM_INC(mem, p); /* mem: OP_REPEAT ID */
      si = repeat_stk[mem];
      stkp = STACK_AT(si);

    repeat_inc_ng:
      stkp->u.repeat.count++;
      if (stkp->u.repeat.count < reg->repeat_range[mem].upper) {
        if (stkp->u.repeat.count >= reg->repeat_range[mem].lower) {
          UChar* pcode = stkp->u.repeat.pcode;

          STACK_PUSH_REPEAT_INC(si);
          STACK_PUSH_ALT(pcode, s, sprev);
        }
        else {
          p = stkp->u.repeat.pcode;
          STACK_PUSH_REPEAT_INC(si);
        }
      }
      else if (stkp->u.repeat.count == reg->repeat_range[mem].upper) {
        STACK_PUSH_REPEAT_INC(si);
      }
      MOP_OUT;
      CHECK_INTERRUPT_IN_MATCH_AT;
      continue;
      break;

    case OP_REPEAT_INC_NG_SG:  MOP_IN(OP_REPEAT_INC_NG_SG);
      GET_MEMNUM_INC(mem, p); /* mem: OP_REPEAT ID */
      STACK_GET_REPEAT(mem, stkp);
      si = GET_STACK_INDEX(stkp);
      goto repeat_inc_ng;
      break;

    case OP_PUSH_POS:  MOP_IN(OP_PUSH_POS);
      STACK_PUSH_POS(s, sprev);
      MOP_OUT;
      continue;
      break;

    case OP_POP_POS:  MOP_IN(OP_POP_POS);
      {
        STACK_POS_END(stkp);
        s     = stkp->u.state.pstr;
        sprev = stkp->u.state.pstr_prev;
      }
      MOP_OUT;
      continue;
      break;

    case OP_PUSH_POS_NOT:  MOP_IN(OP_PUSH_POS_NOT);
      GET_RELADDR_INC(addr, p);
      STACK_PUSH_POS_NOT(p + addr, s, sprev);
      MOP_OUT;
      continue;
      break;

    case OP_FAIL_POS:  MOP_IN(OP_FAIL_POS);
      STACK_POP_TIL_POS_NOT;
      goto fail;
      break;

    case OP_PUSH_STOP_BT:  MOP_IN(OP_PUSH_STOP_BT);
      STACK_PUSH_STOP_BT;
      MOP_OUT;
      continue;
      break;

    case OP_POP_STOP_BT:  MOP_IN(OP_POP_STOP_BT);
      STACK_STOP_BT_END;
      MOP_OUT;
      continue;
      break;

    case OP_LOOK_BEHIND:  MOP_IN(OP_LOOK_BEHIND);
      GET_LENGTH_INC(tlen, p);
      s = (UChar* )ONIGENC_STEP_BACK(encode, str, s, end, (int )tlen);
      if (IS_NULL(s)) goto fail;
      sprev = (UChar* )onigenc_get_prev_char_head(encode, str, s, end);
      MOP_OUT;
      continue;
      break;

    case OP_PUSH_LOOK_BEHIND_NOT:  MOP_IN(OP_PUSH_LOOK_BEHIND_NOT);
      GET_RELADDR_INC(addr, p);
      GET_LENGTH_INC(tlen, p);
      q = (UChar* )ONIGENC_STEP_BACK(encode, str, s, end, (int )tlen);
      if (IS_NULL(q)) {
        /* too short case -> success. ex. /(?<!XXX)a/.match("a")
           If you want to change to fail, replace following line. */
        p += addr;
        /* goto fail; */
      }
      else {
        STACK_PUSH_LOOK_BEHIND_NOT(p + addr, s, sprev);
        s = q;
        sprev = (UChar* )onigenc_get_prev_char_head(encode, str, s, end);
      }
      MOP_OUT;
      continue;
      break;

    case OP_FAIL_LOOK_BEHIND_NOT:  MOP_IN(OP_FAIL_LOOK_BEHIND_NOT);
      STACK_POP_TIL_LOOK_BEHIND_NOT;
      goto fail;
      break;

#ifdef USE_SUBEXP_CALL
    case OP_CALL:  MOP_IN(OP_CALL);
      GET_ABSADDR_INC(addr, p);
      STACK_PUSH_CALL_FRAME(p);
      p = reg->p + addr;
      MOP_OUT;
      continue;
      break;

    case OP_RETURN:  MOP_IN(OP_RETURN);
      STACK_RETURN(p);
      STACK_PUSH_RETURN;
      MOP_OUT;
      continue;
      break;
#endif

    case OP_FINISH:
      goto finish;
      break;

    fail:
      MOP_OUT;
      /* fall */
    case OP_FAIL:  MOP_IN(OP_FAIL);
      STACK_POP;
      p     = stk->u.state.pcode;
      s     = stk->u.state.pstr;
      sprev = stk->u.state.pstr_prev;

#ifdef USE_COMBINATION_EXPLOSION_CHECK
      if (stk->u.state.state_check != 0) {
        stk->type = STK_STATE_CHECK_MARK;
        stk++;
      }
#endif

      MOP_OUT;
      continue;
      break;

    default:
      goto bytecode_error;

    } /* end of switch */
    sprev = sbegin;
  } /* end of while(1) */

 finish:
  STACK_SAVE;
  return best_len;

#ifdef ONIG_DEBUG
 stack_error:
  STACK_SAVE;
  return ONIGERR_STACK_BUG;
#endif

 bytecode_error:
  STACK_SAVE;
  return ONIGERR_UNDEFINED_BYTECODE;

 unexpected_bytecode_error:
  STACK_SAVE;
  return ONIGERR_UNEXPECTED_BYTECODE;
}


static UChar*
slow_search(OnigEncoding enc, UChar* target, UChar* target_end,
            const UChar* text, const UChar* text_end, UChar* text_range)
{
  UChar *t, *p, *s, *end;

  end = (UChar* )text_end;
  end -= target_end - target - 1;
  if (end > text_range)
    end = text_range;

  s = (UChar* )text;

  if (enc->max_enc_len == enc->min_enc_len) {
    int n = enc->max_enc_len;

    while (s < end) {
      if (*s == *target) {
        p = s + 1;
        t = target + 1;
        if (target_end == t || memcmp(t, p, target_end - t) == 0)
          return s;
      }
      s += n;
    }
    return (UChar*)NULL;
  }
  while (s < end) {
    if (*s == *target) {
      p = s + 1;
      t = target + 1;
      if (target_end == t || memcmp(t, p, target_end - t) == 0)
        return s;
    }
    s += enclen(enc, s, text_end);
  }

  return (UChar* )NULL;
}

static int
str_lower_case_match(OnigEncoding enc, int case_fold_flag,
                     const UChar* t, const UChar* tend,
                     const UChar* p, const UChar* end)
{
  int lowlen;
  UChar *q, lowbuf[ONIGENC_MBC_CASE_FOLD_MAXLEN];

  while (t < tend) {
    lowlen = ONIGENC_MBC_CASE_FOLD(enc, case_fold_flag, &p, end, lowbuf);
    q = lowbuf;
    while (lowlen > 0) {
      if (*t++ != *q++) return 0;
      lowlen--;
    }
  }

  return 1;
}

static UChar*
slow_search_ic(OnigEncoding enc, int case_fold_flag,
               UChar* target, UChar* target_end,
               const UChar* text, const UChar* text_end, UChar* text_range)
{
  UChar *s, *end;

  end = (UChar* )text_end;
  end -= target_end - target - 1;
  if (end > text_range)
    end = text_range;

  s = (UChar* )text;

  while (s < end) {
    if (str_lower_case_match(enc, case_fold_flag, target, target_end,
                             s, text_end))
      return s;

    s += enclen(enc, s, text_end);
  }

  return (UChar* )NULL;
}

static UChar*
slow_search_backward(OnigEncoding enc, UChar* target, UChar* target_end,
                     const UChar* text, const UChar* adjust_text,
                     const UChar* text_end, const UChar* text_start)
{
  UChar *t, *p, *s;

  s = (UChar* )text_end;
  s -= (target_end - target);
  if (s > text_start)
    s = (UChar* )text_start;
  else
    s = ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, adjust_text, s, text_end);

  while (s >= text) {
    if (*s == *target) {
      p = s + 1;
      t = target + 1;
      while (t < target_end) {
        if (*t != *p++)
          break;
        t++;
      }
      if (t == target_end)
        return s;
    }
    s = (UChar* )onigenc_get_prev_char_head(enc, adjust_text, s, text_end);
  }

  return (UChar* )NULL;
}

static UChar*
slow_search_backward_ic(OnigEncoding enc, int case_fold_flag,
                        UChar* target, UChar* target_end,
                        const UChar* text, const UChar* adjust_text,
                        const UChar* text_end, const UChar* text_start)
{
  UChar *s;

  s = (UChar* )text_end;
  s -= (target_end - target);
  if (s > text_start)
    s = (UChar* )text_start;
  else
    s = ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, adjust_text, s, text_end);

  while (s >= text) {
    if (str_lower_case_match(enc, case_fold_flag,
                             target, target_end, s, text_end))
      return s;

    s = (UChar* )onigenc_get_prev_char_head(enc, adjust_text, s, text_end);
  }

  return (UChar* )NULL;
}

static UChar*
bm_search_notrev(regex_t* reg, const UChar* target, const UChar* target_end,
                 const UChar* text, const UChar* text_end,
                 const UChar* text_range)
{
  const UChar *s, *se, *t, *p, *end;
  const UChar *tail;
  int skip, tlen1;

#ifdef ONIG_DEBUG_SEARCH
  fprintf(stderr, "bm_search_notrev: text: %d, text_end: %d, text_range: %d\n",
          (int )text, (int )text_end, (int )text_range);
#endif

  tail = target_end - 1;
  tlen1 = tail - target;
  end = text_range;
  if (end + tlen1 > text_end)
    end = text_end - tlen1;

  s = text;

  if (IS_NULL(reg->int_map)) {
    while (s < end) {
      p = se = s + tlen1;
      t = tail;
      while (*p == *t) {
        if (t == target) return (UChar* )s;
        p--; t--;
      }
      skip = reg->map[*se];
      t = s;
      do {
        s += enclen(reg->enc, s, end);
      } while ((s - t) < skip && s < end);
    }
  }
  else {
    while (s < end) {
      p = se = s + tlen1;
      t = tail;
      while (*p == *t) {
        if (t == target) return (UChar* )s;
        p--; t--;
      }
      skip = reg->int_map[*se];
      t = s;
      do {
        s += enclen(reg->enc, s, end);
      } while ((s - t) < skip && s < end);
    }
  }

  return (UChar* )NULL;
}

static UChar*
bm_search(regex_t* reg, const UChar* target, const UChar* target_end,
          const UChar* text, const UChar* text_end, const UChar* text_range)
{
  const UChar *s, *t, *p, *end;
  const UChar *tail;

  end = text_range + (target_end - target) - 1;
  if (end > text_end)
    end = text_end;

  tail = target_end - 1;
  s = text + (target_end - target) - 1;
  if (IS_NULL(reg->int_map)) {
    while (s < end) {
      p = s;
      t = tail;
      while (*p == *t) {
        if (t == target) return (UChar* )p;
        p--; t--;
      }
      s += reg->map[*s];
    }
  }
  else { /* see int_map[] */
    while (s < end) {
      p = s;
      t = tail;
      while (*p == *t) {
        if (t == target) return (UChar* )p;
        p--; t--;
      }
      s += reg->int_map[*s];
    }
  }
  return (UChar* )NULL;
}

static int
set_bm_backward_skip(UChar* s, UChar* end, OnigEncoding enc ARG_UNUSED,
                     int** skip)
{
  int i, len;

  if (IS_NULL(*skip)) {
    *skip = (int* )xmalloc(sizeof(int) * ONIG_CHAR_TABLE_SIZE);
    if (IS_NULL(*skip)) return ONIGERR_MEMORY;
  }

  len = end - s;
  for (i = 0; i < ONIG_CHAR_TABLE_SIZE; i++)
    (*skip)[i] = len;

  for (i = len - 1; i > 0; i--)
    (*skip)[s[i]] = i;

  return 0;
}

static UChar*
bm_search_backward(regex_t* reg, const UChar* target, const UChar* target_end,
                   const UChar* text, const UChar* adjust_text,
                   const UChar* text_end, const UChar* text_start)
{
  const UChar *s, *t, *p;

  s = text_end - (target_end - target);
  if (text_start < s)
    s = text_start;
  else
    s = ONIGENC_LEFT_ADJUST_CHAR_HEAD(reg->enc, adjust_text, s, text_end);

  while (s >= text) {
    p = s;
    t = target;
    while (t < target_end && *p == *t) {
      p++; t++;
    }
    if (t == target_end)
      return (UChar* )s;

    s -= reg->int_map_backward[*s];
    s = ONIGENC_LEFT_ADJUST_CHAR_HEAD(reg->enc, adjust_text, s, text_end);
  }

  return (UChar* )NULL;
}

static UChar*
map_search(OnigEncoding enc, UChar map[],
           const UChar* text, const UChar* text_range, const UChar* text_end)
{
  const UChar *s = text;

  while (s < text_range) {
    if (map[*s]) return (UChar* )s;

    s += enclen(enc, s, text_end);
  }
  return (UChar* )NULL;
}

static UChar*
map_search_backward(OnigEncoding enc, UChar map[],
                    const UChar* text, const UChar* adjust_text,
                    const UChar* text_start, const UChar* text_end)
{
  const UChar *s = text_start;

  while (s >= text) {
    if (map[*s]) return (UChar* )s;

    s = onigenc_get_prev_char_head(enc, adjust_text, s, text_end);
  }
  return (UChar* )NULL;
}

extern long
onig_match(regex_t* reg, const UChar* str, const UChar* end, const UChar* at, OnigRegion* region,
            OnigOptionType option)
{
  long r;
  UChar *prev;
  OnigMatchArg msa;

#if defined(USE_RECOMPILE_API) && defined(USE_MULTI_THREAD_SYSTEM)
 start:
  THREAD_ATOMIC_START;
  if (ONIG_STATE(reg) >= ONIG_STATE_NORMAL) {
    ONIG_STATE_INC(reg);
    if (IS_NOT_NULL(reg->chain) && ONIG_STATE(reg) == ONIG_STATE_NORMAL) {
      onig_chain_reduce(reg);
      ONIG_STATE_INC(reg);
    }
  }
  else {
    int n;

    THREAD_ATOMIC_END;
    n = 0;
    while (ONIG_STATE(reg) < ONIG_STATE_NORMAL) {
      if (++n > THREAD_PASS_LIMIT_COUNT)
        return ONIGERR_OVER_THREAD_PASS_LIMIT_COUNT;
      THREAD_PASS;
    }
    goto start;
  }
  THREAD_ATOMIC_END;
#endif /* USE_RECOMPILE_API && USE_MULTI_THREAD_SYSTEM */

  MATCH_ARG_INIT(msa, option, region, at);
#ifdef USE_COMBINATION_EXPLOSION_CHECK
  {
    int offset = at - str;
    STATE_CHECK_BUFF_INIT(msa, end - str, offset, reg->num_comb_exp_check);
  }
#endif

  if (region
      ) {
    r = onig_region_resize_clear(region, reg->num_mem + 1);
  }
  else
    r = 0;

  if (r == 0) {
    prev = (UChar* )onigenc_get_prev_char_head(reg->enc, str, at, end);
    r = match_at(reg, str, end,
#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
                 end,
#endif
                 at, prev, &msa);
  }

  MATCH_ARG_FREE(msa);
  ONIG_STATE_DEC_THREAD(reg);
  return r;
}

static int
forward_search_range(regex_t* reg, const UChar* str, const UChar* end, UChar* s,
                     UChar* range, UChar** low, UChar** high, UChar** low_prev)
{
  UChar *p, *pprev = (UChar* )NULL;

#ifdef ONIG_DEBUG_SEARCH
  fprintf(stderr, "forward_search_range: str: %d, end: %d, s: %d, range: %d\n",
          (int )str, (int )end, (int )s, (int )range);
#endif

  p = s;
  if (reg->dmin > 0) {
    if (ONIGENC_IS_SINGLEBYTE(reg->enc)) {
      p += reg->dmin;
    }
    else {
      UChar *q = p + reg->dmin;
      while (p < q) p += enclen(reg->enc, p, end);
    }
  }

 retry:
  switch (reg->optimize) {
  case ONIG_OPTIMIZE_EXACT:
    p = slow_search(reg->enc, reg->exact, reg->exact_end, p, end, range);
    break;
  case ONIG_OPTIMIZE_EXACT_IC:
    p = slow_search_ic(reg->enc, reg->case_fold_flag,
                       reg->exact, reg->exact_end, p, end, range);
    break;

  case ONIG_OPTIMIZE_EXACT_BM:
    p = bm_search(reg, reg->exact, reg->exact_end, p, end, range);
    break;

  case ONIG_OPTIMIZE_EXACT_BM_NOT_REV:
    p = bm_search_notrev(reg, reg->exact, reg->exact_end, p, end, range);
    break;

  case ONIG_OPTIMIZE_MAP:
    p = map_search(reg->enc, reg->map, p, range, end);
    break;
  }

  if (p && p < range) {
    if (p - reg->dmin < s) {
    retry_gate:
      pprev = p;
      p += enclen(reg->enc, p, end);
      goto retry;
    }

    if (reg->sub_anchor) {
      UChar* prev;

      switch (reg->sub_anchor) {
      case ANCHOR_BEGIN_LINE:
        if (!ON_STR_BEGIN(p)) {
          prev = onigenc_get_prev_char_head(reg->enc,
                                            (pprev ? pprev : str), p, end);
          if (!ONIGENC_IS_MBC_NEWLINE(reg->enc, prev, end))
            goto retry_gate;
        }
        break;

      case ANCHOR_END_LINE:
        if (ON_STR_END(p)) {
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
          prev = (UChar* )onigenc_get_prev_char_head(reg->enc,
                                            (pprev ? pprev : str), p);
          if (prev && ONIGENC_IS_MBC_NEWLINE(reg->enc, prev, end))
            goto retry_gate;
#endif
        }
        else if (! ONIGENC_IS_MBC_NEWLINE(reg->enc, p, end)
#ifdef USE_CRNL_AS_LINE_TERMINATOR
              && ! ONIGENC_IS_MBC_CRNL(reg->enc, p, end)
#endif
                )
          goto retry_gate;
        break;
      }
    }

    if (reg->dmax == 0) {
      *low = p;
      if (low_prev) {
        if (*low > s)
          *low_prev = onigenc_get_prev_char_head(reg->enc, s, p, end);
        else
          *low_prev = onigenc_get_prev_char_head(reg->enc,
                                                 (pprev ? pprev : str), p, end);
      }
    }
    else {
      if (reg->dmax != ONIG_INFINITE_DISTANCE) {
        *low = p - reg->dmax;
        if (*low > s) {
          *low = onigenc_get_right_adjust_char_head_with_prev(reg->enc, s,
                                                              *low, end, (const UChar** )low_prev);
          if (low_prev && IS_NULL(*low_prev))
            *low_prev = onigenc_get_prev_char_head(reg->enc,
                                                   (pprev ? pprev : s), *low, end);
        }
        else {
          if (low_prev)
            *low_prev = onigenc_get_prev_char_head(reg->enc,
                                               (pprev ? pprev : str), *low, end);
        }
      }
    }
    /* no needs to adjust *high, *high is used as range check only */
    *high = p - reg->dmin;

#ifdef ONIG_DEBUG_SEARCH
    fprintf(stderr,
    "forward_search_range success: low: %d, high: %d, dmin: %d, dmax: %d\n",
            (int )(*low - str), (int )(*high - str), reg->dmin, reg->dmax);
#endif
    return 1; /* success */
  }

  return 0; /* fail */
}

#define BM_BACKWARD_SEARCH_LENGTH_THRESHOLD   100

static long
backward_search_range(regex_t* reg, const UChar* str, const UChar* end,
                      UChar* s, const UChar* range, UChar* adjrange,
                      UChar** low, UChar** high)
{
  int r;
  UChar *p;

  range += reg->dmin;
  p = s;

 retry:
  switch (reg->optimize) {
  case ONIG_OPTIMIZE_EXACT:
  exact_method:
    p = slow_search_backward(reg->enc, reg->exact, reg->exact_end,
                             range, adjrange, end, p);
    break;

  case ONIG_OPTIMIZE_EXACT_IC:
    p = slow_search_backward_ic(reg->enc, reg->case_fold_flag,
                                reg->exact, reg->exact_end,
                                range, adjrange, end, p);
    break;

  case ONIG_OPTIMIZE_EXACT_BM:
  case ONIG_OPTIMIZE_EXACT_BM_NOT_REV:
    if (IS_NULL(reg->int_map_backward)) {
      if (s - range < BM_BACKWARD_SEARCH_LENGTH_THRESHOLD)
        goto exact_method;

      r = set_bm_backward_skip(reg->exact, reg->exact_end, reg->enc,
                               &(reg->int_map_backward));
      if (r) return r;
    }
    p = bm_search_backward(reg, reg->exact, reg->exact_end, range, adjrange,
                           end, p);
    break;

  case ONIG_OPTIMIZE_MAP:
    p = map_search_backward(reg->enc, reg->map, range, adjrange, p, end);
    break;
  }

  if (p) {
    if (reg->sub_anchor) {
      UChar* prev;

      switch (reg->sub_anchor) {
      case ANCHOR_BEGIN_LINE:
        if (!ON_STR_BEGIN(p)) {
          prev = onigenc_get_prev_char_head(reg->enc, str, p, end);
          if (!ONIGENC_IS_MBC_NEWLINE(reg->enc, prev, end)) {
            p = prev;
            goto retry;
          }
        }
        break;

      case ANCHOR_END_LINE:
        if (ON_STR_END(p)) {
#ifndef USE_NEWLINE_AT_END_OF_STRING_HAS_EMPTY_LINE
          prev = onigenc_get_prev_char_head(reg->enc, adjrange, p);
          if (IS_NULL(prev)) goto fail;
          if (ONIGENC_IS_MBC_NEWLINE(reg->enc, prev, end)) {
            p = prev;
            goto retry;
          }
#endif
        }
        else if (! ONIGENC_IS_MBC_NEWLINE(reg->enc, p, end)
#ifdef USE_CRNL_AS_LINE_TERMINATOR
              && ! ONIGENC_IS_MBC_CRNL(reg->enc, p, end)
#endif
                ) {
          p = onigenc_get_prev_char_head(reg->enc, adjrange, p, end);
          if (IS_NULL(p)) goto fail;
          goto retry;
        }
        break;
      }
    }

    /* no needs to adjust *high, *high is used as range check only */
    if (reg->dmax != ONIG_INFINITE_DISTANCE) {
      *low  = p - reg->dmax;
      *high = p - reg->dmin;
      *high = onigenc_get_right_adjust_char_head(reg->enc, adjrange, *high, end);
    }

#ifdef ONIG_DEBUG_SEARCH
    fprintf(stderr, "backward_search_range: low: %d, high: %d\n",
            (int )(*low - str), (int )(*high - str));
#endif
    return 1; /* success */
  }

 fail:
#ifdef ONIG_DEBUG_SEARCH
  fprintf(stderr, "backward_search_range: fail.\n");
#endif
  return 0; /* fail */
}


extern long
onig_search(regex_t* reg, const UChar* str, const UChar* end,
            const UChar* start, const UChar* range, OnigRegion* region, OnigOptionType option)
{
  int r;
  UChar *s, *prev;
  OnigMatchArg msa;
  const UChar *orig_start = start;
#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
  const UChar *orig_range = range;
#endif

#if defined(USE_RECOMPILE_API) && defined(USE_MULTI_THREAD_SYSTEM)
 start:
  THREAD_ATOMIC_START;
  if (ONIG_STATE(reg) >= ONIG_STATE_NORMAL) {
    ONIG_STATE_INC(reg);
    if (IS_NOT_NULL(reg->chain) && ONIG_STATE(reg) == ONIG_STATE_NORMAL) {
      onig_chain_reduce(reg);
      ONIG_STATE_INC(reg);
    }
  }
  else {
    int n;

    THREAD_ATOMIC_END;
    n = 0;
    while (ONIG_STATE(reg) < ONIG_STATE_NORMAL) {
      if (++n > THREAD_PASS_LIMIT_COUNT)
        return ONIGERR_OVER_THREAD_PASS_LIMIT_COUNT;
      THREAD_PASS;
    }
    goto start;
  }
  THREAD_ATOMIC_END;
#endif /* USE_RECOMPILE_API && USE_MULTI_THREAD_SYSTEM */

#ifdef ONIG_DEBUG_SEARCH
  fprintf(stderr,
     "onig_search (entry point): str: %d, end: %d, start: %d, range: %d\n",
     (int )str, (int )(end - str), (int )(start - str), (int )(range - str));
#endif

  if (region
      ) {
    r = onig_region_resize_clear(region, reg->num_mem + 1);
    if (r) goto finish_no_msa;
  }

  if (start > end || start < str) goto mismatch_no_msa;


#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
#define MATCH_AND_RETURN_CHECK(upper_range) \
  r = match_at(reg, str, end, (upper_range), s, prev, &msa); \
  if (r != ONIG_MISMATCH) {\
    if (r >= 0) {\
      if (! IS_FIND_LONGEST(reg->options)) {\
        goto match;\
      }\
    }\
    else goto finish; /* error */ \
  }
#else
#define MATCH_AND_RETURN_CHECK(upper_range) \
  r = match_at(reg, str, end, (upper_range), s, prev, &msa); \
  if (r != ONIG_MISMATCH) {\
    if (r >= 0) {\
      goto match;\
    }\
    else goto finish; /* error */ \
  }
#endif /* USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE */
#else
#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
#define MATCH_AND_RETURN_CHECK(none) \
  r = match_at(reg, str, end, s, prev, &msa);\
  if (r != ONIG_MISMATCH) {\
    if (r >= 0) {\
      if (! IS_FIND_LONGEST(reg->options)) {\
        goto match;\
      }\
    }\
    else goto finish; /* error */ \
  }
#else
#define MATCH_AND_RETURN_CHECK(none) \
  r = match_at(reg, str, end, s, prev, &msa);\
  if (r != ONIG_MISMATCH) {\
    if (r >= 0) {\
      goto match;\
    }\
    else goto finish; /* error */ \
  }
#endif /* USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE */
#endif /* USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE */


  /* anchor optimize: resume search range */
  if (reg->anchor != 0 && str < end) {
    UChar *min_semi_end, *max_semi_end;

    if (reg->anchor & ANCHOR_BEGIN_POSITION) {
      /* search start-position only */
    begin_position:
      if (range > start)
        range = start + 1;
      else
        range = start;
    }
    else if (reg->anchor & ANCHOR_BEGIN_BUF) {
      /* search str-position only */
      if (range > start) {
        if (start != str) goto mismatch_no_msa;
        range = str + 1;
      }
      else {
        if (range <= str) {
          start = str;
          range = str;
        }
        else
          goto mismatch_no_msa;
      }
    }
    else if (reg->anchor & ANCHOR_END_BUF) {
      min_semi_end = max_semi_end = (UChar* )end;

    end_buf:
      if ((OnigDistance )(max_semi_end - str) < reg->anchor_dmin)
        goto mismatch_no_msa;

      if (range > start) {
        if ((OnigDistance )(min_semi_end - start) > reg->anchor_dmax) {
          start = min_semi_end - reg->anchor_dmax;
          if (start < end)
            start = onigenc_get_right_adjust_char_head(reg->enc, str, start, end);
          else { /* match with empty at end */
            start = onigenc_get_prev_char_head(reg->enc, str, end, end);
          }
        }
        if ((OnigDistance )(max_semi_end - (range - 1)) < reg->anchor_dmin) {
          range = max_semi_end - reg->anchor_dmin + 1;
        }

        if (start >= range) goto mismatch_no_msa;
      }
      else {
        if ((OnigDistance )(min_semi_end - range) > reg->anchor_dmax) {
          range = min_semi_end - reg->anchor_dmax;
        }
        if ((OnigDistance )(max_semi_end - start) < reg->anchor_dmin) {
          start = max_semi_end - reg->anchor_dmin;
          start = ONIGENC_LEFT_ADJUST_CHAR_HEAD(reg->enc, str, start, end);
        }
        if (range > start) goto mismatch_no_msa;
      }
    }
    else if (reg->anchor & ANCHOR_SEMI_END_BUF) {
      UChar* pre_end = ONIGENC_STEP_BACK(reg->enc, str, end, end, 1);

      max_semi_end = (UChar* )end;
      if (ONIGENC_IS_MBC_NEWLINE(reg->enc, pre_end, end)) {
        min_semi_end = pre_end;

#ifdef USE_CRNL_AS_LINE_TERMINATOR
        pre_end = ONIGENC_STEP_BACK(reg->enc, str, pre_end, end, 1);
        if (IS_NOT_NULL(pre_end) &&
            ONIGENC_IS_MBC_CRNL(reg->enc, pre_end, end)) {
          min_semi_end = pre_end;
        }
#endif
        if (min_semi_end > str && start <= min_semi_end) {
          goto end_buf;
        }
      }
      else {
        min_semi_end = (UChar* )end;
        goto end_buf;
      }
    }
    else if ((reg->anchor & ANCHOR_ANYCHAR_STAR_ML)) {
      goto begin_position;
    }
  }
  else if (str == end) { /* empty string */
    static const UChar address_for_empty_string[] = "";

#ifdef ONIG_DEBUG_SEARCH
    fprintf(stderr, "onig_search: empty string.\n");
#endif

    if (reg->threshold_len == 0) {
      start = end = str = address_for_empty_string;
      s = (UChar* )start;
      prev = (UChar* )NULL;

      MATCH_ARG_INIT(msa, option, region, start);
#ifdef USE_COMBINATION_EXPLOSION_CHECK
      msa.state_check_buff = (void* )0;
      msa.state_check_buff_size = 0;   /* NO NEED, for valgrind */
#endif
      MATCH_AND_RETURN_CHECK(end);
      goto mismatch;
    }
    goto mismatch_no_msa;
  }

#ifdef ONIG_DEBUG_SEARCH
  fprintf(stderr, "onig_search(apply anchor): end: %d, start: %d, range: %d\n",
          (int )(end - str), (int )(start - str), (int )(range - str));
#endif

  MATCH_ARG_INIT(msa, option, region, orig_start);
#ifdef USE_COMBINATION_EXPLOSION_CHECK
  {
    int offset = (MIN(start, range) - str);
    STATE_CHECK_BUFF_INIT(msa, end - str, offset, reg->num_comb_exp_check);
  }
#endif

  s = (UChar* )start;
  if (range > start) {   /* forward search */
    if (s > str)
      prev = onigenc_get_prev_char_head(reg->enc, str, s, end);
    else
      prev = (UChar* )NULL;

    if (reg->optimize != ONIG_OPTIMIZE_NONE) {
      UChar *sch_range, *low, *high, *low_prev;

      sch_range = (UChar* )range;
      if (reg->dmax != 0) {
        if (reg->dmax == ONIG_INFINITE_DISTANCE)
          sch_range = (UChar* )end;
        else {
          sch_range += reg->dmax;
          if (sch_range > end) sch_range = (UChar* )end;
        }
      }

      if ((end - start) < reg->threshold_len)
        goto mismatch;

      if (reg->dmax != ONIG_INFINITE_DISTANCE) {
        do {
          if (! forward_search_range(reg, str, end, s, sch_range,
                                     &low, &high, &low_prev)) goto mismatch;
          if (s < low) {
            s    = low;
            prev = low_prev;
          }
          while (s <= high) {
            MATCH_AND_RETURN_CHECK(orig_range);
            prev = s;
            s += enclen(reg->enc, s, end);
          }
        } while (s < range);
        goto mismatch;
      }
      else { /* check only. */
        if (! forward_search_range(reg, str, end, s, sch_range,
                                   &low, &high, (UChar** )NULL)) goto mismatch;

        if ((reg->anchor & ANCHOR_ANYCHAR_STAR) != 0) {
          do {
            MATCH_AND_RETURN_CHECK(orig_range);
            prev = s;
            s += enclen(reg->enc, s, end);
          } while (s < range);
          goto mismatch;
        }
      }
    }

    do {
      MATCH_AND_RETURN_CHECK(orig_range);
      prev = s;
      s += enclen(reg->enc, s, end);
    } while (s < range);

    if (s == range) { /* because empty match with /$/. */
      MATCH_AND_RETURN_CHECK(orig_range);
    }
  }
  else {  /* backward search */
#ifdef USE_MATCH_RANGE_MUST_BE_INSIDE_OF_SPECIFIED_RANGE
    if (orig_start < end)
        orig_start += enclen(reg->enc, orig_start, end); /* is upper range */
#endif

    if (reg->optimize != ONIG_OPTIMIZE_NONE) {
      UChar *low, *high, *adjrange, *sch_start;

      if (range < end)
        adjrange = ONIGENC_LEFT_ADJUST_CHAR_HEAD(reg->enc, str, range, end);
      else
        adjrange = (UChar* )end;

      if (reg->dmax != ONIG_INFINITE_DISTANCE &&
          (end - range) >= reg->threshold_len) {
        do {
          sch_start = s + reg->dmax;
          if (sch_start > end) sch_start = (UChar* )end;
          if (backward_search_range(reg, str, end, sch_start, range, adjrange,
                                    &low, &high) <= 0)
            goto mismatch;

          if (s > high)
            s = high;

          while (s >= low) {
            prev = onigenc_get_prev_char_head(reg->enc, str, s, end);
            MATCH_AND_RETURN_CHECK(orig_start);
            s = prev;
          }
        } while (s >= range);
        goto mismatch;
      }
      else { /* check only. */
        if ((end - range) < reg->threshold_len) goto mismatch;

        sch_start = s;
        if (reg->dmax != 0) {
          if (reg->dmax == ONIG_INFINITE_DISTANCE)
            sch_start = (UChar* )end;
          else {
            sch_start += reg->dmax;
            if (sch_start > end) sch_start = (UChar* )end;
            else
              sch_start = ONIGENC_LEFT_ADJUST_CHAR_HEAD(reg->enc,
                                                    start, sch_start, end);
          }
        }
        if (backward_search_range(reg, str, end, sch_start, range, adjrange,
                                  &low, &high) <= 0) goto mismatch;
      }
    }

    do {
      prev = onigenc_get_prev_char_head(reg->enc, str, s, end);
      MATCH_AND_RETURN_CHECK(orig_start);
      s = prev;
    } while (s >= range);
  }

 mismatch:
#ifdef USE_FIND_LONGEST_SEARCH_ALL_OF_RANGE
  if (IS_FIND_LONGEST(reg->options)) {
    if (msa.best_len >= 0) {
      s = msa.best_s;
      goto match;
    }
  }
#endif
  r = ONIG_MISMATCH;

 finish:
  MATCH_ARG_FREE(msa);
  ONIG_STATE_DEC_THREAD(reg);

  /* If result is mismatch and no FIND_NOT_EMPTY option,
     then the region is not setted in match_at(). */
  if (IS_FIND_NOT_EMPTY(reg->options) && region
      ) {
    onig_region_clear(region);
  }

#ifdef ONIG_DEBUG
  if (r != ONIG_MISMATCH)
    fprintf(stderr, "onig_search: error %d\n", r);
#endif
  return r;

 mismatch_no_msa:
  r = ONIG_MISMATCH;
 finish_no_msa:
  ONIG_STATE_DEC_THREAD(reg);
#ifdef ONIG_DEBUG
  if (r != ONIG_MISMATCH)
    fprintf(stderr, "onig_search: error %d\n", r);
#endif
  return r;

 match:
  ONIG_STATE_DEC_THREAD(reg);
  MATCH_ARG_FREE(msa);
  return s - str;
}

extern OnigEncoding
onig_get_encoding(regex_t* reg)
{
  return reg->enc;
}

extern OnigOptionType
onig_get_options(regex_t* reg)
{
  return reg->options;
}

extern  OnigCaseFoldType
onig_get_case_fold_flag(regex_t* reg)
{
  return reg->case_fold_flag;
}

extern const OnigSyntaxType*
onig_get_syntax(regex_t* reg)
{
  return reg->syntax;
}

extern int
onig_number_of_captures(regex_t* reg)
{
  return reg->num_mem;
}

extern int
onig_number_of_capture_histories(regex_t* reg)
{
#ifdef USE_CAPTURE_HISTORY
  int i, n;

  n = 0;
  for (i = 0; i <= ONIG_MAX_CAPTURE_HISTORY_GROUP; i++) {
    if (BIT_STATUS_AT(reg->capture_history, i) != 0)
      n++;
  }
  return n;
#else
  return 0;
#endif
}

extern void
onig_copy_encoding(OnigEncoding to, OnigEncoding from)
{
  *to = *from;
}
#endif //ENABLE_REGEXP
