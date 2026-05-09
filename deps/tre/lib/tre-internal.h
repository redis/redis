/*
  tre-internal.h - TRE internal definitions

  This software is released under a BSD-style license.
  See the file LICENSE for details and copyright.

*/

#ifndef TRE_INTERNAL_H
#define TRE_INTERNAL_H 1

#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif /* HAVE_WCHAR_H */

#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif /* HAVE_WCTYPE_H */

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#include <limits.h>
#include <ctype.h>
#include "../local_includes/tre.h"

#define TRE_MAX_RE	65536
#define TRE_MAX_STRING	INT_MAX
#define TRE_MAX_STACK	1048576

#ifdef TRE_DEBUG
#include <stdio.h>
#define DPRINT(msg) do {printf msg; fflush(stdout);} while(/*CONSTCOND*/(void)0,0)
#else /* !TRE_DEBUG */
#define DPRINT(msg) do { } while(/*CONSTCOND*/(void)0,0)
#endif /* !TRE_DEBUG */

#define elementsof(x)	( sizeof(x) / sizeof(x[0]) )

#ifdef HAVE_MBRTOWC
#define tre_mbrtowc(pwc, s, n, ps) (mbrtowc((pwc), (s), (n), (ps)))
#else /* !HAVE_MBRTOWC */
#ifdef HAVE_MBTOWC
#define tre_mbrtowc(pwc, s, n, ps) (mbtowc((pwc), (s), (n)))
#endif /* HAVE_MBTOWC */
#endif /* !HAVE_MBRTOWC */

#ifdef TRE_MULTIBYTE
#ifdef HAVE_MBSTATE_T
#define TRE_MBSTATE
#endif /* TRE_MULTIBYTE */
#endif /* HAVE_MBSTATE_T */

/* Define the character types and functions. */
#ifdef TRE_WCHAR

/* Wide characters. */
typedef wint_t tre_cint_t;
#if WCHAR_MAX <= INT_MAX
#define TRE_CHAR_MAX WCHAR_MAX
#else /* WCHAR_MAX > INT_MAX */
#define TRE_CHAR_MAX INT_MAX
#endif

#ifdef TRE_MULTIBYTE
#define TRE_MB_CUR_MAX MB_CUR_MAX
#else /* !TRE_MULTIBYTE */
#define TRE_MB_CUR_MAX 1
#endif /* !TRE_MULTIBYTE */

#define tre_isalnum iswalnum
#define tre_isalpha iswalpha
#ifdef HAVE_ISWBLANK
#define tre_isblank iswblank
#endif /* HAVE_ISWBLANK */
#define tre_iscntrl iswcntrl
#define tre_isdigit iswdigit
#define tre_isgraph iswgraph
#define tre_islower iswlower
#define tre_isprint iswprint
#define tre_ispunct iswpunct
#define tre_isspace iswspace
#define tre_isupper iswupper
#define tre_isxdigit iswxdigit

#define tre_tolower towlower
#define tre_toupper towupper
#define tre_strlen  wcslen

#else /* !TRE_WCHAR */

/* 8 bit characters. */
typedef short tre_cint_t;
#define TRE_CHAR_MAX 255
#define TRE_MB_CUR_MAX 1

#define tre_isalnum isalnum
#define tre_isalpha isalpha
#ifdef HAVE_ISASCII
#define tre_isascii isascii
#endif /* HAVE_ISASCII */
#ifdef HAVE_ISBLANK
#define tre_isblank isblank
#endif /* HAVE_ISBLANK */
#define tre_iscntrl iscntrl
#define tre_isdigit isdigit
#define tre_isgraph isgraph
#define tre_islower islower
#define tre_isprint isprint
#define tre_ispunct ispunct
#define tre_isspace isspace
#define tre_isupper isupper
#define tre_isxdigit isxdigit

#define tre_tolower(c) (tre_cint_t)(tolower(c))
#define tre_toupper(c) (tre_cint_t)(toupper(c))
#define tre_strlen(s)  (strlen((const char*)s))

#endif /* !TRE_WCHAR */

#if defined(TRE_WCHAR) && defined(HAVE_ISWCTYPE) && defined(HAVE_WCTYPE)
#define TRE_USE_SYSTEM_WCTYPE 1
#endif

#ifdef TRE_USE_SYSTEM_WCTYPE
/* Use system provided iswctype() and wctype(). */
typedef wctype_t tre_ctype_t;
#define tre_isctype iswctype
#define tre_ctype   wctype
#else /* !TRE_USE_SYSTEM_WCTYPE */
/* Define our own versions of iswctype() and wctype(). */
typedef int (*tre_ctype_t)(tre_cint_t);
#define tre_isctype(c, type) ( (type)(c) )
tre_ctype_t tre_ctype(const char *name);
#endif /* !TRE_USE_SYSTEM_WCTYPE */

typedef enum { STR_WIDE, STR_BYTE, STR_MBS, STR_USER } tre_str_type_t;

/* Returns number of bytes to add to (char *)ptr to make it
   properly aligned for the type. */
#define ALIGN(ptr, type) \
  ((((long)ptr) % sizeof(type)) \
   ? (sizeof(type) - (((long)ptr) % sizeof(type))) \
   : 0)

#undef MAX
#undef MIN
#define MAX(a, b) (((a) >= (b)) ? (a) : (b))
#define MIN(a, b) (((a) <= (b)) ? (a) : (b))

/* Define STRF to the correct printf formatter for strings. */
#ifdef TRE_WCHAR
#define STRF "ls"
#else /* !TRE_WCHAR */
#define STRF "s"
#endif /* !TRE_WCHAR */

/* TNFA transition type. A TNFA state is an array of transitions,
   the terminator is a transition with NULL `state'. */
typedef struct tnfa_transition tre_tnfa_transition_t;

struct tnfa_transition {
  /* Range of accepted characters. */
  tre_cint_t code_min;
  tre_cint_t code_max;
  /* Pointer to the destination state. */
  tre_tnfa_transition_t *state;
  /* ID number of the destination state. */
  int state_id;
  /* -1 terminated array of tags (or NULL). */
  int *tags;
  /* Matching parameters settings (or NULL). */
  int *params;
  /* Assertion bitmap. */
  int assertions;
  /* Assertion parameters. */
  union {
    /* Character class assertion. */
    tre_ctype_t class;
    /* Back reference assertion. */
    int backref;
  } u;
  /* Negative character class assertions. */
  tre_ctype_t *neg_classes;
};


/* Assertions. */
#define ASSERT_AT_BOL		  1   /* Beginning of line. */
#define ASSERT_AT_EOL		  2   /* End of line. */
#define ASSERT_CHAR_CLASS	  4   /* Character class in `class'. */
#define ASSERT_CHAR_CLASS_NEG	  8   /* Character classes in `neg_classes'. */
#define ASSERT_AT_BOW		 16   /* Beginning of word. */
#define ASSERT_AT_EOW		 32   /* End of word. */
#define ASSERT_AT_WB		 64   /* Word boundary. */
#define ASSERT_AT_WB_NEG	128   /* Not a word boundary. */
#define ASSERT_BACKREF		256   /* A back reference in `backref'. */
#define ASSERT_LAST		256

/* Tag directions. */
typedef enum {
  TRE_TAG_MINIMIZE = 0,
  TRE_TAG_MAXIMIZE = 1
} tre_tag_direction_t;

/* Parameters that can be changed dynamically while matching. */
typedef enum {
  TRE_PARAM_COST_INS	    = 0,
  TRE_PARAM_COST_DEL	    = 1,
  TRE_PARAM_COST_SUBST	    = 2,
  TRE_PARAM_COST_MAX	    = 3,
  TRE_PARAM_MAX_INS	    = 4,
  TRE_PARAM_MAX_DEL	    = 5,
  TRE_PARAM_MAX_SUBST	    = 6,
  TRE_PARAM_MAX_ERR	    = 7,
  TRE_PARAM_DEPTH	    = 8,
  TRE_PARAM_LAST	    = 9
} tre_param_t;

/* Unset matching parameter */
#define TRE_PARAM_UNSET -1

/* Signifies the default matching parameter value. */
#define TRE_PARAM_DEFAULT -2

/* Instructions to compute submatch register values from tag values
   after a successful match.  */
struct tre_submatch_data {
  /* Tag that gives the value for rm_so (submatch start offset). */
  int so_tag;
  /* Tag that gives the value for rm_eo (submatch end offset). */
  int eo_tag;
  /* List of submatches this submatch is contained in. */
  int *parents;
};

typedef struct tre_submatch_data tre_submatch_data_t;

typedef enum {
  TRE_LITERAL_OPT_NONE = 0,
  TRE_LITERAL_OPT_CONTAINS,
  TRE_LITERAL_OPT_PREFIX,
  TRE_LITERAL_OPT_SUFFIX,
  TRE_LITERAL_OPT_EXACT
} tre_literal_opt_mode_t;

typedef struct {
  unsigned char *data;
  size_t len;
} tre_literal_opt_literal_t;

typedef struct {
  tre_literal_opt_mode_t mode;
  int nocase;
  size_t num_literals;
  /* Folded byte mapping used by the nocase fast path. */
  unsigned char fold_map[256];
  /* Literal index ranges grouped by the first literal byte. */
  size_t start_offsets[257];
  tre_literal_opt_literal_t *literals;
} tre_literal_opt_t;


/* TNFA definition. */
typedef struct tnfa tre_tnfa_t;

struct tnfa {
  tre_tnfa_transition_t *transitions;
  unsigned int num_transitions;
  tre_tnfa_transition_t *initial;
  tre_tnfa_transition_t *final;
  tre_submatch_data_t *submatch_data;
  char *firstpos_chars;
  int first_char;
  unsigned int num_submatches;
  tre_tag_direction_t *tag_directions;
  int *minimal_tags;
  int num_tags;
  int num_minimals;
  int end_tag;
  int num_states;
  int cflags;
  int have_backrefs;
  int have_approx;
  int params_depth;
  tre_literal_opt_t literal_opt;
};

int
tre_compile(regex_t *preg, const tre_char_t *regex, size_t n, int cflags);

void
tre_free(regex_t *preg);

void
tre_fill_pmatch(size_t nmatch, regmatch_t pmatch[], int cflags,
		const tre_tnfa_t *tnfa, int *tags, int match_eo);

reg_errcode_t
tre_tnfa_run_parallel(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
		      tre_str_type_t type, int *match_tags, int eflags,
		      int *match_end_ofs);

reg_errcode_t
tre_tnfa_run_backtrack(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
		       tre_str_type_t type, int *match_tags, int eflags,
		       int *match_end_ofs);

#ifdef TRE_APPROX
reg_errcode_t
tre_tnfa_run_approx(const tre_tnfa_t *tnfa, const void *string, ssize_t len,
		    tre_str_type_t type, int *match_tags, regamatch_t *match,
		    regaparams_t params, int eflags, int *match_end_ofs);
#endif /* TRE_APPROX */

#endif /* TRE_INTERNAL_H */

/* EOF */
