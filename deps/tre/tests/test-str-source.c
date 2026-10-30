/*
  test-str-source.c - Sample program for using tre_reguexec()

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* look for getopt in order to use a -o option for output. */
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#elif defined(HAVE_GETOPT_H)
#include <getopt.h>
#endif

#include "tre-internal.h"

static FILE *outf = NULL;

/* Context structure for the tre_str_source wrappers.  */
typedef struct {
  /* Our string. */
  const char *str;
  /* Current position in the string. */
  size_t pos;
} str_handler_ctx;

/* The get_next_char() handler.  Sets `c' to the value of the next character,
   and increases `pos_add' by the number of bytes read.  Returns 1 if the
   string has ended, 0 if there are more characters. */
static int
str_handler_get_next(tre_char_t *c, unsigned int *pos_add, void *context)
{
  str_handler_ctx *ctx = context;
  unsigned char ch = ctx->str[ctx->pos];

#ifdef TRE_DEBUG
  fprintf(outf, "str[%lu] = %d\n", (unsigned long)ctx->pos, ch);
#endif /* TRE_DEBUG */
  *c = ch;
  if (ch)
    ctx->pos++;
  *pos_add = 1;

  return ch == '\0';
}

/* The rewind() handler.  Resets the current position in the input string. */
static void
str_handler_rewind(size_t pos, void *context)
{
  str_handler_ctx *ctx = context;

#ifdef TRE_DEBUG
  fprintf(outf, "rewind to %lu\n", (unsigned long)pos);
#endif /* TRE_DEBUG */
  ctx->pos = pos;
}

/* The compare() handler.  Compares two substrings in the input and returns
   0 if the substrings are equal, and a nonzero value if not. */
static int
str_handler_compare(size_t pos1, size_t pos2, size_t len, void *context)
{
  str_handler_ctx *ctx = context;
#ifdef TRE_DEBUG
  fprintf(outf, "comparing %lu-%lu and %lu-%lu\n",
	 (unsigned long)pos1, (unsigned long)pos1 + len,
	 (unsigned long)pos2, (unsigned long)pos2 + len);
#endif /* TRE_DEBUG */
  return strncmp(ctx->str + pos1, ctx->str + pos2, len);
}

/* Creates a tre_str_source wrapper around the string `str'.  Returns the
   tre_str_source object or NULL if out of memory. */
static tre_str_source *
make_str_source(const char *str)
{
  tre_str_source *s;
  str_handler_ctx *ctx;

  s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;

  ctx = malloc(sizeof(str_handler_ctx));
  if (!ctx)
    {
      free(s);
      return NULL;
    }

  ctx->str = str;
  ctx->pos = 0;
  s->context = ctx;
  s->get_next_char = str_handler_get_next;
  s->rewind = str_handler_rewind;
  s->compare = str_handler_compare;

  return s;
}

/* Frees the memory allocated for `s'. */
static void
free_str_source(tre_str_source *s)
{
  free(s->context);
  free(s);
}

/* Run one test with tre_reguexec.  Returns 1 if the regex matches, 0 if
   it doesn't, and -1 if an error occurs. */
static int
test_reguexec(const char *str, const char *regex)
{
  regex_t preg;
  tre_str_source *source;
  regmatch_t pmatch[5];
  int ret;

  if ((source = make_str_source(str)) == NULL)
    {
      fprintf(stderr, "Out of memory\n");
      ret = -1;
    }
  else
    {
      if (tre_regcomp(&preg, regex, REG_EXTENDED) != REG_OK)
	{
	  fprintf(stderr, "Failed to compile /%s/\n", regex);
	  ret = -1;
	}
      else
	{
	  if (tre_reguexec(&preg, source, elementsof(pmatch), pmatch, 0) == 0)
	    {
	      fprintf(outf, "Match: /%s/ matches \"%.*s\" in \"%s\"\n", regex,
		      (int)(pmatch[0].rm_eo - pmatch[0].rm_so),
		      str + pmatch[0].rm_so, str);
	      ret = 1;
	    }
	  else
	    {
	      fprintf(outf, "No match: /%s/ in \"%s\"\n", regex, str);
	      ret = 0;
	    }
	  tre_regfree(&preg);
	}
      free_str_source(source);
    }
  return ret;
}

int
main(int argc, char **argv)
{
  int ret = 0;
  outf = stdout;
#if defined(HAVE_UNISTD_H) || defined(HAVE_GETOPT_H)
  int opt;
  while ((opt = getopt(argc, argv, "o:")) != EOF)
    {
      switch (opt)
	{
	case 'o':
	  if ((outf = fopen(optarg, "w")) == NULL)
	    {
	      perror(optarg);
	      exit(1);
	    }
	  break;
	default:
	  /* getopt() will have printed an error message already */
	  exit(1);
	}
    }
#endif
  ret += test_reguexec("xfoofofoofoo", "(foo)\\1") != 1;
  ret += test_reguexec("catcat", "(cat|dog)\\1") != 1;
  ret += test_reguexec("catdog", "(cat|dog)\\1") != 0;
  ret += test_reguexec("dogdog", "(cat|dog)\\1") != 1;
  ret += test_reguexec("dogcat", "(cat|dog)\\1") != 0;

  return ret;
}
