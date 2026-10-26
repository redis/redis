/*
  tre-parse.c - Regexp parser definitions

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifndef TRE_PARSE_H
#define TRE_PARSE_H 1

/* Parse context. */
typedef struct {
  /* Memory allocator.	The AST is allocated using this. */
  tre_mem_t mem;
  /* Stack used for keeping track of regexp syntax. */
  tre_stack_t *stack;
  /* The parse result. */
  tre_ast_node_t *result;
  /* The regexp to parse and its length. */
  const tre_char_t *re;
  /* The first character of the entire regexp. */
  const tre_char_t *re_start;
  /* The first character after the end of the regexp. */
  const tre_char_t *re_end;
  size_t len;
  /* Current submatch ID. */
  int submatch_id;
  /* The highest back reference or -1 if none seen so far. */
  int max_backref;
  /* This flag is set if the regexp uses approximate matching. */
  int have_approx;
  /* This flag is set if the regexp changes cflags inline using (?...) */
  int have_inline_cflags;
  /* Compilation flags. */
  int cflags;
  /* If this flag is set the top-level submatch is not captured. */
  int nofirstsub;
  /* The currently set approximate matching parameters. */
  int params[TRE_PARAM_LAST];
  /* the MB_CUR_MAX in use */
  int mb_cur_max;
} tre_parse_ctx_t;

/* Parses a wide character regexp pattern into a syntax tree.  This parser
   handles both syntaxes (BRE and ERE), including the TRE extensions. */
reg_errcode_t
tre_parse(tre_parse_ctx_t *ctx);

#endif /* TRE_PARSE_H */

/* EOF */
