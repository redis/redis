/*
  test-literal-opt.c - Validate TRE literal optimization against the
  generic matcher.

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "tre-internal.h"

#define PMATCH_SLOTS 4
#define RC_ANY -9999

typedef struct {
  const char *name;
  const char *pattern;
  size_t pattern_len;
  int cflags;
  const char *string;
  size_t string_len;
  int eflags;
  int expected_rc;
  tre_literal_opt_mode_t expected_mode;
} litopt_case_t;

static void
init_pmatch(regmatch_t pmatch[], size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    {
      pmatch[i].rm_so = 111;
      pmatch[i].rm_eo = 222;
    }
}

static int
same_pmatch(const regmatch_t a[], const regmatch_t b[], size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    if (a[i].rm_so != b[i].rm_so || a[i].rm_eo != b[i].rm_eo)
      return 0;
  return 1;
}

static int
pmatch_cleared(const regmatch_t pmatch[], size_t count)
{
  size_t i;

  for (i = 0; i < count; i++)
    if (pmatch[i].rm_so != -1 || pmatch[i].rm_eo != -1)
      return 0;
  return 1;
}

static int
run_case(const litopt_case_t *tc)
{
  regex_t preg;
  tre_tnfa_t *tnfa;
  regmatch_t fast[PMATCH_SLOTS], slow[PMATCH_SLOTS];
  tre_literal_opt_mode_t saved_mode;
  char errbuf[256];
  int errcode, fast_rc, slow_rc;

  memset(&preg, 0, sizeof(preg));
  errcode = tre_regncompb(&preg, tc->pattern, tc->pattern_len, tc->cflags);
  if (errcode != REG_OK)
    {
      tre_regerror(errcode, &preg, errbuf, sizeof(errbuf));
      fprintf(stderr, "%s: compile failed: %s\n", tc->name, errbuf);
      return 1;
    }

  tnfa = (tre_tnfa_t *)preg.value;
  if (tnfa->literal_opt.mode != tc->expected_mode)
    {
      fprintf(stderr, "%s: optimizer mode %d, expected %d\n",
              tc->name, (int)tnfa->literal_opt.mode, (int)tc->expected_mode);
      tre_regfree(&preg);
      return 1;
    }

  init_pmatch(fast, PMATCH_SLOTS);
  init_pmatch(slow, PMATCH_SLOTS);

  fast_rc = tre_regnexecb(&preg, tc->string, tc->string_len,
                          PMATCH_SLOTS, fast, tc->eflags);

  saved_mode = tnfa->literal_opt.mode;
  tnfa->literal_opt.mode = TRE_LITERAL_OPT_NONE;
  slow_rc = tre_regnexecb(&preg, tc->string, tc->string_len,
                          PMATCH_SLOTS, slow, tc->eflags);
  tnfa->literal_opt.mode = saved_mode;

  if (fast_rc != slow_rc)
    {
      fprintf(stderr, "%s: fast rc %d, slow rc %d\n",
              tc->name, fast_rc, slow_rc);
      tre_regfree(&preg);
      return 1;
    }

  if (tc->expected_rc != RC_ANY && fast_rc != tc->expected_rc)
    {
      fprintf(stderr, "%s: rc %d, expected %d\n",
              tc->name, fast_rc, tc->expected_rc);
      tre_regfree(&preg);
      return 1;
    }

  if (!same_pmatch(fast, slow, PMATCH_SLOTS))
    {
      fprintf(stderr, "%s: fast and slow pmatch differ\n", tc->name);
      tre_regfree(&preg);
      return 1;
    }

  if ((tc->cflags & REG_NOSUB) && fast_rc == REG_OK
      && !pmatch_cleared(fast, PMATCH_SLOTS))
    {
      fprintf(stderr, "%s: REG_NOSUB match did not clear pmatch\n", tc->name);
      tre_regfree(&preg);
      return 1;
    }

  tre_regfree(&preg);
  return 0;
}

int
main(void)
{
  static const char nonascii_pattern[] = { (char)0xc0, '|', (char)0xe0 };
  static const char nonascii_haystack[] = { 'x', (char)0xe0, 'y' };
  static const litopt_case_t cases[] = {
    {
      "contains basic",
      "foo|bar|baz",
      sizeof("foo|bar|baz") - 1,
      REG_EXTENDED | REG_NOSUB,
      "xxbaryy",
      sizeof("xxbaryy") - 1,
      0,
      REG_OK,
      TRE_LITERAL_OPT_CONTAINS
    },
    {
      "contains ignores bol/eol flags",
      "foo|bar|baz",
      sizeof("foo|bar|baz") - 1,
      REG_EXTENDED | REG_NOSUB,
      "xxbaryy",
      sizeof("xxbaryy") - 1,
      REG_NOTBOL | REG_NOTEOL,
      REG_OK,
      TRE_LITERAL_OPT_CONTAINS
    },
    {
      "prefix basic",
      "^(foo|bar|baz)",
      sizeof("^(foo|bar|baz)") - 1,
      REG_EXTENDED | REG_NOSUB,
      "barrier",
      sizeof("barrier") - 1,
      0,
      REG_OK,
      TRE_LITERAL_OPT_PREFIX
    },
    {
      "prefix respects REG_NOTBOL",
      "^(foo|bar|baz)",
      sizeof("^(foo|bar|baz)") - 1,
      REG_EXTENDED | REG_NOSUB,
      "barrier",
      sizeof("barrier") - 1,
      REG_NOTBOL,
      REG_NOMATCH,
      TRE_LITERAL_OPT_PREFIX
    },
    {
      "suffix basic",
      "(foo|bar|baz)$",
      sizeof("(foo|bar|baz)$") - 1,
      REG_EXTENDED | REG_NOSUB,
      "crowbar",
      sizeof("crowbar") - 1,
      0,
      REG_OK,
      TRE_LITERAL_OPT_SUFFIX
    },
    {
      "suffix respects REG_NOTEOL",
      "(foo|bar|baz)$",
      sizeof("(foo|bar|baz)$") - 1,
      REG_EXTENDED | REG_NOSUB,
      "crowbar",
      sizeof("crowbar") - 1,
      REG_NOTEOL,
      REG_NOMATCH,
      TRE_LITERAL_OPT_SUFFIX
    },
    {
      "exact basic",
      "^(foo|bar|baz)$",
      sizeof("^(foo|bar|baz)$") - 1,
      REG_EXTENDED | REG_NOSUB,
      "bar",
      sizeof("bar") - 1,
      0,
      REG_OK,
      TRE_LITERAL_OPT_EXACT
    },
    {
      "exact respects REG_NOTBOL",
      "^(foo|bar|baz)$",
      sizeof("^(foo|bar|baz)$") - 1,
      REG_EXTENDED | REG_NOSUB,
      "bar",
      sizeof("bar") - 1,
      REG_NOTBOL,
      REG_NOMATCH,
      TRE_LITERAL_OPT_EXACT
    },
    {
      "exact respects REG_NOTEOL",
      "^(foo|bar|baz)$",
      sizeof("^(foo|bar|baz)$") - 1,
      REG_EXTENDED | REG_NOSUB,
      "bar",
      sizeof("bar") - 1,
      REG_NOTEOL,
      REG_NOMATCH,
      TRE_LITERAL_OPT_EXACT
    },
    {
      "empty alternation disables optimization",
      "(|foo|bar)",
      sizeof("(|foo|bar)") - 1,
      REG_EXTENDED | REG_NOSUB,
      "",
      0,
      0,
      REG_OK,
      TRE_LITERAL_OPT_NONE
    },
    {
      "inline flag disable stays generic",
      "foo(?-i:zap)zot",
      sizeof("foo(?-i:zap)zot") - 1,
      REG_EXTENDED | REG_ICASE | REG_NOSUB,
      "FoOzApZOt",
      sizeof("FoOzApZOt") - 1,
      0,
      REG_NOMATCH,
      TRE_LITERAL_OPT_NONE
    },
    {
      "inline flag disable still matches exact scoped bytes",
      "foo(?-i:zap)zot",
      sizeof("foo(?-i:zap)zot") - 1,
      REG_EXTENDED | REG_ICASE | REG_NOSUB,
      "FoOzapZOt",
      sizeof("FoOzapZOt") - 1,
      0,
      REG_OK,
      TRE_LITERAL_OPT_NONE
    },
    {
      "nocase non-ascii bytes stay in sync",
      nonascii_pattern,
      sizeof(nonascii_pattern),
      REG_EXTENDED | REG_ICASE | REG_NOSUB,
      nonascii_haystack,
      sizeof(nonascii_haystack),
      0,
      RC_ANY,
      TRE_LITERAL_OPT_CONTAINS
    }
  };
  size_t i;
  int failures = 0;

  setlocale(LC_CTYPE, "en_US.ISO-8859-1");

  for (i = 0; i < elementsof(cases); i++)
    failures += run_case(&cases[i]);

  return failures;
}
