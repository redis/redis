/*
  tre-match-parallel.c - TRE parallel regex matching engine

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

/*
  This algorithm searches for matches basically by reading characters
  in the searched string one by one, starting at the beginning.	 All
  matching paths in the TNFA are traversed in parallel.	 When two or
  more paths reach the same state, exactly one is chosen according to
  tag ordering rules; if returning submatches is not required it does
  not matter which path is chosen.

  The worst case time required for finding the leftmost and longest
  match, or determining that there is no match, is always linearly
  dependent on the length of the text being searched.

  This algorithm cannot handle TNFAs with back referencing nodes.
  See `tre-match-backtrack.c'.
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
#include <stdint.h>
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

#include "tre-internal.h"
#include "tre-match-utils.h"
#include "xmalloc.h"



typedef struct {
  tre_tnfa_transition_t *state;
  int *tags;
} tre_tnfa_reach_t;

typedef struct {
  int pos;
  int **tags;
} tre_reach_pos_t;


#ifdef TRE_DEBUG
static void
tre_print_reach(const tre_tnfa_reach_t *reach, int num_tags)
{
  int i;

  while (reach->state != NULL)
    {
      DPRINT((" %p", (void *)reach->state));
      if (num_tags > 0)
	{
	  DPRINT(("/"));
	  for (i = 0; i < num_tags; i++)
	    {
	      DPRINT(("%d:%d", i, reach->tags[i]));
	      if (i < (num_tags-1))
		DPRINT((","));
	    }
	}
      reach++;
    }
  DPRINT(("\n"));

}
#endif /* TRE_DEBUG */

reg_errcode_t
tre_tnfa_run_parallel(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
		      tre_str_type_t type, int *match_tags, int eflags,
		      int *match_end_ofs)
{
  /* State variables required by GET_NEXT_WCHAR. */
  tre_char_t prev_c = 0, next_c = 0;
  const char *str_byte = string;
  ssize_t pos = -1;
  unsigned int pos_add_next = 1;
#ifdef TRE_WCHAR
  const wchar_t *str_wide = string;
#ifdef TRE_MBSTATE
  mbstate_t mbstate;
#endif /* TRE_MBSTATE */
#endif /* TRE_WCHAR */
  reg_errcode_t ret;
  int reg_notbol = eflags & REG_NOTBOL;
  int reg_noteol = eflags & REG_NOTEOL;
  int reg_newline = tnfa->cflags & REG_NEWLINE;
  int str_user_end = 0;

  char *buf;
  tre_tnfa_transition_t *trans_i;
  tre_tnfa_reach_t *reach, *reach_next, *reach_i, *reach_next_i;
  tre_reach_pos_t *reach_pos;
  int *tag_i;
  int num_tags, i;

  int match_eo = -1;	   /* end offset of match (-1 if no match found yet) */
  int new_match = 0;
  int *tmp_tags = NULL;
  int *tmp_iptr;

  /*
   * TRE internals tend to use int instead of size_t for positions or
   * lengths and don't check for overflow.  This will take time to fix
   * properly.  In the meantime, simply limit the input to what we can
   * handle.
   */
  if (len > TRE_MAX_STRING)
    len = TRE_MAX_STRING;

#ifdef TRE_MBSTATE
  memset(&mbstate, '\0', sizeof(mbstate));
#endif /* TRE_MBSTATE */

  DPRINT(("tre_tnfa_run_parallel, input type %d\n", type));

  if (!match_tags)
    num_tags = 0;
  else
    num_tags = tnfa->num_tags;

  /* Allocate memory for temporary data required for matching.	This needs to
     be done for every matching operation to be thread safe.  This allocates
     everything in a single large block from the stack frame using alloca()
     or with malloc() if alloca is unavailable. */
  {
    size_t tbytes, rbytes, pbytes, xbytes, total_bytes;
    size_t num_states = (size_t)tnfa->num_states;
    size_t state_tag_bytes, reach_bytes;
    size_t padding = (sizeof(long) - 1) * 4;
    char *tmp_buf;

    if (num_states > SIZE_MAX / sizeof(*reach_pos))
      return REG_ESPACE;
    pbytes = sizeof(*reach_pos) * num_states;

    if (num_states + 1 > SIZE_MAX / sizeof(*reach_next))
      return REG_ESPACE;
    rbytes = sizeof(*reach_next) * (num_states + 1);

    if ((size_t)num_tags > SIZE_MAX / sizeof(*tmp_tags))
      return REG_ESPACE;
    tbytes = sizeof(*tmp_tags) * (size_t)num_tags;

    if ((size_t)num_tags > SIZE_MAX / sizeof(int))
      return REG_ESPACE;
    xbytes = sizeof(int) * (size_t)num_tags;

    if (num_states > 0 && xbytes > SIZE_MAX / num_states)
      return REG_ESPACE;
    state_tag_bytes = xbytes * num_states;

    if (rbytes > SIZE_MAX - state_tag_bytes)
      return REG_ESPACE;
    reach_bytes = rbytes + state_tag_bytes;

    if (reach_bytes > (SIZE_MAX - padding - tbytes - pbytes) / 2)
      return REG_ESPACE;

    /* Compute the length of the block we need. */
    total_bytes =
      padding + reach_bytes * 2 + tbytes + pbytes;

    /* Allocate the memory. */
#ifdef TRE_USE_ALLOCA
    buf = alloca(total_bytes);
#else /* !TRE_USE_ALLOCA */
    buf = xmalloc(total_bytes);
#endif /* !TRE_USE_ALLOCA */
    if (buf == NULL)
      return REG_ESPACE;
    memset(buf, 0, total_bytes);

    /* Get the various pointers within tmp_buf (properly aligned). */
    tmp_tags = (void *)buf;
    tmp_buf = buf + tbytes;
    tmp_buf += ALIGN(tmp_buf, long);
    reach_next = (void *)tmp_buf;
    tmp_buf += rbytes;
    tmp_buf += ALIGN(tmp_buf, long);
    reach = (void *)tmp_buf;
    tmp_buf += rbytes;
    tmp_buf += ALIGN(tmp_buf, long);
    reach_pos = (void *)tmp_buf;
    tmp_buf += pbytes;
    tmp_buf += ALIGN(tmp_buf, long);
    for (i = 0; i < tnfa->num_states; i++)
      {
	reach[i].tags = (void *)tmp_buf;
	tmp_buf += xbytes;
	reach_next[i].tags = (void *)tmp_buf;
	tmp_buf += xbytes;
      }
  }

  for (i = 0; i < tnfa->num_states; i++)
    reach_pos[i].pos = -1;

  /* If only one character can start a match, find it first. */
  if (tnfa->first_char >= 0 && type == STR_BYTE && str_byte)
    {
      const char *orig_str = str_byte;
      int first = tnfa->first_char;

      if (len >= 0)
	str_byte = memchr(orig_str, first, (size_t)len);
      else
	str_byte = strchr(orig_str, first);
      if (str_byte == NULL)
	{
#ifndef TRE_USE_ALLOCA
	  if (buf)
	    xfree(buf);
#endif /* !TRE_USE_ALLOCA */
	  return REG_NOMATCH;
	}
      DPRINT(("skipped %lu chars\n", (unsigned long)(str_byte - orig_str)));
      if (str_byte >= orig_str + 1)
	prev_c = (unsigned char)*(str_byte - 1);
      next_c = (unsigned char)*str_byte;
      pos = str_byte - orig_str;
      if (len < 0 || pos < len)
	str_byte++;
    }
  else
    {
      GET_NEXT_WCHAR();
      pos = 0;
    }

#if 0
  /* Skip over characters that cannot possibly be the first character
     of a match. */
  if (tnfa->firstpos_chars != NULL)
    {
      char *chars = tnfa->firstpos_chars;

      if (len < 0)
	{
	  const char *orig_str = str_byte;
	  /* XXX - use strpbrk() and wcspbrk() because they might be
	     optimized for the target architecture.  Try also strcspn()
	     and wcscspn() and compare the speeds. */
	  while (next_c != L'\0' && !chars[next_c])
	    {
	      next_c = *str_byte++;
	    }
	  prev_c = *(str_byte - 2);
	  pos += str_byte - orig_str;
	  DPRINT(("skipped %d chars\n", str_byte - orig_str));
	}
      else
	{
	  while (pos <= len && !chars[next_c])
	    {
	      prev_c = next_c;
	      next_c = (unsigned char)(*str_byte++);
	      pos++;
	    }
	}
    }
#endif

  DPRINT(("length: %zd\n", len));
  DPRINT(("pos:chr/code | states and tags\n"));
  DPRINT(("-------------+------------------------------------------------\n"));

  reach_next_i = reach_next;
  while (/*CONSTCOND*/(void)1,1)
    {
      /* If no match found yet, add the initial states to `reach_next'. */
      if (match_eo < 0)
	{
	  DPRINT((" init >"));
	  trans_i = tnfa->initial;
	  while (trans_i->state != NULL)
	    {
	      if (reach_pos[trans_i->state_id].pos < pos)
		{
		  if (trans_i->assertions
		      && CHECK_ASSERTIONS(trans_i->assertions))
		    {
		      DPRINT(("assertion failed\n"));
		      trans_i++;
		      continue;
		    }

		  DPRINT((" %p", (void *)trans_i->state));
		  reach_next_i->state = trans_i->state;
		  for (i = 0; i < num_tags; i++)
		    reach_next_i->tags[i] = -1;
		  tag_i = trans_i->tags;
		  if (tag_i)
		    while (*tag_i >= 0)
		      {
			if (*tag_i < num_tags)
			  reach_next_i->tags[*tag_i] = pos;
			tag_i++;
		      }
		  if (reach_next_i->state == tnfa->final)
		    {
		      DPRINT(("	 found empty match\n"));
		      match_eo = pos;
		      new_match = 1;
		      for (i = 0; i < num_tags; i++)
			match_tags[i] = reach_next_i->tags[i];
		    }
		  reach_pos[trans_i->state_id].pos = pos;
		  reach_pos[trans_i->state_id].tags = &reach_next_i->tags;
		  reach_next_i++;
		}
	      trans_i++;
	    }
	  DPRINT(("\n"));
	  reach_next_i->state = NULL;
	}
      else
	{
	  if (num_tags == 0 || reach_next_i == reach_next)
	    /* We have found a match. */
	    break;
	}

      /* Check for end of string. */
      if (len < 0)
	{
	  if (type == STR_USER)
	    {
	      if (str_user_end)
		break;
	    }
	  else if (next_c == L'\0' || pos >= TRE_MAX_STRING)
	    break;
	}
      else
	{
	  if (pos >= len)
	    break;
	}

      GET_NEXT_WCHAR();

#ifdef TRE_DEBUG
      DPRINT(("%3zd:%2lc/%05d |", pos - 1, (tre_cint_t)prev_c, (int)prev_c));
      tre_print_reach(reach_next, num_tags);
      DPRINT(("%3zd:%2lc/%05d |", pos, (tre_cint_t)next_c, (int)next_c));
      tre_print_reach(reach_next, num_tags);
#endif /* TRE_DEBUG */

      /* Swap `reach' and `reach_next'. */
      reach_i = reach;
      reach = reach_next;
      reach_next = reach_i;

      /* For each state in `reach', weed out states that don't fulfill the
	 minimal matching conditions. */
      if (tnfa->num_minimals && new_match)
	{
	  new_match = 0;
	  reach_next_i = reach_next;
	  for (reach_i = reach; reach_i->state; reach_i++)
	    {
	      int skip = 0;
	      for (i = 0; tnfa->minimal_tags[i] >= 0; i += 2)
		{
		  int end = tnfa->minimal_tags[i];
		  int start = tnfa->minimal_tags[i + 1];
		  DPRINT(("  Minimal start %d, end %d\n", start, end));
		  if (end >= num_tags)
		    {
		      DPRINT(("	 Throwing %p out.\n", reach_i->state));
		      skip = 1;
		      break;
		    }
		  else if (reach_i->tags[start] == match_tags[start]
			   && reach_i->tags[end] < match_tags[end])
		    {
		      DPRINT(("	 Throwing %p out because t%d < %d\n",
			      reach_i->state, end, match_tags[end]));
		      skip = 1;
		      break;
		    }
		}
	      if (!skip)
		{
		  reach_next_i->state = reach_i->state;
		  tmp_iptr = reach_next_i->tags;
		  reach_next_i->tags = reach_i->tags;
		  reach_i->tags = tmp_iptr;
		  reach_next_i++;
		}
	    }
	  reach_next_i->state = NULL;

	  /* Swap `reach' and `reach_next'. */
	  reach_i = reach;
	  reach = reach_next;
	  reach_next = reach_i;
	}

      /* For each state in `reach' see if there is a transition leaving with
	 the current input symbol to a state not yet in `reach_next', and
	 add the destination states to `reach_next'. */
      reach_next_i = reach_next;
      for (reach_i = reach; reach_i->state; reach_i++)
	{
	  for (trans_i = reach_i->state; trans_i->state; trans_i++)
	    {
	      /* Does this transition match the input symbol? */
	      if (trans_i->code_min <= (tre_cint_t)prev_c &&
		  trans_i->code_max >= (tre_cint_t)prev_c)
		{
		  if (trans_i->assertions
		      && (CHECK_ASSERTIONS(trans_i->assertions)
			  || CHECK_CHAR_CLASSES(trans_i, tnfa, eflags)))
		    {
		      DPRINT(("assertion failed\n"));
		      continue;
		    }

		  /* Compute the tags after this transition. */
		  for (i = 0; i < num_tags; i++)
		    tmp_tags[i] = reach_i->tags[i];
		  tag_i = trans_i->tags;
		  if (tag_i != NULL)
		    while (*tag_i >= 0)
		      {
			if (*tag_i < num_tags)
			  tmp_tags[*tag_i] = pos;
			tag_i++;
		      }

		  if (reach_pos[trans_i->state_id].pos < pos)
		    {
		      /* Found an unvisited node. */
		      reach_next_i->state = trans_i->state;
		      tmp_iptr = reach_next_i->tags;
		      reach_next_i->tags = tmp_tags;
		      tmp_tags = tmp_iptr;
		      reach_pos[trans_i->state_id].pos = pos;
		      reach_pos[trans_i->state_id].tags = &reach_next_i->tags;

		      if (reach_next_i->state == tnfa->final
			  && (match_eo == -1
			      || (num_tags > 0
				  && reach_next_i->tags[0] <= match_tags[0])))
			{
			  DPRINT(("  found match %p\n", trans_i->state));
			  match_eo = pos;
			  new_match = 1;
			  for (i = 0; i < num_tags; i++)
			    match_tags[i] = reach_next_i->tags[i];
			}
		      reach_next_i++;

		    }
		  else
		    {
		      assert(reach_pos[trans_i->state_id].pos == pos);
		      /* Another path has also reached this state.  We choose
			 the winner by examining the tag values for both
			 paths. */
		      if (tre_tag_order(num_tags, tnfa->tag_directions,
					tmp_tags,
					*reach_pos[trans_i->state_id].tags))
			{
			  /* The new path wins. */
			  tmp_iptr = *reach_pos[trans_i->state_id].tags;
			  *reach_pos[trans_i->state_id].tags = tmp_tags;
			  if (trans_i->state == tnfa->final)
			    {
			      DPRINT(("	 found better match\n"));
			      match_eo = pos;
			      new_match = 1;
			      for (i = 0; i < num_tags; i++)
				match_tags[i] = tmp_tags[i];
			    }
			  tmp_tags = tmp_iptr;
			}
		    }
		}
	    }
	}
      reach_next_i->state = NULL;
    }

  DPRINT(("match end offset = %d\n", match_eo));

  *match_end_ofs = match_eo;
  ret = match_eo >= 0 ? REG_OK : REG_NOMATCH;

#ifndef TRE_USE_ALLOCA
  if (buf)
    xfree(buf);
#endif /* !TRE_USE_ALLOCA */
  return ret;
}

/* EOF */
