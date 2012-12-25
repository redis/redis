/*
** mruby/compile.h - mruby parser
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_COMPILE_H
#define MRUBY_COMPILE_H 1

#if defined(__cplusplus)
extern "C" {
#endif

#include "mruby.h"
#include <stdio.h>
#include <setjmp.h>

/* load context */
typedef struct mrbc_context {
  mrb_sym *syms;
  int slen;
  char *filename;
  short lineno;
  int capture_errors:1;
  int dump_result:1;
  int no_exec:1;
} mrbc_context;

mrbc_context* mrbc_context_new(mrb_state *mrb);
void mrbc_context_free(mrb_state *mrb, mrbc_context *cxt);
const char *mrbc_filename(mrb_state *mrb, mrbc_context *c, const char *s);

/* AST node structure */
typedef struct mrb_ast_node {
  struct mrb_ast_node *car, *cdr;
  short lineno;
} mrb_ast_node;

/* lexer states */
enum mrb_lex_state_enum {
    EXPR_BEG,                   /* ignore newline, +/- is a sign. */
    EXPR_END,                   /* newline significant, +/- is an operator. */
    EXPR_ENDARG,                /* ditto, and unbound braces. */
    EXPR_ENDFN,                 /* ditto, and unbound braces. */
    EXPR_ARG,                   /* newline significant, +/- is an operator. */
    EXPR_CMDARG,                /* newline significant, +/- is an operator. */
    EXPR_MID,                   /* newline significant, +/- is an operator. */
    EXPR_FNAME,                 /* ignore newline, no reserved words. */
    EXPR_DOT,                   /* right after `.' or `::', no reserved words. */
    EXPR_CLASS,                 /* immediate after `class', no here document. */
    EXPR_VALUE,                 /* alike EXPR_BEG but label is disallowed. */
    EXPR_MAX_STATE
};

/* saved error message */
struct mrb_parser_message {
  int lineno;
  int column;
  char* message;
};

/* parser structure */
struct mrb_parser_state {
  mrb_state *mrb;
  struct mrb_pool *pool;
  mrb_ast_node *cells;
  const char *s, *send;
  FILE *f;
  char *filename;
  int lineno;
  int column;

  enum mrb_lex_state_enum lstate;
  int sterm;

  unsigned int cond_stack;
  unsigned int cmdarg_stack;
  int paren_nest;
  int lpar_beg;
  int in_def, in_single, cmd_start;
  mrb_ast_node *locals;

  mrb_ast_node *pb;
  char buf[1024];
  int bidx;

  mrb_ast_node *heredoc;

  void *ylval;

  int nerr;
  int nwarn;
  mrb_ast_node *tree;

  int capture_errors;
  struct mrb_parser_message error_buffer[10];
  struct mrb_parser_message warn_buffer[10];

  jmp_buf jmp;
};

struct mrb_parser_state* mrb_parser_new(mrb_state*);
void mrb_parser_free(struct mrb_parser_state*);
const char *mrb_parser_filename(struct mrb_parser_state*, const char*);
void mrb_parser_parse(struct mrb_parser_state*,mrbc_context*);

/* utility functions */
struct mrb_parser_state* mrb_parse_file(mrb_state*,FILE*,mrbc_context*);
struct mrb_parser_state* mrb_parse_string(mrb_state*,const char*,mrbc_context*);
struct mrb_parser_state* mrb_parse_nstring(mrb_state*,const char*,int,mrbc_context*);
int mrb_generate_code(mrb_state*, struct mrb_parser_state*);

/* program load functions */
mrb_value mrb_load_file(mrb_state*,FILE*);
mrb_value mrb_load_string(mrb_state *mrb, const char *s);
mrb_value mrb_load_nstring(mrb_state *mrb, const char *s, int len);
mrb_value mrb_load_file_cxt(mrb_state*,FILE*, mrbc_context *cxt);
mrb_value mrb_load_string_cxt(mrb_state *mrb, const char *s, mrbc_context *cxt);
mrb_value mrb_load_nstring_cxt(mrb_state *mrb, const char *s, int len, mrbc_context *cxt);

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif /* MRUBY_COMPILE_H */
