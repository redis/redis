#ifndef ONIGURUMA_REGPARSE_H
#define ONIGURUMA_REGPARSE_H
/**********************************************************************
  regparse.h -  Oniguruma (regular expression library)
**********************************************************************/
/*-
 * Copyright (c) 2002-2007  K.Kosako  <sndgk393 AT ybb DOT ne DOT jp>
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

#include "regint.h"

/* node type */
#define NT_STR         0
#define NT_CCLASS      1
#define NT_CTYPE       2
#define NT_CANY        3
#define NT_BREF        4
#define NT_QTFR        5
#define NT_ENCLOSE     6
#define NT_ANCHOR      7
#define NT_LIST        8
#define NT_ALT         9
#define NT_CALL       10

/* node type bit */
#define NTYPE2BIT(type)      (1<<(type))

#define BIT_NT_STR        NTYPE2BIT(NT_STR)
#define BIT_NT_CCLASS     NTYPE2BIT(NT_CCLASS)
#define BIT_NT_CTYPE      NTYPE2BIT(NT_CTYPE)
#define BIT_NT_CANY       NTYPE2BIT(NT_CANY)
#define BIT_NT_BREF       NTYPE2BIT(NT_BREF)
#define BIT_NT_QTFR       NTYPE2BIT(NT_QTFR)
#define BIT_NT_ENCLOSE    NTYPE2BIT(NT_ENCLOSE)
#define BIT_NT_ANCHOR     NTYPE2BIT(NT_ANCHOR)
#define BIT_NT_LIST       NTYPE2BIT(NT_LIST)
#define BIT_NT_ALT        NTYPE2BIT(NT_ALT)
#define BIT_NT_CALL       NTYPE2BIT(NT_CALL)

#define IS_NODE_TYPE_SIMPLE(type) \
  ((NTYPE2BIT(type) & (BIT_NT_STR | BIT_NT_CCLASS | BIT_NT_CTYPE |\
                       BIT_NT_CANY | BIT_NT_BREF)) != 0)

#define NTYPE(node)             ((node)->u.base.type)
#define SET_NTYPE(node, ntype)   (node)->u.base.type = (ntype)

#define NSTR(node)         (&((node)->u.str))
#define NCCLASS(node)      (&((node)->u.cclass))
#define NCTYPE(node)       (&((node)->u.ctype))
#define NBREF(node)        (&((node)->u.bref))
#define NQTFR(node)        (&((node)->u.qtfr))
#define NENCLOSE(node)     (&((node)->u.enclose))
#define NANCHOR(node)      (&((node)->u.anchor))
#define NCONS(node)        (&((node)->u.cons))
#define NCALL(node)        (&((node)->u.call))

#define NCAR(node)         (NCONS(node)->car)
#define NCDR(node)         (NCONS(node)->cdr)



#define ANCHOR_ANYCHAR_STAR_MASK (ANCHOR_ANYCHAR_STAR | ANCHOR_ANYCHAR_STAR_ML)
#define ANCHOR_END_BUF_MASK      (ANCHOR_END_BUF | ANCHOR_SEMI_END_BUF)

#define ENCLOSE_MEMORY           (1<<0)
#define ENCLOSE_OPTION           (1<<1)
#define ENCLOSE_STOP_BACKTRACK   (1<<2)

#define NODE_STR_MARGIN         16
#define NODE_STR_BUF_SIZE       24  /* sizeof(CClassNode) - sizeof(int)*4 */
#define NODE_BACKREFS_SIZE       6

#define NSTR_RAW                (1<<0) /* by backslashed number */
#define NSTR_AMBIG              (1<<1)
#define NSTR_DONT_GET_OPT_INFO  (1<<2)

#define NSTRING_LEN(node)             ((node)->u.str.end - (node)->u.str.s)
#define NSTRING_SET_RAW(node)          (node)->u.str.flag |= NSTR_RAW
#define NSTRING_CLEAR_RAW(node)        (node)->u.str.flag &= ~NSTR_RAW
#define NSTRING_SET_AMBIG(node)        (node)->u.str.flag |= NSTR_AMBIG
#define NSTRING_SET_DONT_GET_OPT_INFO(node) \
  (node)->u.str.flag |= NSTR_DONT_GET_OPT_INFO
#define NSTRING_IS_RAW(node)          (((node)->u.str.flag & NSTR_RAW)   != 0)
#define NSTRING_IS_AMBIG(node)        (((node)->u.str.flag & NSTR_AMBIG) != 0)
#define NSTRING_IS_DONT_GET_OPT_INFO(node) \
  (((node)->u.str.flag & NSTR_DONT_GET_OPT_INFO) != 0)

#define BACKREFS_P(br) \
  (IS_NOT_NULL((br)->back_dynamic) ? (br)->back_dynamic : (br)->back_static);

#define NQ_TARGET_ISNOT_EMPTY     0
#define NQ_TARGET_IS_EMPTY        1
#define NQ_TARGET_IS_EMPTY_MEM    2
#define NQ_TARGET_IS_EMPTY_REC    3

/* status bits */
#define NST_MIN_FIXED             (1<<0)
#define NST_MAX_FIXED             (1<<1)
#define NST_CLEN_FIXED            (1<<2)
#define NST_MARK1                 (1<<3)
#define NST_MARK2                 (1<<4)
#define NST_MEM_BACKREFED         (1<<5)
#define NST_STOP_BT_SIMPLE_REPEAT (1<<6)
#define NST_RECURSION             (1<<7)
#define NST_CALLED                (1<<8)
#define NST_ADDR_FIXED            (1<<9)
#define NST_NAMED_GROUP           (1<<10)
#define NST_NAME_REF              (1<<11)
#define NST_IN_REPEAT             (1<<12) /* STK_REPEAT is nested in stack. */
#define NST_NEST_LEVEL            (1<<13)
#define NST_BY_NUMBER             (1<<14) /* {n,m} */

#define SET_ENCLOSE_STATUS(node,f)      (node)->u.enclose.state |=  (f)
#define CLEAR_ENCLOSE_STATUS(node,f)    (node)->u.enclose.state &= ~(f)

#define IS_ENCLOSE_CALLED(en)          (((en)->state & NST_CALLED)        != 0)
#define IS_ENCLOSE_ADDR_FIXED(en)      (((en)->state & NST_ADDR_FIXED)    != 0)
#define IS_ENCLOSE_RECURSION(en)       (((en)->state & NST_RECURSION)     != 0)
#define IS_ENCLOSE_MARK1(en)           (((en)->state & NST_MARK1)         != 0)
#define IS_ENCLOSE_MARK2(en)           (((en)->state & NST_MARK2)         != 0)
#define IS_ENCLOSE_MIN_FIXED(en)       (((en)->state & NST_MIN_FIXED)     != 0)
#define IS_ENCLOSE_MAX_FIXED(en)       (((en)->state & NST_MAX_FIXED)     != 0)
#define IS_ENCLOSE_CLEN_FIXED(en)      (((en)->state & NST_CLEN_FIXED)    != 0)
#define IS_ENCLOSE_STOP_BT_SIMPLE_REPEAT(en) \
    (((en)->state & NST_STOP_BT_SIMPLE_REPEAT) != 0)
#define IS_ENCLOSE_NAMED_GROUP(en)     (((en)->state & NST_NAMED_GROUP)   != 0)

#define SET_CALL_RECURSION(node)       (node)->u.call.state |= NST_RECURSION
#define IS_CALL_RECURSION(cn)          (((cn)->state & NST_RECURSION)  != 0)
#define IS_CALL_NAME_REF(cn)           (((cn)->state & NST_NAME_REF)   != 0)
#define IS_BACKREF_NAME_REF(bn)        (((bn)->state & NST_NAME_REF)   != 0)
#define IS_BACKREF_NEST_LEVEL(bn)      (((bn)->state & NST_NEST_LEVEL) != 0)
#define IS_QUANTIFIER_IN_REPEAT(qn)    (((qn)->state & NST_IN_REPEAT)  != 0)
#define IS_QUANTIFIER_BY_NUMBER(qn)    (((qn)->state & NST_BY_NUMBER)  != 0)

#define CALLNODE_REFNUM_UNDEF  -1

typedef struct {
  NodeBase base;
  UChar* s;
  UChar* end;
  unsigned int flag;
  int    capa;    /* (allocated size - 1) or 0: use buf[] */
  UChar  buf[NODE_STR_BUF_SIZE];
} StrNode;

typedef struct {
  NodeBase base;
  int state;
  struct _Node* target;
  int lower;
  int upper;
  int greedy;
  int target_empty_info;
  struct _Node* head_exact;
  struct _Node* next_head_exact;
  int is_refered;     /* include called node. don't eliminate even if {0} */
#ifdef USE_COMBINATION_EXPLOSION_CHECK
  int comb_exp_check_num;  /* 1,2,3...: check,  0: no check  */
#endif
} QtfrNode;

typedef struct {
  NodeBase base;
  int state;
  int type;
  int regnum;
  OnigOptionType option;
  struct _Node*  target;
  AbsAddrType    call_addr;
  /* for multiple call reference */
  OnigDistance min_len; /* min length (byte) */
  OnigDistance max_len; /* max length (byte) */
  int char_len;         /* character length  */
  int opt_count;        /* referenced count in optimize_node_left() */
} EncloseNode;

#ifdef USE_SUBEXP_CALL

typedef struct {
  int           offset;
  struct _Node* target;
} UnsetAddr;

typedef struct {
  int        num;
  int        alloc;
  UnsetAddr* us;
} UnsetAddrList;

typedef struct {
  NodeBase base;
  int     state;
  int     group_num;
  UChar*  name;
  UChar*  name_end;
  struct _Node*  target;  /* EncloseNode : ENCLOSE_MEMORY */
  UnsetAddrList* unset_addr_list;
} CallNode;

#endif

typedef struct {
  NodeBase base;
  int  state;
  int  back_num;
  int  back_static[NODE_BACKREFS_SIZE];
  int* back_dynamic;
  int  nest_level;
} BRefNode;

typedef struct {
  NodeBase base;
  int type;
  struct _Node* target;
  int char_len;
} AnchorNode;

typedef struct {
  NodeBase base;
  struct _Node* car;
  struct _Node* cdr;
} ConsAltNode;

typedef struct {
  NodeBase base;
  int ctype;
  int is_not;
} CtypeNode;

typedef struct _Node {
  union {
    NodeBase     base;
    StrNode      str;
    CClassNode   cclass;
    QtfrNode     qtfr;
    EncloseNode  enclose;
    BRefNode     bref;
    AnchorNode   anchor;
    ConsAltNode  cons;
    CtypeNode    ctype;
#ifdef USE_SUBEXP_CALL
    CallNode     call;
#endif
  } u;
} Node;


#define NULL_NODE  ((Node* )0)

#define SCANENV_MEMNODES_SIZE               8
#define SCANENV_MEM_NODES(senv)   \
 (IS_NOT_NULL((senv)->mem_nodes_dynamic) ? \
    (senv)->mem_nodes_dynamic : (senv)->mem_nodes_static)

typedef struct {
  OnigOptionType   option;
  OnigCaseFoldType case_fold_flag;
  OnigEncoding     enc;
  const OnigSyntaxType* syntax;
  BitStatusType    capture_history;
  BitStatusType    bt_mem_start;
  BitStatusType    bt_mem_end;
  BitStatusType    backrefed_mem;
  UChar*           pattern;
  UChar*           pattern_end;
  UChar*           error;
  UChar*           error_end;
  regex_t*         reg;       /* for reg->names only */
  int              num_call;
#ifdef USE_SUBEXP_CALL
  UnsetAddrList*   unset_addr_list;
#endif
  int              num_mem;
#ifdef USE_NAMED_GROUP
  int              num_named;
#endif
  int              mem_alloc;
  Node*            mem_nodes_static[SCANENV_MEMNODES_SIZE];
  Node**           mem_nodes_dynamic;
#ifdef USE_COMBINATION_EXPLOSION_CHECK
  int num_comb_exp_check;
  int comb_exp_max_regnum;
  int curr_max_regnum;
  int has_recursion;
#endif
  int warnings_flag;
  const char* sourcefile;
  int sourceline;
} ScanEnv;


#define IS_SYNTAX_OP(syn, opm)    (((syn)->op  & (opm)) != 0)
#define IS_SYNTAX_OP2(syn, opm)   (((syn)->op2 & (opm)) != 0)
#define IS_SYNTAX_BV(syn, bvm)    (((syn)->behavior & (bvm)) != 0)

#ifdef USE_NAMED_GROUP
typedef struct {
  int new_val;
} GroupNumRemap;

extern int    onig_renumber_name_table(regex_t* reg, GroupNumRemap* map);
#endif

extern int    onig_strncmp(const UChar* s1, const UChar* s2, int n);
extern void   onig_strcpy(UChar* dest, const UChar* src, const UChar* end);
extern void   onig_scan_env_set_error_string(ScanEnv* env, int ecode, UChar* arg, UChar* arg_end);
extern int    onig_scan_unsigned_number(UChar** src, const UChar* end, OnigEncoding enc);
extern void   onig_reduce_nested_quantifier(Node* pnode, Node* cnode);
extern void   onig_node_conv_to_str_node(Node* node, int raw);
extern int    onig_node_str_cat(Node* node, const UChar* s, const UChar* end);
extern int    onig_node_str_set(Node* node, const UChar* s, const UChar* end);
extern void   onig_node_free(Node* node);
extern Node*  onig_node_new_enclose(int type);
extern Node*  onig_node_new_anchor(int type);
extern Node*  onig_node_new_str(const UChar* s, const UChar* end);
extern Node*  onig_node_new_list(Node* left, Node* right);
extern Node*  onig_node_list_add(Node* list, Node* x);
extern Node*  onig_node_new_alt(Node* left, Node* right);
extern void   onig_node_str_clear(Node* node);
extern int    onig_free_node_list(void);
extern int    onig_names_free(regex_t* reg);
extern int    onig_parse_make_tree(Node** root, const UChar* pattern, const UChar* end, regex_t* reg, ScanEnv* env);
extern int    onig_free_shared_cclass_table(void);

#ifdef ONIG_DEBUG
#ifdef USE_NAMED_GROUP
extern int onig_print_names(FILE*, regex_t*);
#endif
#endif

#endif /* ONIGURUMA_REGPARSE_H */
