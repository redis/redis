#include "mruby.h"
#include "mruby/proc.h"
#include "mruby/dump.h"
#include "mruby/cdump.h"
#include "mruby/compile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RITEBIN_EXT ".mrb"
#define C_EXT       ".c"
void mrb_show_version(mrb_state *);
void mrb_show_copyright(mrb_state *);
void parser_dump(mrb_state*, struct mrb_ast_node*, int);
void codedump_all(mrb_state*, int);

struct _args {
  FILE *rfp;
  FILE *wfp;
  char *filename;
  char *initname;
  char *ext;
  int check_syntax : 1;
  int dump_type    : 2;
  int verbose      : 1;
};

static void
usage(const char *name)
{
  static const char *const usage_msg[] = {
  "switches:",
  "-c           check syntax only",
  "-o<outfile>  place the output into <outfile>",
  "-v           print version number, then trun on verbose mode",
  "-B<symbol>   binary <symbol> output in C language format",
  "-C<func>     function <func> output in C language format",
  "--verbose    run at verbose mode",
  "--version    print the version",
  "--copyright  print the copyright",
  NULL
  };
  const char *const *p = usage_msg;

  printf("Usage: %s [switches] programfile\n", name);
  while(*p)
  printf("  %s\n", *p++);
}

static char *
get_outfilename(char *infile, char *ext)
{
  char *outfile;
  char *p;

  outfile = (char*)malloc(strlen(infile) + strlen(ext) + 1);
  strcpy(outfile, infile);
  if (*ext) {
    if ((p = strrchr(outfile, '.')) == NULL)
      p = &outfile[strlen(outfile)];
    strcpy(p, ext);
  }

  return outfile;
}

static int
parse_args(mrb_state *mrb, int argc, char **argv, struct _args *args)
{
  char *infile = NULL;
  char *outfile = NULL;
  char **origargv = argv;
  int result = 0;
  static const struct _args args_zero = { 0 };

  *args = args_zero;
  args->ext = RITEBIN_EXT;

  for (argc--,argv++; argc > 0; argc--,argv++) {
    if (**argv == '-') {
      if (strlen(*argv) == 1) {
	args->filename = infile = "-";
	args->rfp = stdin;
	break;
      }

      switch ((*argv)[1]) {
      case 'o':
        outfile = get_outfilename((*argv) + 2, "");
        break;
      case 'B':
      case 'C':
        args->ext = C_EXT;
        args->initname = (*argv) + 2;
        if (*args->initname == '\0') {
          printf("%s: Function name is not specified.\n", *origargv);
	  result = -2;
	  goto exit;
        }
        args->dump_type = ((*argv)[1] == 'B') ? DUMP_TYPE_BIN : DUMP_TYPE_CODE;
        break;
      case 'c':
        args->check_syntax = 1;
        break;
      case 'v':
        mrb_show_version(mrb);
        args->verbose = 1;
        break;
      case '-':
        if (strcmp((*argv) + 2, "version") == 0) {
          mrb_show_version(mrb);
	  exit(0);
        }
        else if (strcmp((*argv) + 2, "verbose") == 0) {
          args->verbose = 1;
          break;
        }
        else if (strcmp((*argv) + 2, "copyright") == 0) {
          mrb_show_copyright(mrb);
	  exit(0);
        }
	result = -3;
	goto exit;
      default:
	break;
      }
    }
    else if (args->rfp == NULL) {
      args->filename = infile = *argv;
      if ((args->rfp = fopen(infile, "r")) == NULL) {
        printf("%s: Cannot open program file. (%s)\n", *origargv, infile);
	goto exit;
      }
    }
  }

  if (infile == NULL) {
    result = -4;
    goto exit;
  }
  if (!args->check_syntax) {
    if (outfile == NULL) {
      if (strcmp("-", infile) == 0) {
	outfile = infile;
      }
      else {
	outfile = get_outfilename(infile, args->ext);
      }
    }
    if (strcmp("-", outfile) == 0) {
      args->wfp = stdout;
    }
    else if ((args->wfp = fopen(outfile, "wb")) == NULL) {
      printf("%s: Cannot open output file. (%s)\n", *origargv, outfile);
      result = -1;
      goto exit;
    }
  }
 exit:
  if (outfile && infile != outfile) free(outfile);
  return result;
}

static void
cleanup(mrb_state *mrb, struct _args *args)
{
  if (args->rfp)
    fclose(args->rfp);
  if (args->wfp)
    fclose(args->wfp);
  mrb_close(mrb);
}

int
main(int argc, char **argv)
{
  mrb_state *mrb = mrb_open();
  int n = -1;
  struct _args args;
  mrbc_context *c;
  mrb_value result;

  if (mrb == NULL) {
    fprintf(stderr, "Invalid mrb_state, exiting mrbc");
    return EXIT_FAILURE;
  }

  n = parse_args(mrb, argc, argv, &args);
  if (n < 0 || args.rfp == NULL) {
    cleanup(mrb, &args);
    usage(argv[0]);
    return n;
  }

  c = mrbc_context_new(mrb);
  if (args.verbose)
    c->dump_result = 1;
  c->no_exec = 1;
  c->filename = args.filename;
  result = mrb_load_file_cxt(mrb, args.rfp, c);
  if (mrb_undef_p(result) || mrb_fixnum(result) < 0) {
    cleanup(mrb, &args);
    return EXIT_FAILURE;
  }
  if (args.check_syntax) {
    printf("Syntax OK\n");
    cleanup(mrb, &args);
    return EXIT_SUCCESS;
  }
  if (args.initname) {
    if (args.dump_type == DUMP_TYPE_BIN)
      n = mrb_bdump_irep(mrb, n, args.wfp, args.initname);
    else
      n = mrb_cdump_irep(mrb, n, args.wfp, args.initname);
  }
  else {
    n = mrb_dump_irep(mrb, n, args.wfp);
  }

  cleanup(mrb, &args);
  return EXIT_SUCCESS;
}

void
mrb_init_mrblib(mrb_state *mrb)
{
}

#ifndef DISABLE_GEMS
void
mrb_init_mrbgems(mrb_state *mrb)
{
}
#endif
