/*
  tre_regexec.c - TRE POSIX compatible matching functions (and more).

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#ifdef TRE_USE_ALLOCA
/* AIX requires this to be the first thing in the file.	 */
#ifndef __GNUC__
# if HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  else
#   ifndef alloca /* predefined by HP cc +Olibcalls */
char *alloca ();
#   endif
#  endif
# endif
#endif
#endif /* TRE_USE_ALLOCA */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif /* HAVE_WCHAR_H */
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif /* HAVE_WCTYPE_H */
#ifndef TRE_WCHAR
#include <ctype.h>
#endif /* !TRE_WCHAR */
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif /* HAVE_MALLOC_H */
#include <limits.h>

#include "tre-internal.h"
#include "xmalloc.h"

/* Literal alternatives are grouped by the first byte so the matcher can
 * reach the relevant candidates in O(1). In nocase mode the lookup uses the
 * same folded byte mapping that was applied at compile time. */
static void
tre_litopt_candidate_range(const tre_literal_opt_t *opt, unsigned char first_byte,
			   size_t *start, size_t *end)
{
  unsigned char key = opt->nocase ? opt->fold_map[first_byte] : first_byte;
  *start = opt->start_offsets[key];
  *end = opt->start_offsets[key + 1];
}

static int
tre_litopt_bytes_equal(const unsigned char *haystack,
		       const unsigned char *needle, size_t len,
		       const unsigned char *fold_map)
{
  size_t i;

  if (fold_map == NULL)
    return memcmp(haystack, needle, len) == 0;

  for (i = 0; i < len; i++)
    if (fold_map[haystack[i]] != needle[i])
      return 0;
  return 1;
}

static int
tre_litopt_contains_case(const unsigned char *haystack, size_t hay_len,
			 const unsigned char *needle, size_t needle_len,
			 int *match_end_ofs)
{
  const unsigned char *p;
  size_t remaining;

  if (needle_len > hay_len)
    return 0;

  p = haystack;
  remaining = hay_len;
  while (remaining >= needle_len)
    {
      p = memchr(p, needle[0], remaining - needle_len + 1);
      if (p == NULL)
	return 0;
      if (memcmp(p, needle, needle_len) == 0)
	{
	  if (match_end_ofs != NULL)
	    *match_end_ofs = (int)(p - haystack + needle_len);
	  return 1;
	}
      remaining = hay_len - (size_t)(p - haystack) - 1;
      p++;
    }
  return 0;
}

/* Nocase substring matching is still byte-oriented, but scanning once and
 * only checking literals that share the same folded first byte avoids the
 * old O(haystack * literals) restart pattern. */
static int
tre_litopt_contains_nocase(const tre_literal_opt_t *opt,
			   const unsigned char *haystack, size_t hay_len,
			   int *match_end_ofs)
{
  size_t i, start, end, j;

  for (i = 0; i < hay_len; i++)
    {
      tre_litopt_candidate_range(opt, haystack[i], &start, &end);
      for (j = start; j < end; j++)
	{
	  const tre_literal_opt_literal_t *lit = &opt->literals[j];
	  if (lit->len <= hay_len - i
	      && tre_litopt_bytes_equal(haystack + i, lit->data, lit->len,
					opt->fold_map))
	    {
	      if (match_end_ofs != NULL)
		*match_end_ofs = (int)(i + lit->len);
	      return 1;
	    }
	}
    }
  return 0;
}

static reg_errcode_t
tre_match_literal_opt(const tre_tnfa_t *tnfa, const char *string, size_t len,
		      int eflags, int *match_end_ofs)
{
  const tre_literal_opt_t *opt = &tnfa->literal_opt;
  const unsigned char *haystack = (const unsigned char *)string;
  size_t start = 0, end = opt->num_literals, i;
  const unsigned char *fold_map = opt->nocase ? opt->fold_map : NULL;

  if ((opt->mode == TRE_LITERAL_OPT_PREFIX
       || opt->mode == TRE_LITERAL_OPT_EXACT)
      && (eflags & REG_NOTBOL))
    return REG_NOMATCH;
  if ((opt->mode == TRE_LITERAL_OPT_SUFFIX
       || opt->mode == TRE_LITERAL_OPT_EXACT)
      && (eflags & REG_NOTEOL))
    return REG_NOMATCH;

  if ((opt->mode == TRE_LITERAL_OPT_EXACT
       || opt->mode == TRE_LITERAL_OPT_PREFIX)
      && len > 0)
    tre_litopt_candidate_range(opt, haystack[0], &start, &end);

  if (opt->mode == TRE_LITERAL_OPT_CONTAINS)
    {
      if (opt->nocase)
	return tre_litopt_contains_nocase(opt, haystack, len, match_end_ofs)
	       ? REG_OK : REG_NOMATCH;

      for (i = 0; i < opt->num_literals; i++)
	{
	  const tre_literal_opt_literal_t *lit = &opt->literals[i];
	  if (tre_litopt_contains_case(haystack, len, lit->data, lit->len,
				       match_end_ofs))
	    return REG_OK;
	}
      return REG_NOMATCH;
    }

  for (i = start; i < end; i++)
    {
      const tre_literal_opt_literal_t *lit = &opt->literals[i];

      switch (opt->mode)
	{
	case TRE_LITERAL_OPT_EXACT:
	  if (len == lit->len
	      && tre_litopt_bytes_equal(haystack, lit->data, len, fold_map))
	    {
	      if (match_end_ofs != NULL)
		*match_end_ofs = (int)len;
	      return REG_OK;
	    }
	  break;

	case TRE_LITERAL_OPT_PREFIX:
	  if (len >= lit->len
	      && tre_litopt_bytes_equal(haystack, lit->data, lit->len,
					fold_map))
	    {
	      if (match_end_ofs != NULL)
		*match_end_ofs = (int)lit->len;
	      return REG_OK;
	    }
	  break;

	case TRE_LITERAL_OPT_SUFFIX:
	  if (len >= lit->len
	      && tre_litopt_bytes_equal(haystack + len - lit->len, lit->data,
					lit->len, fold_map))
	    {
	      if (match_end_ofs != NULL)
		*match_end_ofs = (int)len;
	      return REG_OK;
	    }
	  break;

	case TRE_LITERAL_OPT_CONTAINS:
	case TRE_LITERAL_OPT_NONE:
	  break;
	}
    }

  return REG_NOMATCH;
}


/* Fills the POSIX.2 regmatch_t array according to the TNFA tag and match
   endpoint values. */
void
tre_fill_pmatch(size_t nmatch, regmatch_t pmatch[], int cflags,
		const tre_tnfa_t *tnfa, int *tags, int match_eo)
{
  tre_submatch_data_t *submatch_data;
  unsigned int i, j;
  int *parents;

  i = 0;
  if (match_eo >= 0 && !(cflags & REG_NOSUB))
    {
      /* Construct submatch offsets from the tags. */
      DPRINT(("end tag = t%d = %d\n", tnfa->end_tag, match_eo));
      submatch_data = tnfa->submatch_data;
      while (i < tnfa->num_submatches && i < nmatch)
	{
	  if (submatch_data[i].so_tag == tnfa->end_tag)
	    pmatch[i].rm_so = match_eo;
	  else
	    pmatch[i].rm_so = tags[submatch_data[i].so_tag];

	  if (submatch_data[i].eo_tag == tnfa->end_tag)
	    pmatch[i].rm_eo = match_eo;
	  else
	    pmatch[i].rm_eo = tags[submatch_data[i].eo_tag];

	  /* If either of the endpoints were not used, this submatch
	     was not part of the match. */
	  if (pmatch[i].rm_so == -1 || pmatch[i].rm_eo == -1)
	    pmatch[i].rm_so = pmatch[i].rm_eo = -1;

	  DPRINT(("pmatch[%d] = {t%d = %d, t%d = %d}\n", i,
		  submatch_data[i].so_tag, pmatch[i].rm_so,
		  submatch_data[i].eo_tag, pmatch[i].rm_eo));
	  i++;
	}
      /* Reset all submatches that are not within all of their parent
	 submatches. */
      i = 0;
      while (i < tnfa->num_submatches && i < nmatch)
	{
	  if (pmatch[i].rm_eo == -1)
	    assert(pmatch[i].rm_so == -1);
	  assert(pmatch[i].rm_so <= pmatch[i].rm_eo);

	  parents = submatch_data[i].parents;
	  if (parents != NULL)
	    for (j = 0; parents[j] >= 0; j++)
	      {
		DPRINT(("pmatch[%d] parent %d\n", i, parents[j]));
		if (pmatch[i].rm_so < pmatch[parents[j]].rm_so
		    || pmatch[i].rm_eo > pmatch[parents[j]].rm_eo)
		  pmatch[i].rm_so = pmatch[i].rm_eo = -1;
	      }
	  i++;
	}
    }

  while (i < nmatch)
    {
      pmatch[i].rm_so = -1;
      pmatch[i].rm_eo = -1;
      i++;
    }
}


/*
  Wrapper functions for POSIX compatible regexp matching.
*/

int
tre_have_backrefs(const regex_t *preg)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tnfa->have_backrefs;
}

int
tre_have_approx(const regex_t *preg)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tnfa->have_approx;
}

static int
tre_match(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
	  tre_str_type_t type, size_t nmatch, regmatch_t pmatch[],
	  int eflags)
{
  reg_errcode_t status;
  int *tags = NULL, eo;
  if (tnfa->num_tags > 0 && nmatch > 0)
    {
#ifdef TRE_USE_ALLOCA
      tags = alloca(sizeof(*tags) * tnfa->num_tags);
#else /* !TRE_USE_ALLOCA */
      tags = xmalloc(sizeof(*tags) * tnfa->num_tags);
#endif /* !TRE_USE_ALLOCA */
      if (tags == NULL)
	return REG_ESPACE;
    }

  if (type == STR_BYTE
      && tnfa->literal_opt.mode != TRE_LITERAL_OPT_NONE
      && (nmatch == 0 || (tnfa->cflags & REG_NOSUB))
#ifdef TRE_APPROX
      && !(eflags & REG_APPROX_MATCHER)
#endif /* TRE_APPROX */
      && !(eflags & REG_BACKTRACKING_MATCHER))
    {
      size_t byte_len = (len >= 0) ? (size_t)len : strlen((const char *)string);
      status = tre_match_literal_opt(tnfa, string, byte_len, eflags, &eo);

      /* Even when the caller asked for no submatches, regexec() still has to
       * clear any pmatch entries it was handed. The normal matcher path does
       * this through tre_fill_pmatch(), so mirror that behavior here. */
      if (status == REG_OK && nmatch > 0)
	tre_fill_pmatch(nmatch, pmatch, tnfa->cflags, tnfa, NULL, eo);

#ifndef TRE_USE_ALLOCA
      if (tags)
	xfree(tags);
#endif /* !TRE_USE_ALLOCA */
      return status;
    }

  /* Dispatch to the appropriate matcher. */
  if (tnfa->have_backrefs || eflags & REG_BACKTRACKING_MATCHER)
    {
      /* The regex has back references, use the backtracking matcher. */
      if (type == STR_USER)
	{
	  const tre_str_source *source = string;
	  if (source->rewind == NULL || source->compare == NULL)
	    {
	      /* The backtracking matcher requires rewind and compare
		 capabilities from the input stream. */
#ifndef TRE_USE_ALLOCA
	      if (tags)
		xfree(tags);
#endif /* !TRE_USE_ALLOCA */
	      return REG_BADPAT;
	    }
	}
      status = tre_tnfa_run_backtrack(tnfa, string, len, type,
				      tags, eflags, &eo);
    }
#ifdef TRE_APPROX
  else if (tnfa->have_approx || eflags & REG_APPROX_MATCHER)
    {
      /* The regex uses approximate matching, use the approximate matcher. */
      regamatch_t match;
      regaparams_t params;
      tre_regaparams_default(&params);
      params.max_err = 0;
      params.max_cost = 0;
      status = tre_tnfa_run_approx(tnfa, string, len, type, tags,
				   &match, params, eflags, &eo);
    }
#endif /* TRE_APPROX */
  else
    {
      /* Exact matching, no back references, use the parallel matcher. */
      status = tre_tnfa_run_parallel(tnfa, string, len, type,
				     tags, eflags, &eo);
    }

  if (status == REG_OK)
    /* A match was found, so fill the submatch registers. */
    tre_fill_pmatch(nmatch, pmatch, tnfa->cflags, tnfa, tags, eo);
#ifndef TRE_USE_ALLOCA
  if (tags)
    xfree(tags);
#endif /* !TRE_USE_ALLOCA */
  return status;
}

int
tre_regnexec(const regex_t *preg, const char *str, size_t len,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  tre_str_type_t type = (TRE_MB_CUR_MAX == 1) ? STR_BYTE : STR_MBS;

  return tre_match(tnfa, str, len, type, nmatch, pmatch, eflags);
}

#ifdef TRE_USE_GNUC_REGEXEC_FPL
int
tre_regexec(const regex_t *preg, const char *str,
	size_t nmatch, regmatch_t pmatch[_Restrict_arr_ _REGEX_NELTS (nmatch)],
	int eflags)
#else
int
tre_regexec(const regex_t *preg, const char *str,
	size_t nmatch, regmatch_t pmatch[], int eflags)
#endif
{
  return tre_regnexec(preg, str, -1, nmatch, pmatch, eflags);
}

int
tre_regexecb(const regex_t *preg, const char *str,
        size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;

  return tre_match(tnfa, str, -1, STR_BYTE, nmatch, pmatch, eflags);
}

int
tre_regnexecb(const regex_t *preg, const char *str, size_t len,
        size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;

  return tre_match(tnfa, str, len, STR_BYTE, nmatch, pmatch, eflags);
}


#ifdef TRE_WCHAR

int
tre_regwnexec(const regex_t *preg, const wchar_t *str, size_t len,
	  size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tre_match(tnfa, str, len, STR_WIDE, nmatch, pmatch, eflags);
}

int
tre_regwexec(const regex_t *preg, const wchar_t *str,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  return tre_regwnexec(preg, str, -1, nmatch, pmatch, eflags);
}

#endif /* TRE_WCHAR */

int
tre_reguexec(const regex_t *preg, const tre_str_source *str,
	 size_t nmatch, regmatch_t pmatch[], int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tre_match(tnfa, str, -1, STR_USER, nmatch, pmatch, eflags);
}


#ifdef TRE_APPROX

/*
  Wrapper functions for approximate regexp matching.
*/

static int
tre_match_approx(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
		 tre_str_type_t type, regamatch_t *match, regaparams_t params,
		 int eflags)
{
  reg_errcode_t status;
  int *tags = NULL, eo;

  /* If the regexp does not use approximate matching features, the
     maximum cost is zero, and the approximate matcher isn't forced,
     use the exact matcher instead. */
  if (params.max_cost == 0 && !tnfa->have_approx
      && !(eflags & REG_APPROX_MATCHER))
    return tre_match(tnfa, string, len, type, match->nmatch, match->pmatch,
		     eflags);

  /* Back references are not supported by the approximate matcher. */
  if (tnfa->have_backrefs)
    return REG_BADPAT;

  if (tnfa->num_tags > 0 && match->nmatch > 0)
    {
#if TRE_USE_ALLOCA
      tags = alloca(sizeof(*tags) * tnfa->num_tags);
#else /* !TRE_USE_ALLOCA */
      tags = xmalloc(sizeof(*tags) * tnfa->num_tags);
#endif /* !TRE_USE_ALLOCA */
      if (tags == NULL)
	return REG_ESPACE;
    }
  status = tre_tnfa_run_approx(tnfa, string, len, type, tags,
			       match, params, eflags, &eo);
  if (status == REG_OK)
    tre_fill_pmatch(match->nmatch, match->pmatch, tnfa->cflags, tnfa, tags, eo);
#ifndef TRE_USE_ALLOCA
  if (tags)
    xfree(tags);
#endif /* !TRE_USE_ALLOCA */
  return status;
}

int
tre_reganexec(const regex_t *preg, const char *str, size_t len,
	  regamatch_t *match, regaparams_t params, int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  tre_str_type_t type = (TRE_MB_CUR_MAX == 1) ? STR_BYTE : STR_MBS;

  return tre_match_approx(tnfa, str, len, type, match, params, eflags);
}

int
tre_regaexec(const regex_t *preg, const char *str,
	 regamatch_t *match, regaparams_t params, int eflags)
{
  return tre_reganexec(preg, str, -1, match, params, eflags);
}

int
tre_regaexecb(const regex_t *preg, const char *str,
          regamatch_t *match, regaparams_t params, int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;

  return tre_match_approx(tnfa, str, -1, STR_BYTE, match, params, eflags);
}

#ifdef TRE_WCHAR

int
tre_regawnexec(const regex_t *preg, const wchar_t *str, size_t len,
	   regamatch_t *match, regaparams_t params, int eflags)
{
  tre_tnfa_t *tnfa = (void *)preg->TRE_REGEX_T_FIELD;
  return tre_match_approx(tnfa, str, len, STR_WIDE,
			  match, params, eflags);
}

int
tre_regawexec(const regex_t *preg, const wchar_t *str,
	  regamatch_t *match, regaparams_t params, int eflags)
{
  return tre_regawnexec(preg, str, -1, match, params, eflags);
}

#endif /* TRE_WCHAR */

void
tre_regaparams_default(regaparams_t *params)
{
  memset(params, 0, sizeof(*params));
  params->cost_ins = 1;
  params->cost_del = 1;
  params->cost_subst = 1;
  params->max_cost = INT_MAX;
  params->max_ins = INT_MAX;
  params->max_del = INT_MAX;
  params->max_subst = INT_MAX;
  params->max_err = INT_MAX;
}

#endif /* TRE_APPROX */

/* EOF */
