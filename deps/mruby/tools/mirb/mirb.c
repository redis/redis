/*
** mirb - Embeddable Interactive Ruby Shell
**
** This program takes code from the user in
** an interactive way and executes it
** immediately. It's a REPL...
*/

#include <string.h>

#include <mruby.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/compile.h>

#ifndef ENABLE_STDIO
#include <mruby/string.h>
static void
p(mrb_state *mrb, mrb_value obj)
{
  obj = mrb_funcall(mrb, obj, "inspect", 0);
  fwrite(RSTRING_PTR(obj), RSTRING_LEN(obj), 1, stdout);
  putc('\n', stdout);
}
#else
#define p(mrb,obj) mrb_p(mrb,obj)
#endif

/* Guess if the user might want to enter more
 * or if he wants an evaluation of his code now */
int
is_code_block_open(struct mrb_parser_state *parser)
{
  int code_block_open = FALSE;

  /* check for unterminated string */
  if (parser->sterm) return TRUE;

  /* check if parser error are available */
  if (0 < parser->nerr) {
    const char *unexpected_end = "syntax error, unexpected $end";
    const char *message = parser->error_buffer[0].message;

    /* a parser error occur, we have to check if */
    /* we need to read one more line or if there is */
    /* a different issue which we have to show to */
    /* the user */

    if (strncmp(message, unexpected_end, strlen(unexpected_end)) == 0) {
      code_block_open = TRUE;
    }
    else if (strcmp(message, "syntax error, unexpected keyword_end") == 0) {
      code_block_open = FALSE;
    }
    else if (strcmp(message, "syntax error, unexpected tREGEXP_BEG") == 0) {
      code_block_open = FALSE;
    }
    return code_block_open;
  }

  switch (parser->lstate) {

  /* all states which need more code */

  case EXPR_BEG:
    /* an expression was just started, */
    /* we can't end it like this */
    code_block_open = TRUE;
    break;
  case EXPR_DOT:
    /* a message dot was the last token, */
    /* there has to come more */
    code_block_open = TRUE;
    break;
  case EXPR_CLASS:
    /* a class keyword is not enough! */
    /* we need also a name of the class */
    code_block_open = TRUE;
    break;
  case EXPR_FNAME:
    /* a method name is necessary */
    code_block_open = TRUE;
    break;
  case EXPR_VALUE:
    /* if, elsif, etc. without condition */
    code_block_open = TRUE;
    break;

  /* now all the states which are closed */

  case EXPR_ARG:
    /* an argument is the last token */
    code_block_open = FALSE;
    break;

  /* all states which are unsure */

  case EXPR_CMDARG:
    break;
  case EXPR_END:
    /* an expression was ended */
    break;
  case EXPR_ENDARG:
    /* closing parenthese */
    break;
  case EXPR_ENDFN:
    /* definition end */
    break;
  case EXPR_MID:
    /* jump keyword like break, return, ... */
    break;
  case EXPR_MAX_STATE:
    /* don't know what to do with this token */
    break;
  default:
    /* this state is unexpected! */
    break;
  }

  return code_block_open;
}

/* Print a short remark for the user */
void print_hint(void)
{
  printf("mirb - Embeddable Interactive Ruby Shell\n");
  printf("\nThis is a very early version, please test and report errors.\n");
  printf("Thanks :)\n\n");
}

/* Print the command line prompt of the REPL */
void
print_cmdline(int code_block_open)
{
  if (code_block_open) {
    printf("* ");
  }
  else {
    printf("> ");
  }
}

int
main(void)
{
  int last_char;
  char ruby_code[1024] = { 0 };
  char last_code_line[1024] = { 0 };
  int char_index;
  mrbc_context *cxt;
  struct mrb_parser_state *parser;
  mrb_state *mrb;
  mrb_value result;
  int n;
  int code_block_open = FALSE;

  print_hint();

  /* new interpreter instance */
  mrb = mrb_open();
  if (mrb == NULL) {
    fprintf(stderr, "Invalid mrb interpreter, exiting mirb");
    return EXIT_FAILURE;
  }

  cxt = mrbc_context_new(mrb);
  cxt->capture_errors = 1;

  while (TRUE) {
    print_cmdline(code_block_open);

    char_index = 0;
    while ((last_char = getchar()) != '\n') {
      if (last_char == EOF) break;
      last_code_line[char_index++] = last_char;
    }
    if (last_char == EOF) {
      printf("\n");
      break;
    }

    last_code_line[char_index] = '\0';

    if ((strcmp(last_code_line, "quit") == 0) || (strcmp(last_code_line, "exit") == 0)) {
      if (!code_block_open) {
        break;
      }
      else{
        /* count the quit/exit commands as strings if in a quote block */
        strcat(ruby_code, "\n");
        strcat(ruby_code, last_code_line);
      }
    }
    else {
      if (code_block_open) {
        strcat(ruby_code, "\n");
        strcat(ruby_code, last_code_line);
      }
      else {
        strcpy(ruby_code, last_code_line);
      }
    }

    /* parse code */
    parser = mrb_parser_new(mrb);
    parser->s = ruby_code;
    parser->send = ruby_code + strlen(ruby_code);
    parser->lineno = 1;
    mrb_parser_parse(parser, cxt);
    code_block_open = is_code_block_open(parser);

    if (code_block_open) {
      /* no evaluation of code */
    }
    else {
      if (0 < parser->nerr) {
	/* syntax error */
	printf("line %d: %s\n", parser->error_buffer[0].lineno, parser->error_buffer[0].message);
      }
      else {
	/* generate bytecode */
	n = mrb_generate_code(mrb, parser);

	/* evaluate the bytecode */
	result = mrb_run(mrb,
            /* pass a proc for evaulation */
            mrb_proc_new(mrb, mrb->irep[n]),
            mrb_top_self(mrb));
	/* did an exception occur? */
	if (mrb->exc) {
	  p(mrb, mrb_obj_value(mrb->exc));
	  mrb->exc = 0;
	}
	else {
	  /* no */
	  printf(" => ");
	  p(mrb, result);
	}
      }
      ruby_code[0] = '\0';
      last_code_line[0] = '\0';
      mrb_parser_free(parser);
    }
  }
  mrbc_context_free(mrb, cxt);
  mrb_close(mrb);

  return 0;
}
