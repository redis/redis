/*
  tre_regcomp.c - TRE POSIX compatible regex compilation functions.

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "tre-internal.h"
#include "xmalloc.h"

int
tre_regncomp(regex_t *preg, const char *regex, size_t n, int cflags)
{
  int ret;
  if (n > TRE_MAX_RE)
    return REG_ESPACE;
#if TRE_WCHAR
  tre_char_t *wregex;
  size_t wlen;

  wregex = xmalloc(sizeof(tre_char_t) * (n + 1));
  if (wregex == NULL)
    return REG_ESPACE;

  /* If the current locale uses the standard single byte encoding of
     characters, we don't do a multibyte string conversion.  If we did,
     many applications which use the default locale would break since
     the default "C" locale uses the 7-bit ASCII character set, and
     all characters with the eighth bit set would be considered invalid. */
#if TRE_MULTIBYTE
  if (TRE_MB_CUR_MAX == 1)
#endif /* TRE_MULTIBYTE */
    {
      size_t i;
      const unsigned char *str = (const unsigned char *)regex;
      tre_char_t *wstr = wregex;

      for (i = 0; i < n; i++)
	*(wstr++) = *(str++);
      wlen = n;
    }
#if TRE_MULTIBYTE
  else
    {
      size_t consumed;
      tre_char_t *wcptr = wregex;
#ifdef HAVE_MBSTATE_T
      mbstate_t state;
      memset(&state, '\0', sizeof(state));
#endif /* HAVE_MBSTATE_T */
      while (n > 0)
	{
	  consumed = tre_mbrtowc(wcptr, regex, n, &state);

	  switch (consumed)
	    {
	    case 0:
	      if (*regex == '\0')
		consumed = 1;
	      else
		{
		  xfree(wregex);
		  return REG_BADPAT;
		}
	      break;
	    case -1:
	      DPRINT(("mbrtowc: error %d: %s.\n", errno, strerror(errno)));
	      xfree(wregex);
	      return REG_BADPAT;
	    case -2:
	      /* The last character wasn't complete.  Let's not call it a
		 fatal error. */
	      consumed = n;
	      break;
	    }
	  regex += consumed;
	  n -= consumed;
	  wcptr++;
	}
      wlen = wcptr - wregex;
    }
#endif /* TRE_MULTIBYTE */

  wregex[wlen] = L'\0';
  ret = tre_compile(preg, wregex, wlen, cflags);
  xfree(wregex);
#else /* !TRE_WCHAR */
  ret = tre_compile(preg, (const tre_char_t *)regex, n, cflags);
#endif /* !TRE_WCHAR */

  return ret;
}

/* this version takes bytes literally, to be used with raw vectors */
int
tre_regncompb(regex_t *preg, const char *regex, size_t n, int cflags)
{
  int ret;
  if (n > TRE_MAX_RE)
    return REG_ESPACE;
#if TRE_WCHAR /* wide chars = we need to convert it all to the wide format */
  tre_char_t *wregex;
  size_t i;

  wregex = xmalloc(sizeof(tre_char_t) * n);
  if (wregex == NULL)
    return REG_ESPACE;

  for (i = 0; i < n; i++)
    wregex[i] = (tre_char_t) ((unsigned char) regex[i]);

  ret = tre_compile(preg, wregex, n, cflags | REG_USEBYTES);
  xfree(wregex);
#else /* !TRE_WCHAR */
  ret = tre_compile(preg, (const tre_char_t *)regex, n, cflags | REG_USEBYTES);
#endif /* !TRE_WCHAR */

  return ret;
}

int
tre_regcomp(regex_t *preg, const char *regex, int cflags)
{
  size_t n = regex ? strlen(regex) : 0;
  if (n > TRE_MAX_RE)
    return REG_ESPACE;
  return tre_regncomp(preg, regex, n, cflags);
}

int
tre_regcompb(regex_t *preg, const char *regex, int cflags)
{
  int ret;
  tre_char_t *wregex;
  size_t i, n = regex ? strlen(regex) : 0;
  const unsigned char *str = (const unsigned char *)regex;
  tre_char_t *wstr;

  if (n > TRE_MAX_RE)
    return REG_ESPACE;
  wregex = xmalloc(sizeof(tre_char_t) * (n + 1));
  if (wregex == NULL) return REG_ESPACE;
  wstr = wregex;

  for (i = 0; i < n; i++)
    *(wstr++) = *(str++);
  wregex[n] = L'\0';
  ret = tre_compile(preg, wregex, n, cflags | REG_USEBYTES);
  xfree(wregex);
  return ret;
}


#ifdef TRE_WCHAR
int
tre_regwncomp(regex_t *preg, const wchar_t *regex, size_t n, int cflags)
{
  if (n > TRE_MAX_RE)
    return REG_ESPACE;
  return tre_compile(preg, regex, n, cflags);
}

int
tre_regwcomp(regex_t *preg, const wchar_t *regex, int cflags)
{
  size_t n = regex ? wcslen(regex) : 0;
  if (n > TRE_MAX_RE)
    return REG_ESPACE;
  return tre_compile(preg, regex, n, cflags);
}
#endif /* TRE_WCHAR */

void
tre_regfree(regex_t *preg)
{
  tre_free(preg);
}

/* EOF */
