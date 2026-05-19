/*
  test-malformed-regn.c - Verify exact-length edge-case regexps compile or fail
  cleanly both with and without a trailing NUL byte.

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tre.h"

typedef struct {
  const char *name;
  const char *pattern;
  int expected_err;
} malformed_case_t;

static int
run_case(const malformed_case_t *tc, int nul_terminated)
{
  regex_t preg;
  size_t len = strlen(tc->pattern);
  size_t alloc_len = len + (nul_terminated ? 1 : 0);
  char *pattern = malloc(alloc_len ? alloc_len : 1);
  int errcode;

  if (pattern == NULL)
    {
      fprintf(stderr, "%s: out of memory\n", tc->name);
      return 1;
    }

  if (len > 0)
    memcpy(pattern, tc->pattern, len);
  if (nul_terminated)
    pattern[len] = '\0';

  memset(&preg, 0, sizeof(preg));
  errcode = tre_regncompb(&preg, pattern, len, REG_EXTENDED | REG_NOSUB);
  if (errcode == REG_OK)
    tre_regfree(&preg);

  free(pattern);

  if (errcode != tc->expected_err)
    {
      char errbuf[128];
      memset(&preg, 0, sizeof(preg));
      tre_regerror(errcode, &preg, errbuf, sizeof(errbuf));
      fprintf(stderr, "%s (%s): got %d (%s), expected %d\n",
	      tc->name, nul_terminated ? "nul" : "exact",
	      errcode, errbuf, tc->expected_err);
      return 1;
    }

  return 0;
}

int
main(void)
{
  static const malformed_case_t cases[] = {
    { "open paren", "(", REG_EPAREN },
    { "open bracket", "[", REG_EBRACK },
    { "unterminated comment", "(?#", REG_BADPAT },
    { "unterminated inline flags", "(?i", REG_BADPAT },
    { "short hex escape", "\\x", REG_OK },
    { "unterminated wide hex", "\\x{", REG_EBRACE },
    { "empty wide hex", "\\x{}", REG_OK }
  };
  size_t i;

  for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    {
      if (run_case(&cases[i], 0))
	return 1;
      if (run_case(&cases[i], 1))
	return 1;
    }

  return 0;
}
