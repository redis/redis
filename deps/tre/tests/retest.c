/*
  retest.c - TRE regression test program

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

/*
   This is just a simple test application containing various hands-written
   tests for regression testing TRE.  I've tried to surround TRE specific
   tests inside ifdefs, so this can be used to test any POSIX compatible
   regexp implementation.
*/

/*
   2023/06 - Compilers now sometimes require the input string constants to be
             properly encoded, but how they decide on which encoding (if any)
             is poorly documented and different for different platforms.
             The non-ASCII encoded strings are now guarded by #ifdefs with one
             of the following values.  Define/undef whichever one(s) you need.
   #define SRC_IN_ISO_8859_1
   #define SRC_IN_UTF_8
   #define SRC_IN_EUC_JP
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <locale.h>
/* look for getopt in order to use a -o option for output. */
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#elif defined(HAVE_GETOPT_H)
#include <getopt.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif /* HAVE_MALLOC_H */

#ifdef TRE_VERSION
#define HAVE_REGNEXEC 1
#define HAVE_REGNCOMP 1
#include "xmalloc.h"
#else /* !TRE_VERSION */
#define xmalloc malloc
#define xfree free
#endif /* !TRE_VERSION */

#include "tre-internal.h"

#ifdef WRETEST
#include <wchar.h>
#define CHAR_T wchar_t
#define L(x) (L ## x)

#define MAXSTRSIZE 8192
static wchar_t wstr[MAXSTRSIZE];
static wchar_t wregex[MAXSTRSIZE];
static int woffs[MAXSTRSIZE];

#ifdef TRE_USE_SYSTEM_REGEX_H
/* Avoid some redefinition warnings from including tre.h. */
#ifdef tre_regexec
#undef tre_regexec
/* No need for the *n* fn, it isn't in the system abi. */
#endif
#endif
#define tre_regexec tre_regwexec
#define tre_regnexec tre_regwnexec
#ifdef TRE_USE_SYSTEM_REGEX_H
/* Avoid some redefinition warnings from including tre.h. */
#ifdef tre_regcomp 
#undef tre_regcomp
#endif
/* No need for the *n* fn, it isn't in the system abi. */
#endif
#define tre_regcomp tre_regwcomp
#define tre_regncomp tre_regwncomp

/* Iterate mbrtowc over the multi-byte sequence STR of length LEN,
   store the result in BUF and memoize the successive byte offsets
   in OFF.  */

static int
mbntowc (wchar_t *buf, const char *str, size_t len, int *off)
{
  int n, wlen;
#ifdef HAVE_MBSTATE_T
  mbstate_t cst;
  memset(&cst, 0, sizeof(cst));
#endif

  if (len >= MAXSTRSIZE)
    {
      fprintf(stderr, "Increase MAXSTRSIZE to %ld or more and recompile!\n",
	      (long)len + 1);
      exit(EXIT_FAILURE);
    }

  if (off)
    {
      memset(off + 1, -1, len * sizeof(int));
      *off = 0;
    }

  wlen = 0;
  while (len > 0)
    {
      n = tre_mbrtowc(buf ? buf++ : NULL, str, len, &cst);
      if (n < 0)
	return n;
      if (n == 0)
	n = 1;
      str += n;
      len -= n;
      wlen += 1;
      if (off)
	*(off += n) = wlen;
    }

  return(wlen);
}

#else /* !WRETEST */
#define CHAR_T char
#define L(x) (x)
#endif /* !WRETEST */

static FILE *outf = NULL;

static int valid_reobj = 0;
static regex_t reobj;
static regmatch_t pmatch_global[32];
static const CHAR_T *regex_pattern;
static int cflags_global;
static int use_regnexec = 0;
static int use_regncomp = 0;
static int avoid_eflags = 0;

static int comp_tests = 0;
static int exec_tests = 0;
static int comp_errors = 0;
static int exec_errors = 0;

#ifndef REG_OK
#define REG_OK 0
#endif /* REG_OK */

#define END -2

static void
test_status(char c)
{
  static int k = 0;
  fprintf(outf, "%c", c);
  if (++k % 79 == 0)
    fprintf(outf, "\n");
  fflush(outf);
}


static int
wrap_regexec(const CHAR_T *data, size_t len,
	     size_t pmatch_len, regmatch_t *pmatch, int eflags)
{
  CHAR_T *buf = NULL;
  int result;

  if (len == 0 && use_regnexec)
    {
      /* Zero length string and using tre_regnexec(), the pointer we give
	 should not be dereferenced at all. */
      buf = NULL;
    }
  else
    {
      /* Copy the data to a separate buffer to make a better test for
	 tre_regexec() and tre_regnexec(). */
      buf = xmalloc((len + !use_regnexec) * sizeof(CHAR_T));
      if (!buf)
	return REG_ESPACE;
      memcpy(buf, data, len * sizeof(CHAR_T));
      test_status('#');
    }

#ifdef HAVE_REGNEXEC
  if (use_regnexec)
    {
      if (len == 0)
	result = tre_regnexec(&reobj, NULL, len, pmatch_len, pmatch, eflags);
      else
	result = tre_regnexec(&reobj, buf, len, pmatch_len, pmatch, eflags);
    }
  else
#endif /* HAVE_REGNEXEC */
    {
      buf[len] = L('\0');
      result = tre_regexec(&reobj, buf, pmatch_len, pmatch, eflags);
    }

  xfree(buf);
  return result;
}

static int
wrap_regcomp(regex_t *preg, const CHAR_T *data, size_t len, int cflags)
{
#ifdef HAVE_REGNCOMP
  if (use_regncomp)
    return tre_regncomp(preg, data, len, cflags);
  else
    return tre_regcomp(preg, data, cflags);
#else /* !HAVE_REGNCOMP */
  fprintf(stderr, "%s\n", data);
  return tre_regcomp(preg, data, cflags);
#endif /* !HAVE_REGNCOMP */
}

static int
execute(const CHAR_T *data, int len, size_t pmatch_len, regmatch_t *pmatch,
	int eflags)
{
#ifdef MALLOC_DEBUGGING
  int i = 0;
  int ret;

  while (1)
    {
      xmalloc_configure(i);
      comp_tests++;
      ret = wrap_regexec(data, len, pmatch_len, pmatch, eflags);
      if (ret != REG_ESPACE)
	{
	  break;
	}
#ifdef REGEX_DEBUG
      xmalloc_dump_leaks();
#endif /* REGEX_DEBUG */
      i++;
    }
  return ret;
#else /* !MALLOC_DEBUGGING */
  return wrap_regexec(data, len, pmatch_len, pmatch, eflags);
#endif /* !MALLOC_DEBUGGING */
}

static int
check(va_list ap, int ret, const CHAR_T *str,
      size_t pmatch_len, regmatch_t *pmatch, int eflags)
{
  int fail = 0;

  if (ret != va_arg(ap, int))
    {
#ifndef WRETEST
      fprintf(outf, "Exec error, regex: \"%s\", cflags %d, "
	     "string: \"%s\", eflags %d\n", regex_pattern, cflags_global,
	     str, eflags);
#else /* WRETEST */
      fprintf(outf, "Exec error, regex: \"%ls\", cflags %d, "
	     "string: \"%ls\", eflags %d\n", regex_pattern, cflags_global,
	     str, eflags);
#endif /* WRETEST */
      fprintf(outf, "	got %smatch (tre_regexec returned %d)\n", ret ? "no " : "", ret);
      return 1;
    }

  if (ret == 0)
    {
      unsigned int i;

      for (i = 0; i < pmatch_len; i++)
	{
	  int rm_so, rm_eo;
	  rm_so = va_arg(ap, int);
	  if (rm_so == END)
	    break;
	  rm_eo = va_arg(ap, int);
#ifdef WRETEST
	  if (rm_so >= 0)
	    {
	      int n = rm_so;

	      if ((rm_so = woffs[rm_so]) < 0 ||
		  (n = rm_eo, rm_eo = woffs[rm_eo]) < 0)
		{
		  fprintf(outf, "Invalid or incomplete multi-byte sequence "
			 "in string %ls before byte offset %d\n", str, n);
		  return 1;
		}
	    }
#endif /* WRETEST */
	  if (pmatch[i].rm_so != rm_so
	      || pmatch[i].rm_eo != rm_eo)
	    {
#ifndef WRETEST
	      fprintf(outf, "Exec error, regex: \"%s\", string: \"%s\"\n",
		     regex_pattern, str);
	      fprintf(outf, "	group %d: expected (%d, %d) \"%.*s\", "
		     "got (%d, %d) \"%.*s\"\n",
#else /* WRETEST */
	      fprintf(outf, "Exec error, regex: \"%ls\", string: \"%ls\"\n",
		     regex_pattern, str);
	      fprintf(outf, "	group %d: expected (%d, %d) \"%.*ls\", "
		     "got (%d, %d) \"%.*ls\"\n",
#endif /* WRETEST */
		     i, rm_so, rm_eo, rm_eo - rm_so, str + rm_so,
		     (int)pmatch[i].rm_so, (int)pmatch[i].rm_eo,
		     (int)(pmatch[i].rm_eo - pmatch[i].rm_so),
		     str + pmatch[i].rm_so);
	      fail = 1;
	    }
	}

      if (!(cflags_global & REG_NOSUB) && reobj.re_nsub != i - 1
	  && reobj.re_nsub <= pmatch_len && pmatch)
	{
#ifndef WRETEST
	  fprintf(outf, "Comp error, regex: \"%s\"\n", regex_pattern);
#else /* WRETEST */
	  fprintf(outf, "Comp error, regex: \"%ls\"\n", regex_pattern);
#endif /* WRETEST */
	  fprintf(outf, "  re_nsub is %d, should be %d\n", (int)reobj.re_nsub, i - 1);
	  fail = 1;
	}


      for (; i < pmatch_len; i++)
	if (pmatch[i].rm_so != -1 || pmatch[i].rm_eo != -1)
	  {
	    if (!fail)
#ifndef WRETEST
	      fprintf(outf, "Exec error, regex: \"%s\", string: \"%s\"\n",
		     regex_pattern, str);
#else /* WRETEST */
	      fprintf(outf, "Exec error, regex: \"%ls\", string: \"%ls\"\n",
		     regex_pattern, str);
#endif /* WRETEST */
	    fprintf(outf, "  group %d: expected (-1, -1), got (%d, %d)\n",
		   i, (int)pmatch[i].rm_so, (int)pmatch[i].rm_eo);
	    fail = 1;
	  }
    }

  return fail;
}


static void
test_nexec(const char *data, size_t len, int eflags, ...)
{
  int m;
  int fail = 0;
  int extra_flags[] = {0, REG_BACKTRACKING_MATCHER, REG_APPROX_MATCHER};
  size_t i;
  va_list ap;

  if (!valid_reobj)
    {
      exec_errors++;
      return;
    }

#ifdef WRETEST
      {
	int wlen = mbntowc(wstr, data, len, woffs);
	if (wlen < 0)
	  {
	    exec_errors++;
	    fprintf(outf, "Invalid or incomplete multi-byte sequence in %s\n", data);
	    return;
	  }
	wstr[wlen] = L'\0';
	len = wlen;
      }
#define data wstr
#endif /* WRETEST */

  use_regnexec = 1;

  for (i = 0; i < elementsof(extra_flags); i++)
    {
      int final_flags = eflags | extra_flags[i];

      if ((final_flags & REG_BACKTRACKING_MATCHER
	   && tre_have_approx(&reobj))
	  || (final_flags & REG_APPROX_MATCHER
	      && tre_have_backrefs(&reobj))
	  || (final_flags & avoid_eflags))
	continue;

      /* Test with a pmatch array. */
      exec_tests++;
      m = execute(data, len, elementsof(pmatch_global), pmatch_global,
		  final_flags);
      va_start(ap, eflags);
      fail |= check(ap, m, data, elementsof(pmatch_global), pmatch_global,
		    final_flags);
      va_end(ap);

      /* Same test with a NULL pmatch. */
      exec_tests++;
      m = execute(data, len, 0, NULL, final_flags);
      va_start(ap, eflags);
      fail |= check(ap, m, data, 0, NULL, final_flags);
      va_end(ap);
    }

#ifdef WRETEST
#undef data
#endif /* WRETEST */

  if (fail)
    exec_errors++;
}



static void
test_exec(const char *str, int eflags, ...)
{
  int m;
  int fail = 0;
  size_t len = strlen(str);
  int extra_flags[] = {0,
		       REG_BACKTRACKING_MATCHER,
		       REG_APPROX_MATCHER,
		       REG_BACKTRACKING_MATCHER | REG_APPROX_MATCHER};
  size_t i;
  va_list ap;

  if (!valid_reobj)
    {
      exec_errors++;
      return;
    }

#ifdef WRETEST
      {
	int wlen = mbntowc(wstr, str, len, woffs);
	if (wlen < 0)
	  {
	    exec_errors++;
	    fprintf(outf, "Invalid or incomplete multi-byte sequence in %s\n", str);
	    return;
	  }
	wstr[wlen] = L'\0';
	len = wlen;
      }
#define str wstr
#endif /* WRETEST */

  for (use_regnexec = 0; use_regnexec < 2; use_regnexec++)
    {
      for (i = 0; i < elementsof(extra_flags); i++)
	{
	  int final_flags = eflags | extra_flags[i];

	  if ((final_flags & REG_BACKTRACKING_MATCHER
	       && tre_have_approx(&reobj))
	      || (final_flags & REG_APPROX_MATCHER
		  && tre_have_backrefs(&reobj))
	      || (final_flags & avoid_eflags))
	    continue;

	  /* Test with a pmatch array. */
	  exec_tests++;
	  m = execute(str, len, elementsof(pmatch_global), pmatch_global,
		      final_flags);
	  va_start(ap, eflags);
	  fail |= check(ap, m, str, elementsof(pmatch_global), pmatch_global,
			final_flags);
	  va_end(ap);

	  /* Same test with a NULL pmatch. */
	  exec_tests++;
	  m = execute(str, len, 0, NULL, final_flags);
	  va_start(ap, eflags);
	  fail |= check(ap, m, str, 0, NULL, final_flags);
	  va_end(ap);
	}
    }

#ifdef WRETEST
#undef str
#endif /* WRETEST */

  if (fail)
    exec_errors++;
}


static void
test_comp(const char *re, int flags, int ret)
{
  int errcode = 0;
  int len = re ? strlen(re) : 0;

  if (valid_reobj)
    {
      tre_regfree(&reobj);
      valid_reobj = 0;
    }

  comp_tests++;

#ifdef WRETEST
  {
    int wlen = mbntowc(wregex, re, len, NULL);

    if (wlen < 0)
      {
	comp_errors++;
	fprintf(outf, "Invalid or incomplete multi-byte sequence in %s\n", re);
	return;
      }
    wregex[wlen] = L'\0';
    len = wlen;
  }
#define re wregex
#endif /* WRETEST */
  regex_pattern = re;
  cflags_global = flags;

#ifdef MALLOC_DEBUGGING
  xmalloc_configure(-1);
  if (ret != REG_ESPACE) {
    static int j = 0;
    int i = 0;
    while (1)
      {
	xmalloc_configure(i);
	comp_tests++;
	if (j++ % 20 == 0)
	  test_status('.');
	errcode = wrap_regcomp(&reobj, re, len, flags);
	if (errcode != REG_ESPACE)
	  {
	    test_status('*');
	    break;
	  }
#ifdef REGEX_DEBUG
	xmalloc_dump_leaks();
#endif /* REGEX_DEBUG */
	i++;
      }
  } else
#endif /* !MALLOC_DEBUGGING */
  errcode = wrap_regcomp(&reobj, re, len, flags);

#ifdef WRETEST
#undef re
#endif /* WRETEST */

  if (errcode != ret)
    {
#ifndef WRETEST
      fprintf(outf, "Comp error, regex: \"%s\"\n", regex_pattern);
#else /* WRETEST */
      fprintf(outf, "Comp error, regex: \"%ls\"\n", regex_pattern);
#endif /* WRETEST */
      fprintf(outf, "	expected return code %d, got %d.\n",
	     ret, errcode);
      comp_errors++;
    }

  if (errcode == 0)
    valid_reobj = 1;
}



/* To enable tests for known bugs, set this to 1. */
#define KNOWN_BUG 0

int
main(int argc, char **argv)
{
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
#endif /* HAVE_UNISTD_H */

#ifdef WRETEST
  /* Need an 8-bit locale.  Or move the two tests with non-ascii
     characters to the localized internationalization tests.  */
  if (setlocale(LC_CTYPE, "en_US.ISO-8859-1") == NULL &&
      setlocale(LC_CTYPE, "en_US.ISO8859-1") == NULL)
    fprintf(stderr, "Could not set locale en_US.ISO-8859-1.  Expect some\n"
		    "`Invalid or incomplete multi-byte sequence' errors.\n");
#endif /* WRETEST */
  /* Large number of macros in one regexp. */
  test_comp("[A-Z]\\d\\s?\\d[A-Z]{2}|[A-Z]\\d{2}\\s?\\d[A-Z]{2}|[A-Z]{2}\\d"
	    "\\s?\\d[A-Z]{2}|[A-Z]{2}\\d{2}\\s?\\d[A-Z]{2}|[A-Z]\\d[A-Z]\\s?"
	    "\\d[A-Z]{2}|[A-Z]{2}\\d[A-Z]\\s?\\d[A-Z]{2}|[A-Z]{3}\\s?\\d[A-Z]"
	    "{2}", REG_EXTENDED, 0);

  test_comp("a{11}(b{2}c){2}", REG_EXTENDED, 0);
  test_comp("a{2}{2}xb+xc*xd?x", REG_EXTENDED, 0);
  test_comp("^!packet [0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3} [0-9]+",
	    REG_EXTENDED, 0);
  test_comp("^!pfast [0-9]{1,15} ([0-9]{1,3}\\.){3}[0-9]{1,3}[0-9]{1,5}$",
	    REG_EXTENDED, 0);

#if KNOWN_BUG
  /* Should these match or not? */
  test_comp("(a)*-\\1b", REG_EXTENDED, 0);
  test_exec("aaa-b", 0, REG_NOMATCH);
  test_comp("((.*)\\1)+", REG_EXTENDED, 0);
  test_exec("xxxxxx", 0, REG_NOMATCH);
#endif

#ifdef TRE_APPROX
  /*
   * Approximate matching tests.
   *
   * The approximate matcher always searches for the best match, and returns
   * the leftmost and longest one if there are several best matches.
   */

  test_comp("(fou){# ~1}", REG_EXTENDED, 0);
  test_comp("(fuu){#}", REG_EXTENDED, 0);
  test_comp("(fuu){# ~}", REG_EXTENDED, 0);
  test_comp("(anaconda){ 1i + 1d < 1, #1}", REG_EXTENDED, 0);
  test_comp("(anaconda){ 1i + 1d < 1 #1 ~10 }", REG_EXTENDED, 0);
  test_comp("(anaconda){ #1, ~1, 1i + 1d < 1 }", REG_EXTENDED, 0);

  test_comp("(znacnda){ #1 ~3 1i + 1d < 1 }", REG_EXTENDED, 0);
  test_exec("molasses anaconda foo bar baz smith anderson ",
	    0, REG_NOMATCH);
  test_comp("(znacnda){ #1 ~3 1i + 1d < 2 }", REG_EXTENDED, 0);
  test_exec("molasses anaconda foo bar baz smith anderson ",
	    0, REG_OK, 9, 17, 9, 17, END);
  test_comp("(ananda){ 1i + 1d < 2 }", REG_EXTENDED, 0);
  test_exec("molasses anaconda foo bar baz smith anderson ",
	    0, REG_NOMATCH);

  test_comp("(fuu){ +3 -3 ~5}", REG_EXTENDED, 0);
  test_exec("anaconda foo bar baz smith anderson",
	    0, REG_OK, 9, 10, 9, 10, END);
  test_comp("(fuu){ +2 -2 ~5}", REG_EXTENDED, 0);
  test_exec("anaconda foo bar baz smith anderson",
	    0, REG_OK, 9, 10, 9, 10, END);
  test_comp("(fuu){ +3 -3 ~}", REG_EXTENDED, 0);
  test_exec("anaconda foo bar baz smith anderson",
	    0, REG_OK, 9, 10, 9, 10, END);

  test_comp("(laurikari){ #3, 1i + 1d < 3 }", REG_EXTENDED, 0);

  /* No cost limit. */
  test_comp("(foobar){~}", REG_EXTENDED, 0);
  test_exec("xirefoabralfobarxie", 0, REG_OK, 11, 16, 11, 16, END);

  /* At most two errors. */
  test_comp("(foobar){~2}", REG_EXTENDED, 0);
  test_exec("xirefoabrzlfd", 0, REG_OK, 4, 9, 4, 9, END);
  test_exec("xirefoabzlfd", 0, REG_NOMATCH);

  /* At most two inserts or substitutions and max two errors total. */
  test_comp("(foobar){+2#2~2}", REG_EXTENDED, 0);
  test_exec("oobargoobaploowap", 0, REG_OK, 5, 11, 5, 11, END);

  /* Find best whole word match for "foobar". */
  test_comp("\\<(foobar){~}\\>", REG_EXTENDED, 0);
  test_exec("zfoobarz", 0, REG_OK, 0, 8, 0, 8, END);
  test_exec("boing zfoobarz goobar woop", 0, REG_OK, 15, 21, 15, 21, END);

  /* Match whole string, allow only 1 error. */
  test_comp("^(foobar){~1}$", REG_EXTENDED, 0);
  test_exec("foobar", 0, REG_OK, 0, 6, 0, 6, END);
  test_exec("xfoobar", 0, REG_OK, 0, 7, 0, 7, END);
  /*
    This currently fails.
  test_exec("foobarx", 0, REG_OK, 0, 7, 0, 7, END);
  */
  test_exec("fooxbar", 0, REG_OK, 0, 7, 0, 7, END);
  test_exec("foxbar", 0, REG_OK, 0, 6, 0, 6, END);
  test_exec("xoobar", 0, REG_OK, 0, 6, 0, 6, END);
  test_exec("foobax", 0, REG_OK, 0, 6, 0, 6, END);
  test_exec("oobar", 0, REG_OK, 0, 5, 0, 5, END);
  test_exec("fobar", 0, REG_OK, 0, 5, 0, 5, END);
  test_exec("fooba", 0, REG_OK, 0, 5, 0, 5, END);
  test_exec("xfoobarx", 0, REG_NOMATCH);
  test_exec("foobarxx", 0, REG_NOMATCH);
  test_exec("xxfoobar", 0, REG_NOMATCH);
  test_exec("xfoxbar", 0, REG_NOMATCH);
  test_exec("foxbarx", 0, REG_NOMATCH);

  /* At most one insert, two deletes, and three substitutions.
     Additionally, deletes cost two and substitutes one, and total
     cost must be less than 4. */
  test_comp("(foobar){+1 -2 #3, 2d + 1s < 4}", REG_EXTENDED, 0);
  test_exec("3oifaowefbaoraofuiebofasebfaobfaorfeoaro",
	    0, REG_OK, 26, 33, 26, 33, END);

  /* Partially approximate matches. */
  test_comp("foo(bar){~1}zap", REG_EXTENDED, 0);
  test_exec("foobarzap", 0, REG_OK, 0, 9, 3, 6, END);
  test_exec("fobarzap", 0, REG_NOMATCH);
  test_exec("foobrzap", 0, REG_OK, 0, 8, 3, 5, END);
  test_comp("^.*(dot.org){~}.*$", REG_EXTENDED, 0);
  test_exec("www.cnn.com 64.236.16.20\n"
	    "www.slashdot.org 66.35.250.150\n"
	    "For useful information, use www.slashdot.org\n"
	    "this is demo data!\n",
	    0, REG_OK, 0, 120, 93, 100, END);

  /* Approximate matching and back referencing cannot be used together. */
  test_comp("(foo{~})\\1", REG_EXTENDED, REG_BADPAT);

#endif /* TRE_APPROX */

  /*
   * Basic tests with pure regular expressions
   */

  /* Basic string matching. */
  test_comp("foobar", REG_EXTENDED, 0);
  test_exec("foobar", 0, REG_OK, 0, 6, END);
  test_exec("xxxfoobarzapzot", 0, REG_OK, 3, 9, END);
  test_comp("foobar", REG_EXTENDED | REG_NOSUB, 0);
  test_exec("foobar", 0, REG_OK, END);
  test_comp("aaaa", REG_EXTENDED, 0);
  test_exec("xxaaaaaaaaaaaaaaaaa", 0, REG_OK, 2, 6, END);

  /* Test zero length matches. */
  test_comp("(a*)", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, END);

  test_comp("(a*)*", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, END);

  test_comp("((a*)*)*", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, 0, 0, END);
  test_comp("(a*bcd)*", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaaaabcxbcxbcxaabcxaabcx", 0, REG_OK, 0, 0, -1, -1, END);
  test_exec("aaaaaaaaaaaabcxbcxbcxaabcxaabc", 0, REG_OK, 0, 0, -1, -1, END);
  test_exec("aaaaaaaaaaaabcxbcdbcxaabcxaabc", 0, REG_OK, 0, 0, -1, -1, END);
  test_exec("aaaaaaaaaaaabcdbcdbcxaabcxaabc", 0, REG_OK, 0, 18, 15, 18, END);

  test_comp("(a*)+", REG_EXTENDED, 0);
  test_exec("-", 0, REG_OK, 0, 0, 0, 0, END);

  /* This test blows up the backtracking matcher. */
  avoid_eflags = REG_BACKTRACKING_MATCHER;
  test_comp("((a*)*b)*b", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaaaaaaaaaaaaaaaaab", 0, REG_OK,
	    25, 26, -1, -1, -1, -1, END);
  avoid_eflags = 0;

  test_comp("", 0, 0);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("foo", 0, REG_OK, 0, 0, END);

  /* Test for submatch addressing which requires arbitrary lookahead. */
  test_comp("(a*)aaaaaa", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaaaaaaax", 0, REG_OK, 0, 15, 0, 9, END);

  /* Test leftmost and longest matching and some tricky submatches. */
  test_comp("(a*)(a*)", REG_EXTENDED, 0);
  test_exec("aaaa", 0, REG_OK, 0, 4, 0, 4, 4, 4, END);
  test_comp("(abcd|abc)(d?)", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 4, 0, 4, 4, 4, END);
  test_comp("(abc|abcd)(d?)", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 4, 0, 4, 4, 4, END);
  test_comp("(abc|abcd)(d?)e", REG_EXTENDED, 0);
  test_exec("abcde", 0, REG_OK, 0, 5, 0, 4, 4, 4, END);
  test_comp("(abcd|abc)(d?)e", REG_EXTENDED, 0);
  test_exec("abcde", 0, REG_OK, 0, 5, 0, 4, 4, 4, END);
  test_comp("a(bc|bcd)(d?)", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 4, 1, 4, 4, 4, END);
  test_comp("a(bcd|bc)(d?)", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 4, 1, 4, 4, 4, END);
  test_comp("a*(a?bc|bcd)(d?)", REG_EXTENDED, 0);
  test_exec("aaabcd", 0, REG_OK, 0, 6, 3, 6, 6, 6, END);
  test_comp("a*(bcd|a?bc)(d?)", REG_EXTENDED, 0);
  test_exec("aaabcd", 0, REG_OK, 0, 6, 3, 6, 6, 6, END);
  test_comp("(a|(a*b*))*", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, 0, 0, END);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, -1, -1, END);
  test_exec("aa", 0, REG_OK, 0, 2, 0, 2, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, 0, 3, 0, 3, END);
  test_exec("bbb", 0, REG_OK, 0, 3, 0, 3, 0, 3, END);
  test_exec("aaabbb", 0, REG_OK, 0, 6, 0, 6, 0, 6, END);
  test_exec("bbbaaa", 0, REG_OK, 0, 6, 3, 6, 3, 6, END);
  test_comp("((a*b*)|a)*", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, 0, 0, END);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 2, 0, 2, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, 0, 3, 0, 3, END);
  test_exec("bbb", 0, REG_OK, 0, 3, 0, 3, 0, 3, END);
  test_exec("aaabbb", 0, REG_OK, 0, 6, 0, 6, 0, 6, END);
  test_exec("bbbaaa", 0, REG_OK, 0, 6, 3, 6, 3, 6, END);
  test_comp("a.*(.*b.*(.*c.*).*d.*).*e.*(.*f.*).*g", REG_EXTENDED, 0);
  test_exec("aabbccddeeffgg", 0, REG_OK, 0, 14, 3, 9, 5, 7, 11, 13, END);
  test_comp("(wee|week)(night|knights)s*", REG_EXTENDED, 0);
  test_exec("weeknights", 0, REG_OK, 0, 10, 0, 3, 3, 10, END);
  test_exec("weeknightss", 0, REG_OK, 0, 11, 0, 3, 3, 10, END);
  test_comp("a*", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaa", 0, REG_OK, 0, 10, END);
  test_comp("aa*", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaa", 0, REG_OK, 0, 10, END);
  test_comp("aaa*", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaa", 0, REG_OK, 0, 10, END);
  test_comp("aaaa*", REG_EXTENDED, 0);
  test_exec("aaaaaaaaaa", 0, REG_OK, 0, 10, END);

  /* Test clearing old submatch data with nesting parentheses
     and iteration. */
  test_comp("((a)|(b))*c", REG_EXTENDED, 0);
  test_exec("aaabc", 0, REG_OK, 0, 5, 3, 4, -1, -1, 3, 4, END);
  test_exec("aaaac", 0, REG_OK, 0, 5, 3, 4, 3, 4, -1, -1, END);
  test_comp("foo((bar)*)*zot", REG_EXTENDED, 0);
  test_exec("foozot", 0, REG_OK, 0, 6, 3, 3, -1, -1, END);
  test_exec("foobarzot", 0, REG_OK, 0, 9, 3, 6, 3, 6, END);
  test_exec("foobarbarzot", 0, REG_OK, 0, 12, 3, 9, 6, 9, END);

  test_comp("foo((zup)*|(bar)*|(zap)*)*zot", REG_EXTENDED, 0);
  test_exec("foobarzapzot", 0, REG_OK,
	    0, 12, 6, 9, -1, -1, -1, -1, 6, 9, END);
  test_exec("foobarbarzapzot", 0, REG_OK,
	    0, 15, 9, 12, -1, -1, -1, -1, 9, 12, END);
  test_exec("foozupzot", 0, REG_OK,
	    0, 9, 3, 6, 3, 6, -1, -1, -1, -1, END);
  test_exec("foobarzot", 0, REG_OK,
	    0, 9, 3, 6, -1, -1, 3, 6, -1, -1, END);
  test_exec("foozapzot", 0, REG_OK,
	    0, 9, 3, 6, -1, -1, -1, -1, 3, 6, END);
  test_exec("foozot", 0, REG_OK,
	    0, 6, 3, 3, -1, -1, -1, -1, -1, -1, END);


  /* Test case where, e.g., Perl and Python regexp functions, and many
     other backtracking matchers, fail to produce the longest match.
     It is not exactly a bug since Perl does not claim to find the
     longest match, but a confusing feature and, in my opinion, a bad
     design choice because the union operator is traditionally defined
     to be commutative (with respect to the language denoted by the RE). */
  test_comp("(a|ab)(blip)?", REG_EXTENDED, 0);
  test_exec("ablip", 0, REG_OK, 0, 5, 0, 1, 1, 5, END);
  test_exec("ab", 0, REG_OK, 0, 2, 0, 2, -1, -1, END);
  test_comp("(ab|a)(blip)?", REG_EXTENDED, 0);
  test_exec("ablip", 0, REG_OK, 0, 5, 0, 1, 1, 5, END);
  test_exec("ab", 0, REG_OK, 0, 2, 0, 2, -1, -1, END);

  /* Test more submatch addressing. */
  test_comp("((a|b)*)a(a|b)*", REG_EXTENDED, 0);
  test_exec("aaaaabaaaba", 0, REG_OK, 0, 11, 0, 10, 9, 10, -1, -1, END);
  test_exec("aaaaabaaab", 0, REG_OK, 0, 10, 0, 8, 7, 8, 9, 10, END);
  test_exec("caa", 0, REG_OK, 1, 3, 1, 2, 1, 2, -1, -1, END);
  test_comp("((a|aba)*)(ababbaba)((a|b)*)", REG_EXTENDED, 0);
  test_exec("aabaababbabaaababbab", 0, REG_OK,
	    0, 20, 0, 4, 1, 4, 4, 12, 12, 20, 19, 20, END);
  test_exec("aaaaababbaba", 0, REG_OK,
	    0, 12, 0, 4, 3, 4, 4, 12, 12, 12, -1, -1, END);
  test_comp("((a|aba|abb|bba|bab)*)(ababbababbabbbabbbbbbabbaba)((a|b)*)",
	    REG_EXTENDED, 0);
  test_exec("aabaabbbbabababaababbababbabbbabbbbbbabbabababbababababbabababa",
	    0, REG_OK, 0, 63, 0, 16, 13, 16, 16, 43, 43, 63, 62, 63, END);

  /* Test for empty subexpressions. */
  test_comp("", 0, 0);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("foo", 0, REG_OK, 0, 0, END);
  test_comp("(a|)", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, END);
  test_exec("b", 0, REG_OK, 0, 0, 0, 0, END);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, END);
  test_comp("a|", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("b", 0, REG_OK, 0, 0, END);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_comp("|a", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("b", 0, REG_OK, 0, 0, END);
  test_exec("", 0, REG_OK, 0, 0, END);

  /* Miscellaneous tests. */
  test_comp("(a*)b(c*)", REG_EXTENDED, 0);
  test_exec("abc", 0, REG_OK, 0, 3, 0, 1, 2, 3, END);
  test_exec("***abc***", 0, REG_OK, 3, 6, 3, 4, 5, 6, END);
  test_comp("(a)", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, END);
  test_comp("((a))", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, 0, 1, END);
  test_comp("(((a)))", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, 0, 1, 0, 1, END);
  test_comp("((((((((((((((((((((a))))))))))))))))))))", REG_EXTENDED, 0);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
	    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
	    0, 1, 0, 1, 0, 1, END);

  test_comp("ksntoeaiksntoeaikstneoaiksnteoaiksntoeaiskntoeaiskntoekainstoei"
	    "askntoeakisntoeksaitnokesantiksoentaikosentaiksoentaiksnoeaiskn"
	    "teoaksintoekasitnoeksaitkosetniaksoetnaisknoetakistoeksintokesa"
	    "nitksoentaisknoetaisknoetiaksotneaikstoekasitoeskatioksentaikso"
	    "enatiksoetnaiksonateiksoteaeskanotisknetaiskntoeasknitoskenatis"
	    "konetaisknoteai", 0, 0);

  test_comp("((aab)|(aac)|(aa*))c", REG_EXTENDED, 0);
  test_exec("aabc", 0, REG_OK, 0, 4, 0, 3,  0,	3, -1, -1, -1, -1, END);
  test_exec("aacc", 0, REG_OK, 0, 4, 0, 3, -1, -1,  0,	3, -1, -1, END);
  test_exec("aaac", 0, REG_OK, 0, 4, 0, 3, -1, -1, -1, -1,  0,	3, END);

  test_comp("^(([^!]+!)?([^!]+)|.+!([^!]+!)([^!]+))$",
	    REG_EXTENDED, 0);
  test_exec("foo!bar!bas", 0, REG_OK,
	    0, 11, 0, 11, -1, -1, -1, -1, 4, 8, 8, 11, END);
  test_comp("^([^!]+!)?([^!]+)$|^.+!([^!]+!)([^!]+)$",
	    REG_EXTENDED, 0);
  test_exec("foo!bar!bas", 0, REG_OK,
	    0, 11, -1, -1, -1, -1, 4, 8, 8, 11, END);
  test_comp("^(([^!]+!)?([^!]+)|.+!([^!]+!)([^!]+))$",
	    REG_EXTENDED, 0);
  test_exec("foo!bar!bas", 0, REG_OK,
	    0, 11, 0, 11, -1, -1, -1, -1, 4, 8, 8, 11, END);

  test_comp("M[ou]'?am+[ae]r .*([AEae]l[- ])?[GKQ]h?[aeu]+([dtz][dhz]?)+af[iy]",
	    REG_EXTENDED, 0);
  test_exec("Muammar Quathafi", 0, REG_OK, 0, 16, -1, -1, 11, 13, END);

  test_comp("(Ab|cD)*", REG_EXTENDED | REG_ICASE, 0);
  test_exec("aBcD", 0, REG_OK, 0, 4, 2, 4, END);

  test_comp("a**", REG_EXTENDED, REG_BADRPT);
  test_comp("a*+", REG_EXTENDED, REG_BADRPT);
  test_comp("a+*", REG_EXTENDED, REG_BADRPT);
  test_comp("a++", REG_EXTENDED, REG_BADRPT);
  test_comp("a?+", REG_EXTENDED, REG_BADRPT);
  test_comp("a?*", REG_EXTENDED, REG_BADRPT);
  test_comp("a{1,2}*", REG_EXTENDED, REG_BADRPT);
  test_comp("a{1,2}+", REG_EXTENDED, REG_BADRPT);

  /*
   * Many of the following tests were mostly inspired by (or copied from) the
   * libhackerlab posix test suite by Tom Lord.
   */

  test_comp("a", 0, 0);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_comp("\\.", 0, 0);
  test_exec(".", 0, REG_OK, 0, 1, END);
  test_comp("\\[", 0, 0);
  test_exec("[", 0, REG_OK, 0, 1, END);
  test_comp("\\\\", 0, 0);
  test_exec("\\", 0, REG_OK, 0, 1, END);
  test_comp("\\*", 0, 0);
  test_exec("*", 0, REG_OK, 0, 1, END);
  test_comp("\\^", 0, 0);
  test_exec("^", 0, REG_OK, 0, 1, END);
  test_comp("\\$", 0, 0);
  test_exec("$", 0, REG_OK, 0, 1, END);

  test_comp("\\", 0, REG_EESCAPE);

  test_comp("x\\.", 0, 0);
  test_exec("x.", 0, REG_OK, 0, 2, END);
  test_comp("x\\[", 0, 0);
  test_exec("x[", 0, REG_OK, 0, 2, END);
  test_comp("x\\\\", 0, 0);
  test_exec("x\\", 0, REG_OK, 0, 2, END);
  test_comp("x\\*", 0, 0);
  test_exec("x*", 0, REG_OK, 0, 2, END);
  test_comp("x\\^", 0, 0);
  test_exec("x^", 0, REG_OK, 0, 2, END);
  test_comp("x\\$", 0, 0);
  test_exec("x$", 0, REG_OK, 0, 2, END);

  test_comp("x\\", 0, REG_EESCAPE);

  test_comp(".", 0, 0);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("\n", 0, REG_OK, 0, 1, END);

  test_comp("(+|?)", 0, 0);
  test_exec("(+|?)", 0, REG_OK, 0, 5, END);
  test_exec("+|?", 0, REG_NOMATCH);
  test_exec("(+)", 0, REG_NOMATCH);
  test_exec("+", 0, REG_NOMATCH);


  /*
   * Test bracket expressions.
   */

  test_comp("[", 0, REG_EBRACK);
  test_comp("[]", 0, REG_EBRACK);
  test_comp("[^]", 0, REG_EBRACK);

  test_comp("[]x]", 0, 0);
  test_exec("]", 0, REG_OK, 0, 1, END);
  test_exec("x", 0, REG_OK, 0, 1, END);

  test_comp("[.]", 0, 0);
  test_exec(".", 0, REG_OK, 0, 1, END);
  test_exec("a", 0, REG_NOMATCH);

  test_comp("[*]", 0, 0);
  test_exec("*", 0, REG_OK, 0, 1, END);

  test_comp("[[]", 0, 0);
  test_exec("[", 0, REG_OK, 0, 1, END);

  test_comp("[\\]", 0, 0);
  test_exec("\\", 0, REG_OK, 0, 1, END);

  test_comp("[-x]", 0, 0);
  test_exec("-", 0, REG_OK, 0, 1, END);
  test_exec("x", 0, REG_OK, 0, 1, END);
  test_comp("[x-]", 0, 0);
  test_exec("-", 0, REG_OK, 0, 1, END);
  test_exec("x", 0, REG_OK, 0, 1, END);
  test_comp("[-]", 0, 0);
  test_exec("-", 0, REG_OK, 0, 1, END);

  test_comp("[abc]", 0, 0);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("b", 0, REG_OK, 0, 1, END);
  test_exec("c", 0, REG_OK, 0, 1, END);
  test_exec("d", 0, REG_NOMATCH);
  test_exec("xa", 0, REG_OK, 1, 2, END);
  test_exec("xb", 0, REG_OK, 1, 2, END);
  test_exec("xc", 0, REG_OK, 1, 2, END);
  test_exec("xd", 0, REG_NOMATCH);
  test_comp("x[abc]", 0, 0);
  test_exec("xa", 0, REG_OK, 0, 2, END);
  test_exec("xb", 0, REG_OK, 0, 2, END);
  test_exec("xc", 0, REG_OK, 0, 2, END);
  test_exec("xd", 0, REG_NOMATCH);
  test_comp("[^abc]", 0, 0);
  test_exec("a", 0, REG_NOMATCH);
  test_exec("b", 0, REG_NOMATCH);
  test_exec("c", 0, REG_NOMATCH);
  test_exec("d", 0, REG_OK, 0, 1, END);
  test_exec("xa", 0, REG_OK, 0, 1, END);
  test_exec("xb", 0, REG_OK, 0, 1, END);
  test_exec("xc", 0, REG_OK, 0, 1, END);
  test_exec("xd", 0, REG_OK, 0, 1, END);
  test_comp("x[^abc]", 0, 0);
  test_exec("xa", 0, REG_NOMATCH);
  test_exec("xb", 0, REG_NOMATCH);
  test_exec("xc", 0, REG_NOMATCH);
  test_exec("xd", 0, REG_OK, 0, 2, END);

  test_comp("[()+?*\\]+", REG_EXTENDED, 0);
  test_exec("x\\*?+()x", 0, REG_OK, 1, 7, END);

  /* Standard character classes. */
  test_comp("[[:alnum:]]+", REG_EXTENDED, 0);
  test_exec("%abc123890XYZ=", 0, REG_OK, 1, 13, END);
  test_comp("[[:cntrl:]]+", REG_EXTENDED, 0);
  test_exec("%\n\t\015\f ", 0, REG_OK, 1, 5, END);
  test_comp("[[:lower:]]+", REG_EXTENDED, 0);
  test_exec("AbcdE", 0, REG_OK, 1, 4, END);
  test_comp("[[:lower:]]+", REG_EXTENDED | REG_ICASE, 0);
  test_exec("AbcdE", 0, REG_OK, 0, 5, END);
  test_comp("[[:space:]]+", REG_EXTENDED, 0);
  test_exec("x \t\f\nx", 0, REG_OK, 1, 5, END);
  test_comp("[[:alpha:]]+", REG_EXTENDED, 0);
  test_exec("%abC123890xyz=", 0, REG_OK, 1, 4, END);
  test_comp("[[:digit:]]+", REG_EXTENDED, 0);
  test_exec("%abC123890xyz=", 0, REG_OK, 4, 10, END);
  test_comp("[^[:digit:]]+", REG_EXTENDED, 0);
  test_exec("%abC123890xyz=", 0, REG_OK, 0, 4, END);
  test_comp("[[:print:]]+", REG_EXTENDED, 0);
  test_exec("\n\t %abC12\f", 0, REG_OK, 2, 9, END);
  test_comp("[[:upper:]]+", REG_EXTENDED, 0);
  test_exec("\n aBCDEFGHIJKLMNOPQRSTUVWXYz", 0, REG_OK, 3, 27, END);
  test_comp("[[:upper:]]+", REG_EXTENDED | REG_ICASE, 0);
  test_exec("\n aBCDEFGHIJKLMNOPQRSTUVWXYz", 0, REG_OK, 2, 28, END);
#ifdef HAVE_ISWBLANK
#ifdef HAVE_ISBLANK
  test_comp("[[:blank:]]+", REG_EXTENDED, 0);
  test_exec("\na \t b", 0, REG_OK, 2, 5, END);
#endif /* HAVE_ISBLANK */
#endif /* HAVE_ISWBLANK */
  test_comp("[[:graph:]]+", REG_EXTENDED, 0);
  test_exec("\n %abC12\f", 0, REG_OK, 2, 8, END);
  test_comp("[[:punct:]]+", REG_EXTENDED, 0);
  test_exec("a~!@#$%^&*()_+=-`[]{};':\"|\\,./?>< ",
	    0, REG_OK, 1, 33, END);
  test_comp("[[:xdigit:]]+", REG_EXTENDED, 0);
  test_exec("-0123456789ABCDEFabcdef", 0, REG_OK, 1, 23, END);
  test_comp("[[:bogus-character-class-name:]", REG_EXTENDED, REG_ECTYPE);
  test_comp("[[:\xff:", REG_EXTENDED, REG_ECTYPE);


  /* Range expressions (assuming that the C locale is being used). */
  test_comp("[a-z]+", REG_EXTENDED, 0);
  test_exec("ABCabcxyzABC", 0, REG_OK, 3, 9, END);
  test_comp("[z-a]+", REG_EXTENDED, REG_ERANGE);
  test_comp("[a-b-c]", 0, REG_ERANGE);
  test_comp("[a-a]+", REG_EXTENDED, 0);
  test_exec("zaaaaab", 0, REG_OK, 1, 6, END);
  test_comp("[--Z]+", REG_EXTENDED, 0);
  test_exec("!ABC-./XYZ~", 0, REG_OK, 1, 10, END);
  test_comp("[*--]", 0, 0);
  test_exec("-", 0, REG_OK, 0, 1, END);
  test_exec("*", 0, REG_OK, 0, 1, END);
  test_comp("[*--Z]+", REG_EXTENDED, 0);
  test_exec("!+*,---ABC", 0, REG_OK, 1, 7, END);
  test_comp("[a-]+", REG_EXTENDED, 0);
  test_exec("xa-a--a-ay", 0, REG_OK, 1, 9, END);

  /* REG_ICASE and character sets. */
  test_comp("[a-c]*", REG_ICASE | REG_EXTENDED, 0);
  test_exec("cABbage", 0, REG_OK, 0, 5, END);
  test_comp("[^a-c]*", REG_ICASE | REG_EXTENDED, 0);
  test_exec("tObAcCo", 0, REG_OK, 0, 2, END);
  test_comp("[A-C]*", REG_ICASE | REG_EXTENDED, 0);
  test_exec("cABbage", 0, REG_OK, 0, 5, END);
  test_comp("[^A-C]*", REG_ICASE | REG_EXTENDED, 0);
  test_exec("tObAcCo", 0, REG_OK, 0, 2, END);

  /* Complex character sets. */
  test_comp("[[:digit:]a-z#$%]+", REG_EXTENDED, 0);
  test_exec("__abc#lmn012$x%yz789*", 0, REG_OK, 2, 20, END);
  test_comp("[[:digit:]a-z#$%]+", REG_ICASE | REG_EXTENDED, 0);
  test_exec("__abcLMN012x%#$yz789*", 0, REG_OK, 2, 20, END);
  test_comp("[^[:digit:]a-z#$%]+", REG_EXTENDED, 0);
  test_exec("abc#lmn012$x%yz789--@*,abc", 0, REG_OK, 18, 23, END);
  test_comp("[^[:digit:]a-z#$%]+", REG_ICASE | REG_EXTENDED, 0);
  test_exec("abc#lmn012$x%yz789--@*,abc", 0, REG_OK, 18, 23, END);
  test_comp("[^[:digit:]#$%[:xdigit:]]+", REG_ICASE | REG_EXTENDED, 0);
  test_exec("abc#lmn012$x%yz789--@*,abc", 0, REG_OK, 4, 7, END);
  test_comp("[^-]+", REG_EXTENDED, 0);
  test_exec("---afd*(&,ml---", 0, REG_OK, 3, 12, END);
  test_comp("[^--Z]+", REG_EXTENDED, 0);
  test_exec("---AFD*(&,ml---", 0, REG_OK, 6, 12, END);
  test_comp("[^--Z]+", REG_ICASE | REG_EXTENDED, 0);
  test_exec("---AFD*(&,ml---", 0, REG_OK, 6, 10, END);

  /* Unsupported things (equivalence classes and multicharacter collating
     elements) */
  test_comp("[[.foo.]]", 0, REG_ECOLLATE);
  test_comp("[[=foo=]]", 0, REG_ECOLLATE);
  test_comp("[[..]]", 0, REG_ECOLLATE);
  test_comp("[[==]]", 0, REG_ECOLLATE);
  test_comp("[[.]]", 0, REG_ECOLLATE);
  test_comp("[[=]]", 0, REG_ECOLLATE);
  test_comp("[[.]", 0, REG_ECOLLATE);
  test_comp("[[=]", 0, REG_ECOLLATE);
  test_comp("[[.", 0, REG_ECOLLATE);
  test_comp("[[=", 0, REG_ECOLLATE);



  /* Miscellaneous tests. */
  test_comp("abc\\(\\(de\\)\\(fg\\)\\)hi", 0, 0);
  test_exec("xabcdefghiy", 0, REG_OK, 1, 10, 4, 8, 4, 6, 6, 8, END);

  test_comp("abc*def", 0, 0);
  test_exec("xabdefy", 0, REG_OK, 1, 6, END);
  test_exec("xabcdefy", 0, REG_OK, 1, 7, END);
  test_exec("xabcccccccdefy", 0, REG_OK, 1, 13, END);

  test_comp("abc\\(def\\)*ghi", 0, 0);
  test_exec("xabcghiy", 0, REG_OK, 1, 7, -1, -1, END);
  test_exec("xabcdefghi", 0, REG_OK, 1, 10, 4, 7, END);
  test_exec("xabcdefdefdefghi", 0, REG_OK, 1, 16, 10, 13, END);

  test_comp("a?", REG_EXTENDED, REG_OK);
  test_exec("aaaaa", 0, REG_OK, 0, 1, END);
  test_exec("xaaaaa", 0, REG_OK, 0, 0, END);
  test_comp("a+", REG_EXTENDED, REG_OK);
  test_exec("aaaaa", 0, REG_OK, 0, 5, END);
  test_exec("xaaaaa", 0, REG_OK, 1, 6, END);


  /*
   * Test anchors and their behaviour with the REG_NEWLINE compilation
   * flag and the REG_NOTBOL, REG_NOTEOL execution flags.
   */

  /* Normally, `^' matches the empty string at beginning of input.
     If REG_NOTBOL is used, `^' won't match the zero length string. */
  test_comp("^abc", 0, 0);
  test_exec("abcdef", 0, REG_OK, 0, 3, END);
  test_exec("abcdef", REG_NOTBOL, REG_NOMATCH);
  test_exec("xyzabcdef", 0, REG_NOMATCH);
  test_exec("xyzabcdef", REG_NOTBOL, REG_NOMATCH);
  test_exec("\nabcdef", 0, REG_NOMATCH);
  test_exec("\nabcdef", REG_NOTBOL, REG_NOMATCH);

  /* Normally, `$' matches the empty string at end of input.
     If REG_NOTEOL is used, `$' won't match the zero length string. */
  test_comp("abc$", 0, 0);
  test_exec("defabc", 0, REG_OK, 3, 6, END);
  test_exec("defabc", REG_NOTEOL, REG_NOMATCH);
  test_exec("defabcxyz", 0, REG_NOMATCH);
  test_exec("defabcxyz", REG_NOTEOL, REG_NOMATCH);
  test_exec("defabc\n", 0, REG_NOMATCH);
  test_exec("defabc\n", REG_NOTEOL, REG_NOMATCH);

  test_comp("^abc$", 0, 0);
  test_exec("abc", 0, REG_OK, 0, 3, END);
  test_exec("abc", REG_NOTBOL, REG_NOMATCH);
  test_exec("abc", REG_NOTEOL, REG_NOMATCH);
  test_exec("abc", REG_NOTBOL | REG_NOTEOL, REG_NOMATCH);
  test_exec("\nabc\n", 0, REG_NOMATCH);
  test_exec("defabc\n", 0, REG_NOMATCH);
  test_exec("\nabcdef", 0, REG_NOMATCH);
  test_exec("abcdef", 0, REG_NOMATCH);
  test_exec("defabc", 0, REG_NOMATCH);
  test_exec("abc\ndef", 0, REG_NOMATCH);
  test_exec("def\nabc", 0, REG_NOMATCH);

  /* If REG_NEWLINE is used, `^' matches the empty string immediately after
     a newline, regardless of whether execution flags contain REG_NOTBOL.
     Similarly, if REG_NEWLINE is used, `$' matches the empty string
     immediately before a newline, regardless of execution flags. */
  test_comp("^abc", REG_NEWLINE, 0);
  test_exec("abcdef", 0, REG_OK, 0, 3, END);
  test_exec("abcdef", REG_NOTBOL, REG_NOMATCH);
  test_exec("xyzabcdef", 0, REG_NOMATCH);
  test_exec("xyzabcdef", REG_NOTBOL, REG_NOMATCH);
  test_exec("\nabcdef", 0, REG_OK, 1, 4, END);
  test_exec("\nabcdef", REG_NOTBOL, 0, 1, 4, END);
  test_comp("abc$", REG_NEWLINE, 0);
  test_exec("defabc", 0, REG_OK, 3, 6, END);
  test_exec("defabc", REG_NOTEOL, REG_NOMATCH);
  test_exec("defabcxyz", 0, REG_NOMATCH);
  test_exec("defabcxyz", REG_NOTEOL, REG_NOMATCH);
  test_exec("defabc\n", 0, REG_OK, 3, 6, END);
  test_exec("defabc\n", REG_NOTEOL, 0, 3, 6, END);
  test_comp("^abc$", REG_NEWLINE, 0);
  test_exec("abc", 0, REG_OK, 0, 3, END);
  test_exec("abc", REG_NOTBOL, REG_NOMATCH);
  test_exec("abc", REG_NOTEOL, REG_NOMATCH);
  test_exec("abc", REG_NOTBOL | REG_NOTEOL, REG_NOMATCH);
  test_exec("\nabc\n", 0, REG_OK, 1, 4, END);
  test_exec("defabc\n", 0, REG_NOMATCH);
  test_exec("\nabcdef", 0, REG_NOMATCH);
  test_exec("abcdef", 0, REG_NOMATCH);
  test_exec("abcdef", REG_NOTBOL, REG_NOMATCH);
  test_exec("defabc", 0, REG_NOMATCH);
  test_exec("defabc", REG_NOTEOL, REG_NOMATCH);
  test_exec("abc\ndef", 0, REG_OK, 0, 3, END);
  test_exec("abc\ndef", REG_NOTBOL, REG_NOMATCH);
  test_exec("abc\ndef", REG_NOTEOL, 0, 0, 3, END);
  test_exec("abc\ndef", REG_NOTBOL | REG_NOTEOL, REG_NOMATCH);
  test_exec("def\nabc", 0, REG_OK, 4, 7, END);
  test_exec("def\nabc", REG_NOTBOL, 0, 4, 7, END);
  test_exec("def\nabc", REG_NOTEOL, REG_NOMATCH);
  test_exec("def\nabc", REG_NOTBOL | REG_NOTEOL, REG_NOMATCH);

  /* With BRE syntax, `^' has a special meaning only at the beginning of the
     RE or the beginning of a parenthesized subexpression. */
  test_comp("a\\{0,1\\}^bc", 0, 0);
  test_exec("bc", 0, REG_NOMATCH);
  test_exec("^bc", 0, REG_OK, 0, 3, END);
  test_exec("abc", 0, REG_NOMATCH);
  test_exec("a^bc", 0, REG_OK, 0, 4, END);
  test_comp("a\\{0,1\\}\\(^bc\\)", 0, 0);
  test_exec("bc", 0, REG_OK, 0, 2, 0, 2, END);
  test_exec("^bc", 0, REG_NOMATCH);
  test_exec("abc", 0, REG_NOMATCH);
  test_exec("a^bc", 0, REG_NOMATCH);
  test_comp("(^a", 0, 0);
  test_exec("(^a", 0, REG_OK, 0, 3, END);

  /* With BRE syntax, `$' has a special meaning only at the end of the
     RE or the end of a parenthesized subexpression. */
  test_comp("ab$c\\{0,1\\}", 0, 0);
  test_exec("ab", 0, REG_NOMATCH);
  test_exec("ab$", 0, REG_OK, 0, 3, END);
  test_exec("abc", 0, REG_NOMATCH);
  test_exec("ab$c", 0, REG_OK, 0, 4, END);
  test_comp("\\(ab$\\)c\\{0,1\\}", 0, 0);
  test_exec("ab", 0, REG_OK, 0, 2, 0, 2, END);
  test_exec("ab$", 0, REG_NOMATCH);
  test_exec("abc", 0, REG_NOMATCH);
  test_exec("ab$c", 0, REG_NOMATCH);
  test_comp("a$)", 0, 0);
  test_exec("a$)", 0, REG_OK, 0, 3, END);

  /* Miscellaneous tests for `^' and `$'. */
  test_comp("foo^$", REG_EXTENDED, 0);
  test_exec("foo", 0, REG_NOMATCH);
  test_comp("x$\n^y", REG_EXTENDED | REG_NEWLINE, 0);
  test_exec("foo\nybarx\nyes\n", 0, REG_OK, 8, 11, END);
  test_comp("^$", 0, 0);
  test_exec("x", 0, REG_NOMATCH);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("\n", 0, REG_NOMATCH);
  test_comp("^$", REG_NEWLINE, 0);
  test_exec("x", 0, REG_NOMATCH);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("\n", 0, REG_OK, 0, 0, END);

  /* REG_NEWLINE causes `.' not to match newlines. */
  test_comp(".*", 0, 0);
  test_exec("ab\ncd", 0, REG_OK, 0, 5, END);
  test_comp(".*", REG_NEWLINE, 0);
  test_exec("ab\ncd", 0, REG_OK, 0, 2, END);

  /*
   * Tests for nonstandard syntax extensions.
   */

  /* Zero width assertions. */
  test_comp("\\<x", REG_EXTENDED, 0);
  test_exec("aax xaa", 0, REG_OK, 4, 5, END);
  test_exec("xaa", 0, REG_OK, 0, 1, END);
  test_comp("x\\>", REG_EXTENDED, 0);
  test_exec("axx xaa", 0, REG_OK, 2, 3, END);
  test_exec("aax", 0, REG_OK, 2, 3, END);
  test_comp("\\bx", REG_EXTENDED, 0);
  test_exec("axx xaa", 0, REG_OK, 4, 5, END);
  test_exec("aax", 0, REG_NOMATCH);
  test_exec("xax", 0, REG_OK, 0, 1, END);
  test_comp("x\\b", REG_EXTENDED, 0);
  test_exec("axx xaa", 0, REG_OK, 2, 3, END);
  test_exec("aax", 0, REG_OK, 2, 3, END);
  test_exec("xaa", 0, REG_NOMATCH);
  test_comp("\\Bx", REG_EXTENDED, 0);
  test_exec("aax xxa", 0, REG_OK, 2, 3, END);
  test_comp("\\Bx\\b", REG_EXTENDED, 0);
  test_exec("aax xxx", 0, REG_OK, 2, 3, END);
  test_comp("\\<.", REG_EXTENDED, 0);
  test_exec(";xaa", 0, REG_OK, 1, 2, END);

  /* Shorthands for character classes. */
  test_comp("\\w+", REG_EXTENDED, 0);
#ifdef SRC_IN_ISO_8859_1
  test_exec(",.(a23_Nt-ˆo)", 0, REG_OK, 3, 9, END);
#else
#ifdef SRC_IN_UTF_8
  /* iconv -f ISO-8859-1 -t UTF-8 file_with_lines_above > www_utf_8 */
  test_exec(",.(a23_Nt-√∂o)", 0, REG_OK, 3, 9, END);
#else
  unsigned char str_000[] = {
    ',','.','(','a','2','3','_','N','t','-',0xF6,'o',0x00
  };
  test_exec((char const *)str_000, 0, REG_OK, 3, 9, END);
#endif
#endif
  test_comp("\\d+", REG_EXTENDED, 0);
  test_exec("uR120_4=v4", 0, REG_OK, 2, 5, END);
  test_comp("\\D+", REG_EXTENDED, 0);
  test_exec("120d_=vA4s", 0, REG_OK, 3, 8, END);

  /* Quoted special characters. */
  test_comp("\\t", REG_EXTENDED, 0);
  test_comp("\\e", REG_EXTENDED, 0);

  /* Test the \x1B and \x{263a} extensions for specifying 8 bit and wide
     characters in hexadecimal. */
  test_comp("\\x41", REG_EXTENDED, 0);
  test_exec("ABC", 0, REG_OK, 0, 1, END);
  test_comp("\\x5", REG_EXTENDED, 0);
  test_exec("\005", 0, REG_OK, 0, 1, END);
  test_comp("\\x5r", REG_EXTENDED, 0);
  test_exec("\005r", 0, REG_OK, 0, 2, END);
  test_comp("\\x", REG_EXTENDED, 0);
  test_nexec("\000", 1, 0, REG_OK, 0, 1, END);
  test_comp("\\xr", REG_EXTENDED, 0);
  test_nexec("\000r", 2, 0, REG_OK, 0, 2, END);
  test_comp("\\x{41}", REG_EXTENDED, 0);
  test_exec("ABC", 0, REG_OK, 0, 1, END);
  test_comp("\\x{5}", REG_EXTENDED, 0);
  test_exec("\005", 0, REG_OK, 0, 1, END);
  test_comp("\\x{5}r", REG_EXTENDED, 0);
  test_exec("\005r", 0, REG_OK, 0, 2, END);
  test_comp("\\x{}", REG_EXTENDED, 0);
  test_nexec("\000", 1, 0, REG_OK, 0, 1, END);
  test_comp("\\x{}r", REG_EXTENDED, 0);
  test_nexec("\000r", 2, 0, REG_OK, 0, 2, END);
  test_comp("\\x{00000000}", REG_EXTENDED, 0);
  test_comp("\\x{000000000}", REG_EXTENDED, REG_EBRACE);

  /* Tests for (?inrU-inrU) and (?inrU-inrU:) */
  test_comp("foo(?i)bar", REG_EXTENDED, 0);
  test_exec("fooBaR", 0, REG_OK, 0, 6, END);
  test_comp("foo(?i)bar|zap", REG_EXTENDED, 0);
  test_exec("fooBaR", 0, REG_OK, 0, 6, END);
  test_exec("foozap", 0, REG_OK, 0, 6, END);
  test_exec("foozAp", 0, REG_OK, 0, 6, END);
  test_exec("zap", 0, REG_NOMATCH);
  test_comp("foo(?-i:zap)zot", REG_EXTENDED | REG_ICASE, 0);
  test_exec("FoOzapZOt", 0, REG_OK, 0, 9, END);
  test_exec("FoOzApZOt", 0, REG_NOMATCH);
  test_comp("foo(?i:bar|zap)", REG_EXTENDED, 0);
  test_exec("foozap", 0, REG_OK, 0, 6, END);
  test_exec("foobar", 0, REG_OK, 0, 6, END);
  test_exec("foobAr", 0, REG_OK, 0, 6, END);
  test_exec("fooZaP", 0, REG_OK, 0, 6, END);
  test_comp("foo(?U:o*)(o*)", REG_EXTENDED, 0);
  test_exec("foooo", 0, REG_OK, 0, 5, 3, 5, END);

  /* Test comment syntax. */
  test_comp("foo(?# This here is a comment. )bar", REG_EXTENDED, 0);
  test_exec("foobar", 0, REG_OK, 0, 6, END);

  /* Tests for \Q and \E. */
  test_comp("\\((\\Q)?:\\<[^$\\E)", REG_EXTENDED, 0);
  test_exec("()?:\\<[^$", 0, REG_OK, 0, 9, 1, 9, END);
  test_comp("\\Qabc\\E.*", REG_EXTENDED, 0);
  test_exec("abcdef", 0, REG_OK, 0, 6, END);
  test_comp("\\Qabc\\E.*|foo", REG_EXTENDED, 0);
  test_exec("parabc123wxyz", 0, REG_OK, 3, 13, END);
  test_exec("fooabc123wxyz", 0, REG_OK, 0, 3, END);

  /*
   * Test integer parser used for bounded repititions.
   */

  test_comp("a{9223372036854775808,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{9223372036854775808}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{9223372036854775807,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{9223372036854775807}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{2147483648,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{2147483648}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{2147483647,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{2147483647}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{32768,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{32768}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{32767,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{32767}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{256,}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{256}", REG_EXTENDED, REG_BADMAX);
  test_comp("a{255,}", REG_EXTENDED, REG_OK);
  test_comp("a{255}", REG_EXTENDED, REG_OK);

  /*
   * Test bounded repetitions.
   */

  test_comp("a{0,0}", REG_EXTENDED, REG_OK);
  test_exec("aaa", 0, REG_OK, 0, 0, END);
  test_comp("a{0,1}", REG_EXTENDED, REG_OK);
  test_exec("aaa", 0, REG_OK, 0, 1, END);
  test_comp("a{1,1}", REG_EXTENDED, REG_OK);
  test_exec("aaa", 0, REG_OK, 0, 1, END);
  test_comp("a{1,3}", REG_EXTENDED, REG_OK);
  test_exec("xaaaaa", 0, REG_OK, 1, 4, END);
  test_comp("a{0,3}", REG_EXTENDED, REG_OK);
  test_exec("aaaaa", 0, REG_OK, 0, 3, END);
  test_comp("a{0,}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, END);
  test_comp("a{1,}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_NOMATCH);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, END);
  test_comp("a{2,}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_NOMATCH);
  test_exec("a", 0, REG_NOMATCH);
  test_exec("aa", 0, REG_OK, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, END);
  test_comp("a{3,}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_NOMATCH);
  test_exec("a", 0, REG_NOMATCH);
  test_exec("aa", 0, REG_NOMATCH);
  test_exec("aaa", 0, REG_OK, 0, 3, END);
  test_exec("aaaa", 0, REG_OK, 0, 4, END);
  test_exec("aaaaa", 0, REG_OK, 0, 5, END);
  test_exec("aaaaaa", 0, REG_OK, 0, 6, END);
  test_exec("aaaaaaa", 0, REG_OK, 0, 7, END);
  test_comp("a{,}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("aaa", 0, REG_OK, 0, 3, END);
  test_comp("a{,0}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("aaa", 0, REG_OK, 0, 0, END);
  test_comp("a{,1}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 1, END);
  test_comp("a{,2}", REG_EXTENDED, REG_OK);
  test_exec("", 0, REG_OK, 0, 0, END);
  test_exec("a", 0, REG_OK, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 2, END);

  test_comp("a{5,10}", REG_EXTENDED, REG_OK);
  test_comp("a{6,6}", REG_EXTENDED, REG_OK);
  test_exec("aaaaaaaaaaaa", 0, REG_OK, 0, 6, END);
  test_exec("xxaaaaaaaaaaaa", 0, REG_OK, 2, 8, END);
  test_exec("xxaaaaa", 0, REG_NOMATCH);
  test_comp("a{5,6}", REG_EXTENDED, REG_OK);
  test_exec("aaaaaaaaaaaa", 0, REG_OK, 0, 6, END);
  test_exec("xxaaaaaaaaaaaa", 0, REG_OK, 2, 8, END);
  test_exec("xxaaaaa", 0, REG_OK, 2, 7, END);
  test_exec("xxaaaa", 0, REG_NOMATCH);

  /* Trickier ones... */
  test_comp("([ab]{5,10})*b", REG_EXTENDED, REG_OK);
  test_exec("bbbbbabaaaaab", 0, REG_OK, 0, 13, 5, 12, END);
  test_exec("bbbbbbaaaaab", 0, REG_OK, 0, 12, 5, 11, END);
  test_exec("bbbbbbaaaab", 0, REG_OK, 0, 11, 0, 10, END);
  test_exec("bbbbbbaaab", 0, REG_OK, 0, 10, 0, 9, END);
  test_exec("bbbbbbaab", 0, REG_OK, 0, 9, 0, 8, END);
  test_exec("bbbbbbab", 0, REG_OK, 0, 8, 0, 7, END);

  test_comp("([ab]*)(ab[ab]{5,10})ba", REG_EXTENDED, REG_OK);
  test_exec("abbabbbabaabbbbbbbbbbbbbabaaaabab", 0, REG_OK,
	    0, 10, 0, 0, 0, 8, END);
  test_exec("abbabbbabaabbbbbbbbbbbbabaaaaabab", 0, REG_OK,
	    0, 32, 0, 23, 23, 30, END);
  test_exec("abbabbbabaabbbbbbbbbbbbabaaaabab", 0, REG_OK,
	    0, 24, 0, 10, 10, 22, END);
  test_exec("abbabbbabaabbbbbbbbbbbba", 0, REG_OK,
	    0, 24, 0, 10, 10, 22, END);

  test_comp("^((a{1,2})?x)*y", REG_EXTENDED | REG_NOSUB, REG_OK);
  test_exec("y", 0, REG_OK, END);
  test_exec("xy", 0, REG_OK, END);
  test_exec("axy", 0, REG_OK, END);
  test_exec("aaxy", 0, REG_OK, END);
  test_exec("aaaxy", 0, REG_NOMATCH, END);

  /* Test repeating something that has submatches inside. */
  test_comp("(a){0,5}", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, -1, -1, END);
  test_exec("a", 0, REG_OK, 0, 1, 0, 1, END);
  test_exec("aa", 0, REG_OK, 0, 2, 1, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, 2, 3, END);
  test_exec("aaaa", 0, REG_OK, 0, 4, 3, 4, END);
  test_exec("aaaaa", 0, REG_OK, 0, 5, 4, 5, END);
  test_exec("aaaaaa", 0, REG_OK, 0, 5, 4, 5, END);

  test_comp("(a){2,3}", REG_EXTENDED, 0);
  test_exec("", 0, REG_NOMATCH);
  test_exec("a", 0, REG_NOMATCH);
  test_exec("aa", 0, REG_OK, 0, 2, 1, 2, END);
  test_exec("aaa", 0, REG_OK, 0, 3, 2, 3, END);
  test_exec("aaaa", 0, REG_OK, 0, 3, 2, 3, END);

  test_comp("\\(a\\)\\{4\\}", 0, 0);
  test_exec("aaaa", 0, REG_OK, 0, 4, 3, 4, END);

  test_comp("\\(a*\\)\\{2\\}", 0, 0);
  test_exec("a", 0, REG_OK, 0, 1, 1, 1, END);

  test_comp("((..)|(.)){2}", REG_EXTENDED, 0);
  test_exec("aa", 0, REG_OK, 0, 2, 1, 2, -1, -1, 1, 2, END);

  /* Nested repeats. */
  test_comp("(.){2}{3}", REG_EXTENDED, 0);
  test_exec("xxxxx", 0, REG_NOMATCH);
  test_exec("xxxxxx", 0, REG_OK, 0, 6, 5, 6, END);
  test_comp("(..){2}{3}", REG_EXTENDED, 0);
  test_exec("xxxxxxxxxxx", 0, REG_NOMATCH);
  test_exec("xxxxxxxxxxxx", 0, REG_OK, 0, 12, 10, 12, END);
  test_comp("((..){2}.){3}", REG_EXTENDED, 0);
  test_exec("xxxxxxxxxxxxxx", 0, REG_NOMATCH);
  test_exec("xxxxxxxxxxxxxxx", 0, REG_OK, 0, 15, 10, 15, 12, 14, END);
  test_comp("((..){1,2}.){3}", REG_EXTENDED, 0);
  test_exec("xxxxxxxx", 0, REG_NOMATCH);
  test_exec("xxxxxxxxx", 0, REG_OK, 0, 9, 6, 9, 6, 8, END);
  test_exec("xxxxxxxxxx", 0, REG_OK, 0, 9, 6, 9, 6, 8, END);
  test_exec("xxxxxxxxxxx", 0, REG_OK, 0, 11, 8, 11, 8, 10, END);
  test_comp("a{2}{2}x", REG_EXTENDED, 0);
  test_exec("", 0, REG_NOMATCH);
  test_exec("x", 0, REG_NOMATCH);
  test_exec("ax", 0, REG_NOMATCH);
  test_exec("aax", 0, REG_NOMATCH);
  test_exec("aaax", 0, REG_NOMATCH);
  test_exec("aaaax", 0, REG_OK, 0, 5, END);
  test_exec("aaaaax", 0, REG_OK, 1, 6, END);
  test_exec("aaaaaax", 0, REG_OK, 2, 7, END);
  test_exec("aaaaaaax", 0, REG_OK, 3, 8, END);
  test_exec("aaaaaaaax", 0, REG_OK, 4, 9, END);

  /* Repeats with iterations inside. */
  test_comp("([a-z]+){2,5}", REG_EXTENDED, 0);
  test_exec("a\n", 0, REG_NOMATCH);
  test_exec("aa\n", 0, REG_OK, 0, 2, 1, 2, END);

  /* Multiple repeats in one regexp. */
  test_comp("a{3}b{3}", REG_EXTENDED, 0);
  test_exec("aaabbb", 0, REG_OK, 0, 6, END);
  test_exec("aaabbbb", 0, REG_OK, 0, 6, END);
  test_exec("aaaabbb", 0, REG_OK, 1, 7, END);
  test_exec("aabbb", 0, REG_NOMATCH);
  test_exec("aaabb", 0, REG_NOMATCH);

  /* Test that different types of repetitions work correctly when used
     in the same regexp.  */
  test_comp("a{2}{2}xb+xc*xd?x", REG_EXTENDED, 0);
  test_exec("aaaaxbxcxdx", 0, REG_OK, 0, 11, END);
  test_exec("aaaxbxcxdx", 0, REG_NOMATCH);
  test_exec("aabxcxdx", 0, REG_NOMATCH);
  test_exec("aaaacxdx", 0, REG_NOMATCH);
  test_exec("aaaaxbdx", 0, REG_NOMATCH);
  test_comp("^!packet [0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3} [0-9]+",
	    REG_EXTENDED, 0);
  test_exec("!packet 10.0.2.4 12765 ei voittoa", 0, REG_OK, 0, 22, END);

  /*
   * Back referencing tests.
   */
  test_comp("([a-z]*) \\1", REG_EXTENDED, 0);
  test_exec("foobar foobar", 0, REG_OK, 0, 13, 0, 6, END);

  /* Searching for a leftmost longest square (repeated string) */
  test_comp("(.*)\\1", REG_EXTENDED, 0);
  test_exec("foobarfoobar", 0, REG_OK, 0, 12, 0, 6, END);

  test_comp("a(b)*c\\1", REG_EXTENDED, 0);
  test_exec("acb", 0, REG_OK, 0, 2, -1, -1, END);
  test_exec("abbcbbb", 0, REG_OK, 0, 5, 2, 3, END);
  test_exec("abbdbd", 0, REG_NOMATCH);

  test_comp("([a-c]*)\\1", REG_EXTENDED, 0);
  test_exec("abcacdef", 0, REG_OK, 0, 0, 0, 0, END);
  test_exec("abcabcabcd", 0, REG_OK, 0, 6, 0, 3, END);
  test_comp("(.{1,3})\\1", REG_EXTENDED, 0);
  test_exec("foo", 0, REG_OK, 1, 3, 1, 2, END);

  test_comp("\\(a*\\)*\\(x\\)\\(\\1\\)", 0, 0);
  test_exec("x", 0, REG_OK, 0, 1, 0, 0, 0, 1, 1, 1, END);
#if KNOWN_BUG
  test_exec("ax", 0, REG_OK, 0, 2, 1, 1, 1, 2, 2, 2, END);
#endif

  test_comp("(a)\\1{1,2}", REG_EXTENDED, 0);
  test_exec("aabc", 0, REG_OK, 0, 2, 0, 1, END);

  test_comp("((.*)\\1)+", REG_EXTENDED, 0);
  test_exec("aa", 0, REG_OK, 0, 2, 0, 2, 0, 1, END);

#if KNOWN_BUG
  test_comp("()(\\1\\1)*", REG_EXTENDED, 0);
  test_exec("", 0, REG_OK, 0, 0, 0, 0, 0, 0, END);
#endif

  /* Check that back references work with REG_NOSUB. */
  test_comp("(o)\\1", REG_EXTENDED | REG_NOSUB, 0);
  test_exec("foobar", 0, REG_OK, END);
  test_comp("(o)\\1", REG_EXTENDED, 0);
  test_exec("foobar", 0, REG_OK, 1, 3, 1, 2, END);
  test_comp("(o)\\1", REG_EXTENDED, 0);
  test_exec("fobar", 0, REG_NOMATCH);

  test_comp("\\1foo", REG_EXTENDED, REG_ESUBREG);
  test_comp("\\1foo(bar)", REG_EXTENDED, 0);

  /* Back reference with zero-width assertion. */
  test_comp("(.)\\1$", REG_EXTENDED, 0);
  test_exec("foox", 0, REG_NOMATCH);
  test_exec("foo", 0, REG_OK, 1, 3, 1, 2, END);

  /* Back references together with {}. */
  test_comp("([0-9]{5})\\1", REG_EXTENDED, 0);
  test_exec("12345", 0, REG_NOMATCH);
  test_exec("1234512345", 0, REG_OK, 0, 10, 0, 5, END);
  test_comp("([0-9]{4})\\1", REG_EXTENDED, 0);
  test_exec("1234", 0, REG_NOMATCH);
  test_exec("12341234", 0, REG_OK, 0, 8, 0, 4, END);

  /*
   * Test minimal repetitions (non-greedy repetitions)
   */
  avoid_eflags = REG_BACKTRACKING_MATCHER | REG_APPROX_MATCHER;

  /* Basic .*/
  test_comp(".*?", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 0, END);
  test_comp(".+?", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 1, END);
  test_comp(".??", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 0, END);
  test_comp(".{2,5}?", REG_EXTENDED, 0);
  test_exec("abcd", 0, REG_OK, 0, 2, END);

  /* More complicated. */
  test_comp("<b>(.*?)</b>", REG_EXTENDED, 0);
  test_exec("<b>text1</b><b>text2</b>", 0, REG_OK, 0, 12, 3, 8, END);
  test_comp("a(.*?)(foo|bar|zap)", REG_EXTENDED, 0);
  test_exec("hubba wooga-booga zabar gafoo wazap", 0, REG_OK,
	    4, 23, 5, 20, 20, 23, END);

  /* Test REG_UNGREEDY. */
  test_comp(".*", REG_EXTENDED | REG_UNGREEDY, 0);
  test_exec("abcd", 0, REG_OK, 0, 0, END);
  test_comp(".*?", REG_EXTENDED | REG_UNGREEDY, 0);
  test_exec("abcd", 0, REG_OK, 0, 4, END);

  avoid_eflags = 0;


  /*
   * Error reporting tests.
   */

  test_comp("\\", REG_EXTENDED, REG_EESCAPE);
  test_comp("\\\\", REG_EXTENDED, REG_OK);
  test_exec("\\", 0, REG_OK, 0, 1, END);
  test_comp("(", REG_EXTENDED, REG_EPAREN);
  test_comp("(aaa", REG_EXTENDED, REG_EPAREN);
  test_comp(")", REG_EXTENDED, REG_OK);
  test_exec(")", 0, REG_OK, 0, 1, END);
  test_comp("a{1", REG_EXTENDED, REG_EBRACE);
  test_comp("a{1,x}", REG_EXTENDED, REG_BADBR);
  test_comp("a{1x}", REG_EXTENDED, REG_BADBR);
  test_comp("a{1,0}", REG_EXTENDED, REG_BADBR);
  test_comp("a{x}", REG_EXTENDED, REG_BADBR);
  test_comp("a{}", REG_EXTENDED, REG_BADBR);


  test_comp("\\", 0, REG_EESCAPE);
  test_comp("\\(", 0, REG_EPAREN);
  test_comp("\\)", 0, REG_EPAREN);
  test_comp("a\\{1", 0, REG_EBRACE);
  test_comp("a\\{1,x\\}", 0, REG_BADBR);
  test_comp("a\\{1x\\}", 0, REG_BADBR);
  test_comp("a\\{1,0\\}", 0, REG_BADBR);
  test_comp("a\\{x\\}", 0, REG_BADBR);
  test_comp("a\\{\\}", 0, REG_BADBR);
  test_comp("a\\{1,256\\}", 0, REG_BADMAX);


  test_comp(NULL, REG_BASIC, REG_OK);
  test_comp(NULL, REG_EXTENDED, REG_OK);


  /*
   * Internationalization tests.
   */

  /* This same test with the correct locale is below.
     TBR: This is a guess for the source encoding, see comments below after the locale is set to a Japanese locale. */
#ifdef SRC_IN_EUC_JP
  test_comp("µ°+", REG_EXTENDED, 0);
  test_exec("§≥§Œæﬁ§œ°¢µ°°¶Õ¯ ÿ¿≠°¶•ª•≠",
            0, REG_OK, 10, 13, END);
#else
#ifdef SRC_IN_UTF_8
  /* iconv -f EUC_JP -t UTF-8 file_with_lines_above > zzz_utf_8
     This may be incorrect because the match results might be incorrect for UTF-8, I (TBR) just don't know enough to be certain.
     It compiles and runs successfully on my desktop with the C.UTF-8 locale. */
  test_comp("Ê©ü+", REG_EXTENDED, 0);
  test_exec("„Åì„ÅÆË≥û„ÅØ„ÄÅÊ©ü„ÉªÂà©‰æøÊÄß„Éª„Çª„Ç≠",
            0, REG_OK, 15, 18, END);
#else
  /* Represent the test strings as a sequence of bytes so we don't run afoul of the compiler's expected source-charset. */
  unsigned char str_001[] = {
    0xB5,0xA1,'+',0x00
  };
  unsigned char str_002[] = {
    0xA4,0xB3,0xA4,0xCE,0xBE,0xDE,0xA4,0xCF,0xA1,0xA2,0xB5,0xA1,0xA1,0xA6,0xCD,0xF8,0xCA,0xD8,0xC0,0xAD,0xA1,0xA6,0xA5,0xBB,0xA5,0xAD,0x00
  };
  test_comp((char const *)str_001, REG_EXTENDED, 0);
  test_exec((char const *)str_002, 0, REG_OK, 10, 13, END);
#endif
#endif

#if !defined(WIN32) && !defined(__OpenBSD__)
  if (setlocale(LC_CTYPE, "en_US.ISO-8859-1") != NULL ||
      setlocale(LC_CTYPE, "en_US.ISO8859-1") != NULL)
    {
      fprintf(outf, "\nTesting LC_CTYPE en_US.ISO-8859-1\n");
#ifdef SRC_IN_ISO_8859_1
      test_comp("aBCdeFghiJKlmnoPQRstuvWXyZÂ‰ˆ", REG_ICASE, 0);
      test_exec("abCDefGhiJKlmNoPqRStuVwXyz≈ƒ÷", 0, REG_OK, 0, 29, END);
#else
#ifdef SRC_IN_UTF_8
      /* iconv -f ISO-8859-1 -t UTF-8 file_with_lines_above > yyy_utf_8 */
      /* This fails with no match on freebsd, but succeeds in linux. */
      test_comp("aBCdeFghiJKlmnoPQRstuvWXyZ√•√§√∂", REG_ICASE, 0);
      test_exec("abCDefGhiJKlmNoPqRStuVwXyz√Ö√Ñ√ñ", 0, REG_OK, 0, 29, END);
#else
      /* Represent the test strings as a sequence of bytes so we don't run afoul of the compiler's expected source-charset. */
      unsigned char str_003[] = {
        'a','B','C','d','e','F','g','h','i','J','K','l','m','n','o','P','Q','R','s','t','u','v','W','X','y','Z',0xE5,0xE4,0xF6,0x00
      };
      unsigned char str_004[] = {
        'a','b','C','D','e','f','G','h','i','J','K','l','m','N','o','P','q','R','S','t','u','V','w','X','y','z',0xC5,0xC4,0xD6,0x00
      };
      test_comp((char const *)str_003, REG_ICASE, 0);
      test_exec((char const *)str_004, 0, REG_OK, 0, 29, END);
#endif
#endif
    }

#ifdef TRE_MULTIBYTE
  if (setlocale(LC_CTYPE, "ja_JP.eucjp") != NULL ||
      setlocale(LC_CTYPE, "ja_JP.eucJP") != NULL)
    {
      fprintf(outf, "\nTesting LC_CTYPE ja_JP.eucjp\n");
      /* I tried to make a test where implementations not aware of multibyte
	 character sets will fail.  I have no idea what the japanese text here
	 means, I took it from http://www.ipsec.co.jp/. */
      /* TBR 2023/03/22: iconv has (at least) the following encoding names for Japanese:
           EUC-JIS-2004 EUC-JISX0213
           EUC-JP-MS EUCJP-MS EUCJP-OPEN EUCJP-WIN EUCJPMS
           EUC-JP CSEUCPKDFMTJAPANESE EUCJP IBM-EUCJP
           ISO-2022-JP-1 ISO2022-JP1
           ISO-2022-JP-2 CSISO2022JP2 ISO2022-JP2 ISO-2022-JP-2004 ISO-2022-JP-3 ISO2022-JP2004 ISO2022-JP3
           ISO-2022-JP CSISO2022JP ISO2022-JP
         Both iconv arguments of EUC-JP and EUC-JP-MS produced the converted strings below,
         all the others I tried resulted in invalid characters.  So guess at EUC-JP.
         If anyone knows what the encoding actually was, feel free to let me know at tbr at acm dot org :). */
#ifdef SRC_IN_EUC_JP
      test_comp("µ°+", REG_EXTENDED, 0);
      test_exec("§≥§Œæﬁ§œ°¢µ°°¶Õ¯ ÿ¿≠°¶•ª•≠", 0, REG_OK, 10, 12, END);
#else
#ifdef SRC_IN_UTF_8
      /* iconv -f EUC_JP -t UTF-8 file_with_lines_above > zzz_utf_8
         This may fail because the match results might be incorrect for UTF-8, I (TBR) just don't know enough to be certain.
         It compiles and runs successfully on my desktop with the C.UTF-8 locale. */
      test_comp("Ê©ü+", REG_EXTENDED, 0);
      test_exec("„Åì„ÅÆË≥û„ÅØ„ÄÅÊ©ü„ÉªÂà©‰æøÊÄß„Éª„Çª„Ç≠", 0, REG_OK, 10, 12, END);
#else
      /* Represent the test strings as a sequence of bytes so we don't run afoul of the compiler's expected source-charset. */
      /* This test uses the same strings (str_001 and str_002) as above, now with a Japanese locale.
         NOTE THE DIFFERENCE IN MATCH RESULTS - (10,13) earlier with the default locale, and (10,12) here with the Japanese locale. */
      test_comp((char const *)str_001, REG_EXTENDED, 0);
      test_exec((char const *)str_002, 0, REG_OK, 10, 12, END);
#endif
#endif
      test_comp("a", REG_EXTENDED, 0);
      test_nexec("foo\000bar", 7, 0, REG_OK, 5, 6, END);
      test_comp("c$", REG_EXTENDED, 0);
      test_exec("abc", 0, REG_OK, 2, 3, END);
    }
  else
    {
      fprintf(outf, "\nTRE_MULTIBYTE enabled, but skipping LC_CTYPE ja_JP.eucJP (locale unavailable)\n");
    }
#endif /* TRE_MULTIBYTE */
#endif

  tre_regfree(&reobj);

  fprintf(outf, "\n");
  if (comp_errors || exec_errors)
    fprintf(outf, "%d (%d + %d) out of %d tests FAILED!\n",
	   comp_errors + exec_errors, comp_errors, exec_errors,
	   comp_tests + exec_tests);
  else
    fprintf(outf, "All %d tests passed.\n", comp_tests + exec_tests);


#ifdef MALLOC_DEBUGGING
  if (xmalloc_dump_leaks())
    return 1;
#endif /* MALLOC_DEBUGGING */

  return comp_errors || exec_errors;
}

/* EOF */
