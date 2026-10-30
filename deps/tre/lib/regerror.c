/*
  tre_regerror.c - POSIX tre_regerror() implementation for TRE.

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <string.h>
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif /* HAVE_WCHAR_H */
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif /* HAVE_WCTYPE_H */

#include "tre-internal.h"

#ifdef HAVE_GETTEXT
#include <libintl.h>
#else
#define dgettext(p, s) s
#define gettext(s) s
#endif

#define _(String) dgettext(PACKAGE, String)
#define gettext_noop(String) String

#define xstr(s) str(s)
#define str(s) #s

/* Error message strings for error codes listed in `tre.h'.  This list
   needs to be in sync with the codes listed there, naturally. */
static const char *tre_error_messages[] =
  { gettext_noop("No error"),							/* REG_OK */
    gettext_noop("No match"),							/* REG_NOMATCH */
    gettext_noop("Invalid regexp"),						/* REG_BADPAT */
    gettext_noop("Unknown collating element"),					/* REG_ECOLLATE */
    gettext_noop("Unknown character class name"),				/* REG_ECTYPE */
    gettext_noop("Trailing backslash"),						/* REG_EESCAPE */
    gettext_noop("Invalid back reference"),					/* REG_ESUBREG */
    gettext_noop("Missing ']'"),						/* REG_EBRACK */
    gettext_noop("Missing ')'"),						/* REG_EPAREN */
    gettext_noop("Missing '}'"),						/* REG_EBRACE */
    gettext_noop("Invalid contents of {}"),					/* REG_BADBR */
    gettext_noop("Invalid character range"),					/* REG_ERANGE */
    gettext_noop("Out of memory"),						/* REG_ESPACE */
    gettext_noop("Invalid use of repetition operators"),			/* REG_BADRPT */
    gettext_noop("Maximum repetition in {} larger than " xstr(RE_DUP_MAX)),	/* REG_BADMAX */
  };

size_t
tre_regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
  const char *err;
  size_t err_len;

  /*LINTED*/(void)&preg;
  if (errcode >= 0
      && errcode < (int)(sizeof(tre_error_messages)
			 / sizeof(*tre_error_messages)))
    err = gettext(tre_error_messages[errcode]);
  else
    err = gettext("Unknown error");

  err_len = strlen(err) + 1;
  if (errbuf_size > 0 && errbuf != NULL)
    {
      if (err_len > errbuf_size)
	{
	  strncpy(errbuf, err, errbuf_size - 1);
	  errbuf[errbuf_size - 1] = '\0';
	}
      else
	{
	  strcpy(errbuf, err);
	}
    }
  return err_len;
}

/* EOF */
