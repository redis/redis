#ifndef ONIGURUMA_H
#define ONIGURUMA_H
/**********************************************************************
  oniguruma.h - Oniguruma (regular expression library)
**********************************************************************/
/*-
 * Copyright (c) 2002-2008  K.Kosako  <sndgk393 AT ybb DOT ne DOT jp>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ONIGURUMA
#define ONIGURUMA_VERSION_MAJOR   5
#define ONIGURUMA_VERSION_MINOR   9
#define ONIGURUMA_VERSION_TEENY   2

#ifdef __cplusplus
# ifndef  HAVE_PROTOTYPES
#  define HAVE_PROTOTYPES 1
# endif
# ifndef  HAVE_STDARG_PROTOTYPES
#  define HAVE_STDARG_PROTOTYPES 1
# endif
#endif

/* escape Mac OS X/Xcode 2.4/gcc 4.0.1 problem */
#if defined(__APPLE__) && defined(__GNUC__) && __GNUC__ >= 4
# ifndef  HAVE_STDARG_PROTOTYPES
#  define HAVE_STDARG_PROTOTYPES 1
# endif
#endif

#ifndef ONIG_EXTERN
#ifdef RUBY_EXTERN
#define ONIG_EXTERN   RUBY_EXTERN
#else
#if defined(_WIN32) && !defined(__GNUC__)
#if defined(EXPORT) || defined(RUBY_EXPORT)
#define ONIG_EXTERN   extern __declspec(dllexport)
#else
#define ONIG_EXTERN   extern __declspec(dllimport)
#endif
#endif
#endif
#endif

#ifndef ONIG_EXTERN
#define ONIG_EXTERN   extern
#endif

/* PART: character encoding */

#ifndef ONIG_ESCAPE_UCHAR_COLLISION
#define UChar OnigUChar
#endif

typedef unsigned char  OnigUChar;
typedef unsigned int  OnigCodePoint;
typedef unsigned int   OnigCtype;
typedef size_t         OnigDistance;

#define ONIG_INFINITE_DISTANCE  ~((OnigDistance )0)

typedef unsigned int OnigCaseFoldType; /* case fold flag */

ONIG_EXTERN OnigCaseFoldType OnigDefaultCaseFoldFlag;

/* #define ONIGENC_CASE_FOLD_HIRAGANA_KATAKANA  (1<<1) */
/* #define ONIGENC_CASE_FOLD_KATAKANA_WIDTH     (1<<2) */
#define ONIGENC_CASE_FOLD_TURKISH_AZERI         (1<<20)
#define INTERNAL_ONIGENC_CASE_FOLD_MULTI_CHAR   (1<<30)

#define ONIGENC_CASE_FOLD_MIN      INTERNAL_ONIGENC_CASE_FOLD_MULTI_CHAR
#define ONIGENC_CASE_FOLD_DEFAULT  OnigDefaultCaseFoldFlag


#define ONIGENC_MAX_COMP_CASE_FOLD_CODE_LEN       3
#define ONIGENC_GET_CASE_FOLD_CODES_MAX_NUM      13
/* 13 => Unicode:0x1ffc */

/* code range */
#define ONIGENC_CODE_RANGE_NUM(range)     ((int )range[0])
#define ONIGENC_CODE_RANGE_FROM(range,i)  range[((i)*2) + 1]
#define ONIGENC_CODE_RANGE_TO(range,i)    range[((i)*2) + 2]

typedef struct {
  int byte_len;  /* argument(original) character(s) byte length */
  int code_len;  /* number of code */
  OnigCodePoint code[ONIGENC_MAX_COMP_CASE_FOLD_CODE_LEN];
} OnigCaseFoldCodeItem;

typedef struct {
  OnigCodePoint esc;
  OnigCodePoint anychar;
  OnigCodePoint anytime;
  OnigCodePoint zero_or_one_time;
  OnigCodePoint one_or_more_time;
  OnigCodePoint anychar_anytime;
} OnigMetaCharTableType;

typedef int (*OnigApplyAllCaseFoldFunc)(OnigCodePoint from, OnigCodePoint* to, int to_len, void* arg);

typedef struct OnigEncodingTypeST {
  int    (*precise_mbc_enc_len)(const OnigUChar* p,const OnigUChar* e, struct OnigEncodingTypeST* enc);
  const char*   name;
  int           max_enc_len;
  int           min_enc_len;
  int    (*is_mbc_newline)(const OnigUChar* p, const OnigUChar* end, struct OnigEncodingTypeST* enc);
  OnigCodePoint (*mbc_to_code)(const OnigUChar* p, const OnigUChar* end, struct OnigEncodingTypeST* enc);
  int    (*code_to_mbclen)(OnigCodePoint code, struct OnigEncodingTypeST* enc);
  int    (*code_to_mbc)(OnigCodePoint code, OnigUChar *buf, struct OnigEncodingTypeST* enc);
  int    (*mbc_case_fold)(OnigCaseFoldType flag, const OnigUChar** pp, const OnigUChar* end, OnigUChar* to, struct OnigEncodingTypeST* enc);
  int    (*apply_all_case_fold)(OnigCaseFoldType flag, OnigApplyAllCaseFoldFunc f, void* arg, struct OnigEncodingTypeST* enc);
  int    (*get_case_fold_codes_by_str)(OnigCaseFoldType flag, const OnigUChar* p, const OnigUChar* end, OnigCaseFoldCodeItem acs[], struct OnigEncodingTypeST* enc);
  int    (*property_name_to_ctype)(struct OnigEncodingTypeST* enc, OnigUChar* p, OnigUChar* end);
  int    (*is_code_ctype)(OnigCodePoint code, OnigCtype ctype, struct OnigEncodingTypeST* enc);
  int    (*get_ctype_code_range)(OnigCtype ctype, OnigCodePoint* sb_out, const OnigCodePoint* ranges[], struct OnigEncodingTypeST* enc);
  OnigUChar* (*left_adjust_char_head)(const OnigUChar* start, const OnigUChar* p, const OnigUChar* end, struct OnigEncodingTypeST* enc);
  int    (*is_allowed_reverse_match)(const OnigUChar* p, const OnigUChar* end, struct OnigEncodingTypeST* enc);
  int ruby_encoding_index;
} OnigEncodingType;

typedef OnigEncodingType* OnigEncoding;

ONIG_EXTERN OnigEncodingType OnigEncodingASCII;

#define ONIG_ENCODING_ASCII        (&OnigEncodingASCII)

#define ONIG_ENCODING_UNDEF    ((OnigEncoding )0)


/* work size */
#define ONIGENC_CODE_TO_MBC_MAXLEN       7
#define ONIGENC_MBC_CASE_FOLD_MAXLEN    18
/* 18: 6(max-byte) * 3(case-fold chars) */

/* character types */
#define ONIGENC_CTYPE_NEWLINE   0
#define ONIGENC_CTYPE_ALPHA     1
#define ONIGENC_CTYPE_BLANK     2
#define ONIGENC_CTYPE_CNTRL     3
#define ONIGENC_CTYPE_DIGIT     4
#define ONIGENC_CTYPE_GRAPH     5
#define ONIGENC_CTYPE_LOWER     6
#define ONIGENC_CTYPE_PRINT     7
#define ONIGENC_CTYPE_PUNCT     8
#define ONIGENC_CTYPE_SPACE     9
#define ONIGENC_CTYPE_UPPER    10
#define ONIGENC_CTYPE_XDIGIT   11
#define ONIGENC_CTYPE_WORD     12
#define ONIGENC_CTYPE_ALNUM    13  /* alpha || digit */
#define ONIGENC_CTYPE_ASCII    14
#define ONIGENC_MAX_STD_CTYPE  ONIGENC_CTYPE_ASCII
#define ONIGENC_CTYPE_SPECIAL_MASK        128
#define ONIGENC_CTYPE_S            /* [\t\n\v\f\r\s] */ \
    ONIGENC_CTYPE_SPECIAL_MASK | ONIGENC_CTYPE_SPACE
#define ONIGENC_CTYPE_D            /* [0-9] */ \
    ONIGENC_CTYPE_SPECIAL_MASK | ONIGENC_CTYPE_DIGIT
#define ONIGENC_CTYPE_W            /* [0-9A-Za-z_] */ \
    ONIGENC_CTYPE_SPECIAL_MASK | ONIGENC_CTYPE_WORD
#define ONIGENC_CTYPE_SPECIAL_P(ctype) ((ctype) & ONIGENC_CTYPE_SPECIAL_MASK)


#define onig_enc_len(enc,p,e)                ONIGENC_MBC_ENC_LEN(enc, p, e)

#define ONIGENC_IS_UNDEF(enc)          ((enc) == ONIG_ENCODING_UNDEF)
#define ONIGENC_IS_SINGLEBYTE(enc)     (ONIGENC_MBC_MAXLEN(enc) == 1)
#define ONIGENC_IS_MBC_HEAD(enc,p,e)   (ONIGENC_MBC_ENC_LEN(enc,p,e) != 1)
#define ONIGENC_IS_MBC_ASCII(p)           (*(p)   < 128)
#define ONIGENC_IS_CODE_ASCII(code)       ((code) < 128)
#define ONIGENC_IS_MBC_WORD(enc,s,end) \
   ONIGENC_IS_CODE_WORD(enc,ONIGENC_MBC_TO_CODE(enc,s,end))


#define ONIGENC_NAME(enc)                      ((enc)->name)

#define ONIGENC_MBC_CASE_FOLD(enc,flag,pp,end,buf) \
  (enc)->mbc_case_fold(flag,(const OnigUChar** )pp,end,buf,enc)
#define ONIGENC_IS_ALLOWED_REVERSE_MATCH(enc,s,end) \
        (enc)->is_allowed_reverse_match(s,end,enc)
#define ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc,start,s,end) \
        (enc)->left_adjust_char_head(start, s, end, enc)
#define ONIGENC_APPLY_ALL_CASE_FOLD(enc,case_fold_flag,f,arg) \
        (enc)->apply_all_case_fold(case_fold_flag,f,arg,enc)
#define ONIGENC_GET_CASE_FOLD_CODES_BY_STR(enc,case_fold_flag,p,end,acs) \
       (enc)->get_case_fold_codes_by_str(case_fold_flag,p,end,acs,enc)
#define ONIGENC_STEP_BACK(enc,start,s,end,n) \
        onigenc_step_back((enc),(start),(s),(end),(n))

#define ONIGENC_CONSTRUCT_MBCLEN_CHARFOUND(n)   (n)
#define ONIGENC_MBCLEN_CHARFOUND_P(r)           (0 < (r))
#define ONIGENC_MBCLEN_CHARFOUND_LEN(r)         (r)

#define ONIGENC_CONSTRUCT_MBCLEN_INVALID()      (-1)
#define ONIGENC_MBCLEN_INVALID_P(r)             ((r) == -1)

#define ONIGENC_CONSTRUCT_MBCLEN_NEEDMORE(n)    (-1-(n))
#define ONIGENC_MBCLEN_NEEDMORE_P(r)            ((r) < -1)
#define ONIGENC_MBCLEN_NEEDMORE_LEN(r)          (-1-(r))

#define ONIGENC_PRECISE_MBC_ENC_LEN(enc,p,e)   (enc)->precise_mbc_enc_len(p,e,enc)

ONIG_EXTERN
int onigenc_mbclen_approximate(const OnigUChar* p,const OnigUChar* e, struct OnigEncodingTypeST* enc);

#define ONIGENC_MBC_ENC_LEN(enc,p,e)           onigenc_mbclen_approximate(p,e,enc)
#define ONIGENC_MBC_MAXLEN(enc)               ((enc)->max_enc_len)
#define ONIGENC_MBC_MAXLEN_DIST(enc)           ONIGENC_MBC_MAXLEN(enc)
#define ONIGENC_MBC_MINLEN(enc)               ((enc)->min_enc_len)
#define ONIGENC_IS_MBC_NEWLINE(enc,p,end)      (enc)->is_mbc_newline((p),(end),enc)
#define ONIGENC_MBC_TO_CODE(enc,p,end)         (enc)->mbc_to_code((p),(end),enc)
#define ONIGENC_CODE_TO_MBCLEN(enc,code)       (enc)->code_to_mbclen(code,enc)
#define ONIGENC_CODE_TO_MBC(enc,code,buf)      (enc)->code_to_mbc(code,buf,enc)
#define ONIGENC_PROPERTY_NAME_TO_CTYPE(enc,p,end) \
  (enc)->property_name_to_ctype(enc,p,end)

#define ONIGENC_IS_CODE_CTYPE(enc,code,ctype)  (enc)->is_code_ctype(code,ctype,enc)

#define ONIGENC_IS_CODE_NEWLINE(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_NEWLINE)
#define ONIGENC_IS_CODE_GRAPH(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_GRAPH)
#define ONIGENC_IS_CODE_PRINT(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_PRINT)
#define ONIGENC_IS_CODE_ALNUM(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_ALNUM)
#define ONIGENC_IS_CODE_ALPHA(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_ALPHA)
#define ONIGENC_IS_CODE_LOWER(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_LOWER)
#define ONIGENC_IS_CODE_UPPER(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_UPPER)
#define ONIGENC_IS_CODE_CNTRL(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_CNTRL)
#define ONIGENC_IS_CODE_PUNCT(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_PUNCT)
#define ONIGENC_IS_CODE_SPACE(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_SPACE)
#define ONIGENC_IS_CODE_BLANK(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_BLANK)
#define ONIGENC_IS_CODE_DIGIT(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_DIGIT)
#define ONIGENC_IS_CODE_XDIGIT(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_XDIGIT)
#define ONIGENC_IS_CODE_WORD(enc,code) \
        ONIGENC_IS_CODE_CTYPE(enc,code,ONIGENC_CTYPE_WORD)

#define ONIGENC_GET_CTYPE_CODE_RANGE(enc,ctype,sbout,ranges) \
        (enc)->get_ctype_code_range(ctype,sbout,ranges,enc)

ONIG_EXTERN
OnigUChar* onigenc_step_back(OnigEncoding enc, const OnigUChar* start, const OnigUChar* s, const OnigUChar* end, int n);


/* encoding API */
ONIG_EXTERN
int onigenc_init(void);
ONIG_EXTERN
int onigenc_set_default_encoding(OnigEncoding enc);
ONIG_EXTERN
OnigEncoding onigenc_get_default_encoding(void);
ONIG_EXTERN
void  onigenc_set_default_caseconv_table(const OnigUChar* table);
ONIG_EXTERN
OnigUChar* onigenc_get_right_adjust_char_head_with_prev(OnigEncoding enc, const OnigUChar* start, const OnigUChar* s, const OnigUChar* end, const OnigUChar** prev);
ONIG_EXTERN
OnigUChar* onigenc_get_prev_char_head(OnigEncoding enc, const OnigUChar* start, const OnigUChar* s, const OnigUChar* end);
ONIG_EXTERN
OnigUChar* onigenc_get_left_adjust_char_head(OnigEncoding enc, const OnigUChar* start, const OnigUChar* s, const OnigUChar* end);
ONIG_EXTERN
OnigUChar* onigenc_get_right_adjust_char_head(OnigEncoding enc, const OnigUChar* start, const OnigUChar* s, const OnigUChar* end);
ONIG_EXTERN
int onigenc_strlen(OnigEncoding enc, const OnigUChar* p, const OnigUChar* end);
ONIG_EXTERN
int onigenc_strlen_null(OnigEncoding enc, const OnigUChar* p);
ONIG_EXTERN
int onigenc_str_bytelen_null(OnigEncoding enc, const OnigUChar* p);



/* PART: regular expression */

/* config parameters */
#define ONIG_NREGION                          10
#define ONIG_MAX_BACKREF_NUM                1000
#define ONIG_MAX_REPEAT_NUM               100000
#define ONIG_MAX_MULTI_BYTE_RANGES_NUM     10000
/* constants */
#define ONIG_MAX_ERROR_MESSAGE_LEN            90

typedef unsigned int        OnigOptionType;

#define ONIG_OPTION_DEFAULT            ONIG_OPTION_NONE

/* options */
#define ONIG_OPTION_NONE                 0U
#define ONIG_OPTION_IGNORECASE           1U
#define ONIG_OPTION_EXTEND               (ONIG_OPTION_IGNORECASE         << 1)
#define ONIG_OPTION_MULTILINE            (ONIG_OPTION_EXTEND             << 1)
#define ONIG_OPTION_SINGLELINE           (ONIG_OPTION_MULTILINE          << 1)
#define ONIG_OPTION_FIND_LONGEST         (ONIG_OPTION_SINGLELINE         << 1)
#define ONIG_OPTION_FIND_NOT_EMPTY       (ONIG_OPTION_FIND_LONGEST       << 1)
#define ONIG_OPTION_NEGATE_SINGLELINE    (ONIG_OPTION_FIND_NOT_EMPTY     << 1)
#define ONIG_OPTION_DONT_CAPTURE_GROUP   (ONIG_OPTION_NEGATE_SINGLELINE  << 1)
#define ONIG_OPTION_CAPTURE_GROUP        (ONIG_OPTION_DONT_CAPTURE_GROUP << 1)
/* options (search time) */
#define ONIG_OPTION_NOTBOL               (ONIG_OPTION_CAPTURE_GROUP << 1)
#define ONIG_OPTION_NOTEOL               (ONIG_OPTION_NOTBOL << 1)
#define ONIG_OPTION_POSIX_REGION         (ONIG_OPTION_NOTEOL << 1)
#define ONIG_OPTION_MAXBIT               ONIG_OPTION_POSIX_REGION  /* limit */

#define ONIG_OPTION_ON(options,regopt)      ((options) |= (regopt))
#define ONIG_OPTION_OFF(options,regopt)     ((options) &= ~(regopt))
#define ONIG_IS_OPTION_ON(options,option)   ((options) & (option))

/* syntax */
typedef struct {
  unsigned int   op;
  unsigned int   op2;
  unsigned int   behavior;
  OnigOptionType options;   /* default option */
  OnigMetaCharTableType meta_char_table;
} OnigSyntaxType;

ONIG_EXTERN const OnigSyntaxType OnigSyntaxASIS;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxPosixBasic;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxPosixExtended;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxEmacs;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxGrep;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxGnuRegex;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxJava;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxPerl;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxPerl_NG;
ONIG_EXTERN const OnigSyntaxType OnigSyntaxRuby;

/* predefined syntaxes (see regsyntax.c) */
#define ONIG_SYNTAX_ASIS               (&OnigSyntaxASIS)
#define ONIG_SYNTAX_POSIX_BASIC        (&OnigSyntaxPosixBasic)
#define ONIG_SYNTAX_POSIX_EXTENDED     (&OnigSyntaxPosixExtended)
#define ONIG_SYNTAX_EMACS              (&OnigSyntaxEmacs)
#define ONIG_SYNTAX_GREP               (&OnigSyntaxGrep)
#define ONIG_SYNTAX_GNU_REGEX          (&OnigSyntaxGnuRegex)
#define ONIG_SYNTAX_JAVA               (&OnigSyntaxJava)
#define ONIG_SYNTAX_PERL               (&OnigSyntaxPerl)
#define ONIG_SYNTAX_PERL_NG            (&OnigSyntaxPerl_NG)
#define ONIG_SYNTAX_RUBY               (&OnigSyntaxRuby)

/* default syntax */
ONIG_EXTERN const OnigSyntaxType*   OnigDefaultSyntax;
#define ONIG_SYNTAX_DEFAULT   OnigDefaultSyntax

/* syntax (operators) */
#define ONIG_SYN_OP_VARIABLE_META_CHARACTERS    (1U<<0)
#define ONIG_SYN_OP_DOT_ANYCHAR                 (1U<<1)   /* . */
#define ONIG_SYN_OP_ASTERISK_ZERO_INF           (1U<<2)   /* * */
#define ONIG_SYN_OP_ESC_ASTERISK_ZERO_INF       (1U<<3)
#define ONIG_SYN_OP_PLUS_ONE_INF                (1U<<4)   /* + */
#define ONIG_SYN_OP_ESC_PLUS_ONE_INF            (1U<<5)
#define ONIG_SYN_OP_QMARK_ZERO_ONE              (1U<<6)   /* ? */
#define ONIG_SYN_OP_ESC_QMARK_ZERO_ONE          (1U<<7)
#define ONIG_SYN_OP_BRACE_INTERVAL              (1U<<8)   /* {lower,upper} */
#define ONIG_SYN_OP_ESC_BRACE_INTERVAL          (1U<<9)   /* \{lower,upper\} */
#define ONIG_SYN_OP_VBAR_ALT                    (1U<<10)   /* | */
#define ONIG_SYN_OP_ESC_VBAR_ALT                (1U<<11)  /* \| */
#define ONIG_SYN_OP_LPAREN_SUBEXP               (1U<<12)  /* (...)   */
#define ONIG_SYN_OP_ESC_LPAREN_SUBEXP           (1U<<13)  /* \(...\) */
#define ONIG_SYN_OP_ESC_AZ_BUF_ANCHOR           (1U<<14)  /* \A, \Z, \z */
#define ONIG_SYN_OP_ESC_CAPITAL_G_BEGIN_ANCHOR  (1U<<15)  /* \G     */
#define ONIG_SYN_OP_DECIMAL_BACKREF             (1U<<16)  /* \num   */
#define ONIG_SYN_OP_BRACKET_CC                  (1U<<17)  /* [...]  */
#define ONIG_SYN_OP_ESC_W_WORD                  (1U<<18)  /* \w, \W */
#define ONIG_SYN_OP_ESC_LTGT_WORD_BEGIN_END     (1U<<19)  /* \<. \> */
#define ONIG_SYN_OP_ESC_B_WORD_BOUND            (1U<<20)  /* \b, \B */
#define ONIG_SYN_OP_ESC_S_WHITE_SPACE           (1U<<21)  /* \s, \S */
#define ONIG_SYN_OP_ESC_D_DIGIT                 (1U<<22)  /* \d, \D */
#define ONIG_SYN_OP_LINE_ANCHOR                 (1U<<23)  /* ^, $   */
#define ONIG_SYN_OP_POSIX_BRACKET               (1U<<24)  /* [:xxxx:] */
#define ONIG_SYN_OP_QMARK_NON_GREEDY            (1U<<25)  /* ??,*?,+?,{n,m}? */
#define ONIG_SYN_OP_ESC_CONTROL_CHARS           (1U<<26)  /* \n,\r,\t,\a ... */
#define ONIG_SYN_OP_ESC_C_CONTROL               (1U<<27)  /* \cx  */
#define ONIG_SYN_OP_ESC_OCTAL3                  (1U<<28)  /* \OOO */
#define ONIG_SYN_OP_ESC_X_HEX2                  (1U<<29)  /* \xHH */
#define ONIG_SYN_OP_ESC_X_BRACE_HEX8            (1U<<30)  /* \x{7HHHHHHH} */

#define ONIG_SYN_OP2_ESC_CAPITAL_Q_QUOTE        (1U<<0)  /* \Q...\E */
#define ONIG_SYN_OP2_QMARK_GROUP_EFFECT         (1U<<1)  /* (?...) */
#define ONIG_SYN_OP2_OPTION_PERL                (1U<<2)  /* (?imsx),(?-imsx) */
#define ONIG_SYN_OP2_OPTION_RUBY                (1U<<3)  /* (?imx), (?-imx)  */
#define ONIG_SYN_OP2_PLUS_POSSESSIVE_REPEAT     (1U<<4)  /* ?+,*+,++ */
#define ONIG_SYN_OP2_PLUS_POSSESSIVE_INTERVAL   (1U<<5)  /* {n,m}+   */
#define ONIG_SYN_OP2_CCLASS_SET_OP              (1U<<6)  /* [...&&..[..]..] */
#define ONIG_SYN_OP2_QMARK_LT_NAMED_GROUP       (1U<<7)  /* (?<name>...) */
#define ONIG_SYN_OP2_ESC_K_NAMED_BACKREF        (1U<<8)  /* \k<name> */
#define ONIG_SYN_OP2_ESC_G_SUBEXP_CALL          (1U<<9)  /* \g<name>, \g<n> */
#define ONIG_SYN_OP2_ATMARK_CAPTURE_HISTORY     (1U<<10) /* (?@..),(?@<x>..) */
#define ONIG_SYN_OP2_ESC_CAPITAL_C_BAR_CONTROL  (1U<<11) /* \C-x */
#define ONIG_SYN_OP2_ESC_CAPITAL_M_BAR_META     (1U<<12) /* \M-x */
#define ONIG_SYN_OP2_ESC_V_VTAB                 (1U<<13) /* \v as VTAB */
#define ONIG_SYN_OP2_ESC_U_HEX4                 (1U<<14) /* \uHHHH */
#define ONIG_SYN_OP2_ESC_GNU_BUF_ANCHOR         (1U<<15) /* \`, \' */
#define ONIG_SYN_OP2_ESC_P_BRACE_CHAR_PROPERTY  (1U<<16) /* \p{...}, \P{...} */
#define ONIG_SYN_OP2_ESC_P_BRACE_CIRCUMFLEX_NOT (1U<<17) /* \p{^..}, \P{^..} */
/* #define ONIG_SYN_OP2_CHAR_PROPERTY_PREFIX_IS (1U<<18) */
#define ONIG_SYN_OP2_ESC_H_XDIGIT               (1U<<19) /* \h, \H */
#define ONIG_SYN_OP2_INEFFECTIVE_ESCAPE         (1U<<20) /* \ */

/* syntax (behavior) */
#define ONIG_SYN_CONTEXT_INDEP_ANCHORS           (1U<<31) /* not implemented */
#define ONIG_SYN_CONTEXT_INDEP_REPEAT_OPS        (1U<<0)  /* ?, *, +, {n,m} */
#define ONIG_SYN_CONTEXT_INVALID_REPEAT_OPS      (1U<<1)  /* error or ignore */
#define ONIG_SYN_ALLOW_UNMATCHED_CLOSE_SUBEXP    (1U<<2)  /* ...)... */
#define ONIG_SYN_ALLOW_INVALID_INTERVAL          (1U<<3)  /* {??? */
#define ONIG_SYN_ALLOW_INTERVAL_LOW_ABBREV       (1U<<4)  /* {,n} => {0,n} */
#define ONIG_SYN_STRICT_CHECK_BACKREF            (1U<<5)  /* /(\1)/,/\1()/ ..*/
#define ONIG_SYN_DIFFERENT_LEN_ALT_LOOK_BEHIND   (1U<<6)  /* (?<=a|bc) */
#define ONIG_SYN_CAPTURE_ONLY_NAMED_GROUP        (1U<<7)  /* see doc/RE */
#define ONIG_SYN_ALLOW_MULTIPLEX_DEFINITION_NAME (1U<<8)  /* (?<x>)(?<x>) */
#define ONIG_SYN_FIXED_INTERVAL_IS_GREEDY_ONLY   (1U<<9)  /* a{n}?=(?:a{n})? */

/* syntax (behavior) in char class [...] */
#define ONIG_SYN_NOT_NEWLINE_IN_NEGATIVE_CC      (1U<<20) /* [^...] */
#define ONIG_SYN_BACKSLASH_ESCAPE_IN_CC          (1U<<21) /* [..\w..] etc.. */
#define ONIG_SYN_ALLOW_EMPTY_RANGE_IN_CC         (1U<<22)
#define ONIG_SYN_ALLOW_DOUBLE_RANGE_OP_IN_CC     (1U<<23) /* [0-9-a]=[0-9\-a] */
/* syntax (behavior) warning */
#define ONIG_SYN_WARN_CC_OP_NOT_ESCAPED          (1U<<24) /* [,-,] */
#define ONIG_SYN_WARN_REDUNDANT_NESTED_REPEAT    (1U<<25) /* (?:a*)+ */
#define ONIG_SYN_WARN_CC_DUP                     (1U<<26) /* [aa] */

/* meta character specifiers (onig_set_meta_char()) */
#define ONIG_META_CHAR_ESCAPE               0
#define ONIG_META_CHAR_ANYCHAR              1
#define ONIG_META_CHAR_ANYTIME              2
#define ONIG_META_CHAR_ZERO_OR_ONE_TIME     3
#define ONIG_META_CHAR_ONE_OR_MORE_TIME     4
#define ONIG_META_CHAR_ANYCHAR_ANYTIME      5

#define ONIG_INEFFECTIVE_META_CHAR          0

/* error codes */
#define ONIG_IS_PATTERN_ERROR(ecode)   ((ecode) <= -100 && (ecode) > -1000)
/* normal return */
#define ONIG_NORMAL                                            0
#define ONIG_MISMATCH                                         -1
#define ONIG_NO_SUPPORT_CONFIG                                -2

/* internal error */
#define ONIGERR_MEMORY                                         -5
#define ONIGERR_TYPE_BUG                                       -6
#define ONIGERR_PARSER_BUG                                    -11
#define ONIGERR_STACK_BUG                                     -12
#define ONIGERR_UNDEFINED_BYTECODE                            -13
#define ONIGERR_UNEXPECTED_BYTECODE                           -14
#define ONIGERR_MATCH_STACK_LIMIT_OVER                        -15
#define ONIGERR_DEFAULT_ENCODING_IS_NOT_SETTED                -21
#define ONIGERR_SPECIFIED_ENCODING_CANT_CONVERT_TO_WIDE_CHAR  -22
/* general error */
#define ONIGERR_INVALID_ARGUMENT                              -30
/* syntax error */
#define ONIGERR_END_PATTERN_AT_LEFT_BRACE                    -100
#define ONIGERR_END_PATTERN_AT_LEFT_BRACKET                  -101
#define ONIGERR_EMPTY_CHAR_CLASS                             -102
#define ONIGERR_PREMATURE_END_OF_CHAR_CLASS                  -103
#define ONIGERR_END_PATTERN_AT_ESCAPE                        -104
#define ONIGERR_END_PATTERN_AT_META                          -105
#define ONIGERR_END_PATTERN_AT_CONTROL                       -106
#define ONIGERR_META_CODE_SYNTAX                             -108
#define ONIGERR_CONTROL_CODE_SYNTAX                          -109
#define ONIGERR_CHAR_CLASS_VALUE_AT_END_OF_RANGE             -110
#define ONIGERR_CHAR_CLASS_VALUE_AT_START_OF_RANGE           -111
#define ONIGERR_UNMATCHED_RANGE_SPECIFIER_IN_CHAR_CLASS      -112
#define ONIGERR_TARGET_OF_REPEAT_OPERATOR_NOT_SPECIFIED      -113
#define ONIGERR_TARGET_OF_REPEAT_OPERATOR_INVALID            -114
#define ONIGERR_NESTED_REPEAT_OPERATOR                       -115
#define ONIGERR_UNMATCHED_CLOSE_PARENTHESIS                  -116
#define ONIGERR_END_PATTERN_WITH_UNMATCHED_PARENTHESIS       -117
#define ONIGERR_END_PATTERN_IN_GROUP                         -118
#define ONIGERR_UNDEFINED_GROUP_OPTION                       -119
#define ONIGERR_INVALID_POSIX_BRACKET_TYPE                   -121
#define ONIGERR_INVALID_LOOK_BEHIND_PATTERN                  -122
#define ONIGERR_INVALID_REPEAT_RANGE_PATTERN                 -123
/* values error (syntax error) */
#define ONIGERR_TOO_BIG_NUMBER                               -200
#define ONIGERR_TOO_BIG_NUMBER_FOR_REPEAT_RANGE              -201
#define ONIGERR_UPPER_SMALLER_THAN_LOWER_IN_REPEAT_RANGE     -202
#define ONIGERR_EMPTY_RANGE_IN_CHAR_CLASS                    -203
#define ONIGERR_MISMATCH_CODE_LENGTH_IN_CLASS_RANGE          -204
#define ONIGERR_TOO_MANY_MULTI_BYTE_RANGES                   -205
#define ONIGERR_TOO_SHORT_MULTI_BYTE_STRING                  -206
#define ONIGERR_TOO_BIG_BACKREF_NUMBER                       -207
#define ONIGERR_INVALID_BACKREF                              -208
#define ONIGERR_NUMBERED_BACKREF_OR_CALL_NOT_ALLOWED         -209
#define ONIGERR_TOO_LONG_WIDE_CHAR_VALUE                     -212
#define ONIGERR_EMPTY_GROUP_NAME                             -214
#define ONIGERR_INVALID_GROUP_NAME                           -215
#define ONIGERR_INVALID_CHAR_IN_GROUP_NAME                   -216
#define ONIGERR_UNDEFINED_NAME_REFERENCE                     -217
#define ONIGERR_UNDEFINED_GROUP_REFERENCE                    -218
#define ONIGERR_MULTIPLEX_DEFINED_NAME                       -219
#define ONIGERR_MULTIPLEX_DEFINITION_NAME_CALL               -220
#define ONIGERR_NEVER_ENDING_RECURSION                       -221
#define ONIGERR_GROUP_NUMBER_OVER_FOR_CAPTURE_HISTORY        -222
#define ONIGERR_INVALID_CHAR_PROPERTY_NAME                   -223
#define ONIGERR_INVALID_CODE_POINT_VALUE                     -400
#define ONIGERR_INVALID_WIDE_CHAR_VALUE                      -400
#define ONIGERR_TOO_BIG_WIDE_CHAR_VALUE                      -401
#define ONIGERR_NOT_SUPPORTED_ENCODING_COMBINATION           -402
#define ONIGERR_INVALID_COMBINATION_OF_OPTIONS               -403

/* errors related to thread */
#define ONIGERR_OVER_THREAD_PASS_LIMIT_COUNT                -1001


/* must be smaller than BIT_STATUS_BITS_NUM (unsigned int * 8) */
#define ONIG_MAX_CAPTURE_HISTORY_GROUP   31
#define ONIG_IS_CAPTURE_HISTORY_GROUP(r, i) \
  ((i) <= ONIG_MAX_CAPTURE_HISTORY_GROUP && (r)->list && (r)->list[i])

typedef struct OnigCaptureTreeNodeStruct {
  int group;   /* group number */
  int beg;
  int end;
  int allocated;
  int num_childs;
  struct OnigCaptureTreeNodeStruct** childs;
} OnigCaptureTreeNode;

/* match result region type */
struct re_registers {
  int  allocated;
  int  num_regs;
  int* beg;
  int* end;
  /* extended */
  OnigCaptureTreeNode* history_root;  /* capture history tree root */
};

/* capture tree traverse */
#define ONIG_TRAVERSE_CALLBACK_AT_FIRST   1
#define ONIG_TRAVERSE_CALLBACK_AT_LAST    2
#define ONIG_TRAVERSE_CALLBACK_AT_BOTH \
  ( ONIG_TRAVERSE_CALLBACK_AT_FIRST | ONIG_TRAVERSE_CALLBACK_AT_LAST )


#define ONIG_REGION_NOTPOS            -1

typedef struct re_registers   OnigRegion;

typedef struct {
  OnigEncoding enc;
  OnigUChar* par;
  OnigUChar* par_end;
} OnigErrorInfo;

typedef struct {
  int lower;
  int upper;
} OnigRepeatRange;

typedef void (*OnigWarnFunc)(const char* s);
extern void onig_null_warn(const char* s);
#define ONIG_NULL_WARN       onig_null_warn

#define ONIG_CHAR_TABLE_SIZE   256

/* regex_t state */
#define ONIG_STATE_NORMAL              0
#define ONIG_STATE_SEARCHING           1
#define ONIG_STATE_COMPILING          -1
#define ONIG_STATE_MODIFY             -2

#define ONIG_STATE(reg) \
  ((reg)->state > 0 ? ONIG_STATE_SEARCHING : (reg)->state)

typedef struct re_pattern_buffer {
  /* common members of BBuf(bytes-buffer) */
  unsigned char* p;         /* compiled pattern */
  unsigned int used;        /* used space for p */
  unsigned int alloc;       /* allocated space for p */

  int state;                     /* normal, searching, compiling */
  int num_mem;                   /* used memory(...) num counted from 1 */
  int num_repeat;                /* OP_REPEAT/OP_REPEAT_NG id-counter */
  int num_null_check;            /* OP_NULL_CHECK_START/END id counter */
  int num_comb_exp_check;        /* combination explosion check */
  int num_call;                  /* number of subexp call */
  unsigned int capture_history;  /* (?@...) flag (1-31) */
  unsigned int bt_mem_start;     /* need backtrack flag */
  unsigned int bt_mem_end;       /* need backtrack flag */
  int stack_pop_level;
  int repeat_range_alloc;
  OnigRepeatRange* repeat_range;

  OnigEncoding      enc;
  OnigOptionType    options;
  const OnigSyntaxType* syntax;
  OnigCaseFoldType  case_fold_flag;
  void*             name_table;

  /* optimization info (string search, char-map and anchors) */
  int            optimize;          /* optimize flag */
  int            threshold_len;     /* search str-length for apply optimize */
  int            anchor;            /* BEGIN_BUF, BEGIN_POS, (SEMI_)END_BUF */
  OnigDistance   anchor_dmin;       /* (SEMI_)END_BUF anchor distance */
  OnigDistance   anchor_dmax;       /* (SEMI_)END_BUF anchor distance */
  int            sub_anchor;        /* start-anchor for exact or map */
  unsigned char *exact;
  unsigned char *exact_end;
  unsigned char  map[ONIG_CHAR_TABLE_SIZE]; /* used as BM skip or char-map */
  int           *int_map;                   /* BM skip for exact_len > 255 */
  int           *int_map_backward;          /* BM skip for backward search */
  OnigDistance   dmin;                      /* min-distance of exact or map */
  OnigDistance   dmax;                      /* max-distance of exact or map */

  /* regex_t link chain */
  struct re_pattern_buffer* chain;  /* escape compile-conflict */
} OnigRegexType;

typedef OnigRegexType*  OnigRegex;

#ifndef ONIG_ESCAPE_REGEX_T_COLLISION
  typedef OnigRegexType  regex_t;
#endif


typedef struct {
  int             num_of_elements;
  OnigEncoding    pattern_enc;
  OnigEncoding    target_enc;
  OnigSyntaxType* syntax;
  OnigOptionType  option;
  OnigCaseFoldType   case_fold_flag;
} OnigCompileInfo;

/* Oniguruma Native API */
ONIG_EXTERN
int onig_init(void);
ONIG_EXTERN
int onig_error_code_to_str(OnigUChar* s, int err_code, ...);
ONIG_EXTERN
void onig_set_warn_func(OnigWarnFunc f);
ONIG_EXTERN
void onig_set_verb_warn_func(OnigWarnFunc f);
ONIG_EXTERN
int onig_new(OnigRegex*, const OnigUChar* pattern, const OnigUChar* pattern_end, OnigOptionType option, OnigEncoding enc, const OnigSyntaxType* syntax, OnigErrorInfo* einfo);
ONIG_EXTERN
int  onig_reg_init(regex_t* reg, OnigOptionType option, OnigCaseFoldType case_fold_flag, OnigEncoding enc, const OnigSyntaxType* syntax);
ONIG_EXTERN
int onig_new_without_alloc(OnigRegex, const OnigUChar* pattern, const OnigUChar* pattern_end, OnigOptionType option, OnigEncoding enc, OnigSyntaxType* syntax, OnigErrorInfo* einfo);
ONIG_EXTERN
int onig_new_deluxe(OnigRegex* reg, const OnigUChar* pattern, const OnigUChar* pattern_end, OnigCompileInfo* ci, OnigErrorInfo* einfo);
ONIG_EXTERN
void onig_free(OnigRegex);
ONIG_EXTERN
void onig_free_body(OnigRegex);
ONIG_EXTERN
int onig_recompile(OnigRegex, const OnigUChar* pattern, const OnigUChar* pattern_end, OnigOptionType option, OnigEncoding enc, OnigSyntaxType* syntax, OnigErrorInfo* einfo);
ONIG_EXTERN
int onig_recompile_deluxe(OnigRegex reg, const OnigUChar* pattern, const OnigUChar* pattern_end, OnigCompileInfo* ci, OnigErrorInfo* einfo);
ONIG_EXTERN
long onig_search(OnigRegex, const OnigUChar* str, const OnigUChar* end, const OnigUChar* start, const OnigUChar* range, OnigRegion* region, OnigOptionType option);
ONIG_EXTERN
long onig_match(OnigRegex, const OnigUChar* str, const OnigUChar* end, const OnigUChar* at, OnigRegion* region, OnigOptionType option);
ONIG_EXTERN
OnigRegion* onig_region_new(void);
ONIG_EXTERN
void onig_region_init(OnigRegion* region);
ONIG_EXTERN
void onig_region_free(OnigRegion* region, int free_self);
ONIG_EXTERN
void onig_region_copy(OnigRegion* to, OnigRegion* from);
ONIG_EXTERN
void onig_region_clear(OnigRegion* region);
ONIG_EXTERN
int onig_region_resize(OnigRegion* region, int n);
ONIG_EXTERN
int onig_region_set(OnigRegion* region, int at, int beg, int end);
ONIG_EXTERN
int onig_name_to_group_numbers(OnigRegex reg, const OnigUChar* name, const OnigUChar* name_end, int** nums);
ONIG_EXTERN
int onig_name_to_backref_number(OnigRegex reg, const OnigUChar* name, const OnigUChar* name_end, OnigRegion *region);
ONIG_EXTERN
int onig_foreach_name(OnigRegex reg, int (*func)(const OnigUChar*, const OnigUChar*,int,int*,OnigRegex,void*), void* arg);
ONIG_EXTERN
int onig_number_of_names(OnigRegex reg);
ONIG_EXTERN
int onig_number_of_captures(OnigRegex reg);
ONIG_EXTERN
int onig_number_of_capture_histories(OnigRegex reg);
ONIG_EXTERN
OnigCaptureTreeNode* onig_get_capture_tree(OnigRegion* region);
ONIG_EXTERN
int onig_capture_tree_traverse(OnigRegion* region, int at, int(*callback_func)(int,int,int,int,int,void*), void* arg);
ONIG_EXTERN
int onig_noname_group_capture_is_active(OnigRegex reg);
ONIG_EXTERN
OnigEncoding onig_get_encoding(OnigRegex reg);
ONIG_EXTERN
OnigOptionType onig_get_options(OnigRegex reg);
ONIG_EXTERN
OnigCaseFoldType onig_get_case_fold_flag(OnigRegex reg);
ONIG_EXTERN
const OnigSyntaxType* onig_get_syntax(OnigRegex reg);
ONIG_EXTERN
int onig_set_default_syntax(const OnigSyntaxType* syntax);
ONIG_EXTERN
void onig_copy_syntax(OnigSyntaxType* to, const OnigSyntaxType* from);
ONIG_EXTERN
unsigned int onig_get_syntax_op(OnigSyntaxType* syntax);
ONIG_EXTERN
unsigned int onig_get_syntax_op2(OnigSyntaxType* syntax);
ONIG_EXTERN
unsigned int onig_get_syntax_behavior(OnigSyntaxType* syntax);
ONIG_EXTERN
OnigOptionType onig_get_syntax_options(OnigSyntaxType* syntax);
ONIG_EXTERN
void onig_set_syntax_op(OnigSyntaxType* syntax, unsigned int op);
ONIG_EXTERN
void onig_set_syntax_op2(OnigSyntaxType* syntax, unsigned int op2);
ONIG_EXTERN
void onig_set_syntax_behavior(OnigSyntaxType* syntax, unsigned int behavior);
ONIG_EXTERN
void onig_set_syntax_options(OnigSyntaxType* syntax, OnigOptionType options);
ONIG_EXTERN
int onig_set_meta_char(OnigSyntaxType* syntax, unsigned int what, OnigCodePoint code);
ONIG_EXTERN
void onig_copy_encoding(OnigEncoding to, OnigEncoding from);
ONIG_EXTERN
OnigCaseFoldType onig_get_default_case_fold_flag(void);
ONIG_EXTERN
int onig_set_default_case_fold_flag(OnigCaseFoldType case_fold_flag);
ONIG_EXTERN
unsigned int onig_get_match_stack_limit_size(void);
ONIG_EXTERN
int onig_set_match_stack_limit_size(unsigned int size);
ONIG_EXTERN
int onig_end(void);
ONIG_EXTERN
const char* onig_version(void);
ONIG_EXTERN
const char* onig_copyright(void);

#ifdef __cplusplus
}
#endif

#endif /* ONIGURUMA_H */
