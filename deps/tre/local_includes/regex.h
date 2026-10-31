/*
  regex.h - TRE legacy API

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

  This header is for source level compatibility with old code using
  the <tre/regex.h> header which defined the TRE API functions without
  a prefix.  New code should include <tre/tre.h> instead.

*/

#ifndef TRE_REXEX_H
#define TRE_REGEX_H 1

#ifdef USE_LOCAL_TRE_H
/* Use the header(s) from the TRE package that this file is part of.
   (Yes, this file is in local_include too, but the explict path
   means there is no way to get a system tre.h by accident.) */
#include "../local_includes/tre.h"
#else
/* Use the header(s) from an installed version of the TRE package
   (so that this application matches the installed libtre),
   not the one(s) in the local_includes directory. */
#include <tre/tre.h>
#endif

#ifndef TRE_USE_SYSTEM_REGEX_H
#define regcomp	   tre_regcomp
#define regerror   tre_regerror
#define regexec	   tre_regexec
#define regfree	   tre_regfree
#endif /* TRE_USE_SYSTEM_REGEX_H */

#define regacomp   tre_regacomp
#define regaexec   tre_regaexec
#define regancomp  tre_regancomp
#define reganexec  tre_reganexec
#define regawncomp tre_regawncomp
#define regawnexec tre_regawnexec
#define regncomp   tre_regncomp
#define regnexec   tre_regnexec
#define regwcomp   tre_regwcomp
#define regwexec   tre_regwexec
#define regwncomp  tre_regwncomp
#define regwnexec  tre_regwnexec

#endif /* TRE_REGEX_H */
