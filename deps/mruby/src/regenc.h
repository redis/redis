#ifndef ONIGURUMA_REGENC_H
#define ONIGURUMA_REGENC_H
/**********************************************************************
  regenc.h -  Oniguruma (regular expression library)
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

#include <stdio.h>
#include <stdarg.h>
#define RUBY

#ifndef mrb_compile_warn
#define mrb_compile_warn(a,b,c,d) printf(c,d)
#endif

#ifndef REGINT_H
#ifdef ONIG_ESCAPE_UCHAR_COLLISION
#undef ONIG_ESCAPE_UCHAR_COLLISION
#endif
#endif
#include "oniguruma.h"

typedef struct {
  OnigCodePoint from;
  OnigCodePoint to;
} OnigPairCaseFoldCodes;


#ifndef ARG_UNUSED
#if defined(__GNUC__)
#  define ARG_UNUSED  __attribute__ ((unused))
#else
#  define ARG_UNUSED
#endif
#endif

#define ONIG_IS_NULL(p)                    (((void*)(p)) == (void*)0)
#define ONIG_IS_NOT_NULL(p)                (((void*)(p)) != (void*)0)
#define ONIG_CHECK_NULL_RETURN(p)          if (ONIG_IS_NULL(p)) return NULL
#define ONIG_CHECK_NULL_RETURN_VAL(p,val)  if (ONIG_IS_NULL(p)) return (val)

#define enclen(enc,p,e) ((enc->max_enc_len == enc->min_enc_len) ? enc->min_enc_len : ONIGENC_MBC_ENC_LEN(enc,p,e))

/* character types bit flag */
#define BIT_CTYPE_NEWLINE  (1<< ONIGENC_CTYPE_NEWLINE)
#define BIT_CTYPE_ALPHA    (1<< ONIGENC_CTYPE_ALPHA)
#define BIT_CTYPE_BLANK    (1<< ONIGENC_CTYPE_BLANK)
#define BIT_CTYPE_CNTRL    (1<< ONIGENC_CTYPE_CNTRL)
#define BIT_CTYPE_DIGIT    (1<< ONIGENC_CTYPE_DIGIT)
#define BIT_CTYPE_GRAPH    (1<< ONIGENC_CTYPE_GRAPH)
#define BIT_CTYPE_LOWER    (1<< ONIGENC_CTYPE_LOWER)
#define BIT_CTYPE_PRINT    (1<< ONIGENC_CTYPE_PRINT)
#define BIT_CTYPE_PUNCT    (1<< ONIGENC_CTYPE_PUNCT)
#define BIT_CTYPE_SPACE    (1<< ONIGENC_CTYPE_SPACE)
#define BIT_CTYPE_UPPER    (1<< ONIGENC_CTYPE_UPPER)
#define BIT_CTYPE_XDIGIT   (1<< ONIGENC_CTYPE_XDIGIT)
#define BIT_CTYPE_WORD     (1<< ONIGENC_CTYPE_WORD)
#define BIT_CTYPE_ALNUM    (1<< ONIGENC_CTYPE_ALNUM)
#define BIT_CTYPE_ASCII    (1<< ONIGENC_CTYPE_ASCII)

#define CTYPE_TO_BIT(ctype)  (1<<(ctype))
#define CTYPE_IS_WORD_GRAPH_PRINT(ctype) \
  ((ctype) == ONIGENC_CTYPE_WORD || (ctype) == ONIGENC_CTYPE_GRAPH ||\
   (ctype) == ONIGENC_CTYPE_PRINT)


typedef struct {
  const UChar *name;
  int       ctype;
  short int len;
} PosixBracketEntryType;

#define PosixBracketEntryInit(name, ctype) {(const UChar *)name, ctype, (short int)(sizeof(name) - 1)}

/* #define USE_CRNL_AS_LINE_TERMINATOR */
#define USE_UNICODE_PROPERTIES
/* #define USE_UNICODE_CASE_FOLD_TURKISH_AZERI */
/* #define USE_UNICODE_ALL_LINE_TERMINATORS */  /* see Unicode.org UTF#18 */


#define ONIG_ENCODING_INIT_DEFAULT           ONIG_ENCODING_ASCII

/* for encoding system implementation (internal) */
ONIG_EXTERN int onigenc_ascii_apply_all_case_fold(OnigCaseFoldType flag, OnigApplyAllCaseFoldFunc f, void* arg, OnigEncoding enc);
ONIG_EXTERN int onigenc_ascii_get_case_fold_codes_by_str(OnigCaseFoldType flag, const OnigUChar* p, const OnigUChar* end, OnigCaseFoldCodeItem items[], OnigEncoding enc);
ONIG_EXTERN int onigenc_apply_all_case_fold_with_map(int map_size, const OnigPairCaseFoldCodes map[], int ess_tsett_flag, OnigCaseFoldType flag, OnigApplyAllCaseFoldFunc f, void* arg);
ONIG_EXTERN int onigenc_get_case_fold_codes_by_str_with_map(int map_size, const OnigPairCaseFoldCodes map[], int ess_tsett_flag, OnigCaseFoldType flag, const OnigUChar* p, const OnigUChar* end, OnigCaseFoldCodeItem items[]);
ONIG_EXTERN int onigenc_not_support_get_ctype_code_range(OnigCtype ctype, OnigCodePoint* sb_out, const OnigCodePoint* ranges[], OnigEncoding enc);
ONIG_EXTERN int onigenc_is_mbc_newline_0x0a(const UChar* p, const UChar* end, OnigEncoding enc);

/* methods for single byte encoding */
ONIG_EXTERN int onigenc_ascii_mbc_case_fold(OnigCaseFoldType flag, const UChar** p, const UChar* end, UChar* lower, OnigEncoding enc);
ONIG_EXTERN int onigenc_single_byte_mbc_enc_len(const UChar* p, const UChar* e, OnigEncoding enc);
ONIG_EXTERN OnigCodePoint onigenc_single_byte_mbc_to_code(const UChar* p, const UChar* end, OnigEncoding enc);
ONIG_EXTERN int onigenc_single_byte_code_to_mbclen(OnigCodePoint code, OnigEncoding enc);
ONIG_EXTERN int onigenc_single_byte_code_to_mbc(OnigCodePoint code, UChar *buf, OnigEncoding enc);
ONIG_EXTERN UChar* onigenc_single_byte_left_adjust_char_head(const UChar* start, const UChar* s, const OnigUChar* end, OnigEncoding enc);
ONIG_EXTERN int onigenc_always_true_is_allowed_reverse_match(const UChar* s, const UChar* end, OnigEncoding enc);
ONIG_EXTERN int onigenc_always_false_is_allowed_reverse_match(const UChar* s, const UChar* end, OnigEncoding enc);
ONIG_EXTERN int onigenc_ascii_is_code_ctype(OnigCodePoint code, unsigned int ctype, OnigEncoding enc);

/* methods for multi byte encoding */
ONIG_EXTERN OnigCodePoint onigenc_mbn_mbc_to_code(OnigEncoding enc, const UChar* p, const UChar* end);
ONIG_EXTERN int onigenc_mbn_mbc_case_fold(OnigEncoding enc, OnigCaseFoldType flag, const UChar** p, const UChar* end, UChar* lower);
ONIG_EXTERN int onigenc_mb2_code_to_mbclen(OnigCodePoint code, OnigEncoding enc);
ONIG_EXTERN int onigenc_mb2_code_to_mbc(OnigEncoding enc, OnigCodePoint code, UChar *buf);
ONIG_EXTERN int onigenc_minimum_property_name_to_ctype(OnigEncoding enc, UChar* p, UChar* end);
ONIG_EXTERN int onigenc_unicode_property_name_to_ctype(OnigEncoding enc, UChar* p, UChar* end);
ONIG_EXTERN int onigenc_mb2_is_code_ctype(OnigEncoding enc, OnigCodePoint code, unsigned int ctype);
ONIG_EXTERN int onigenc_mb4_code_to_mbclen(OnigCodePoint code, OnigEncoding enc);
ONIG_EXTERN int onigenc_mb4_code_to_mbc(OnigEncoding enc, OnigCodePoint code, UChar *buf);
ONIG_EXTERN int onigenc_mb4_is_code_ctype(OnigEncoding enc, OnigCodePoint code, unsigned int ctype);


/* in enc/unicode.c */
ONIG_EXTERN int onigenc_unicode_is_code_ctype(OnigCodePoint code, unsigned int ctype, OnigEncoding enc);
ONIG_EXTERN int onigenc_utf16_32_get_ctype_code_range(OnigCtype ctype, OnigCodePoint *sb_out, const OnigCodePoint* ranges[], OnigEncoding enc);
ONIG_EXTERN int onigenc_unicode_ctype_code_range(int ctype, const OnigCodePoint* ranges[]);
ONIG_EXTERN int onigenc_unicode_get_case_fold_codes_by_str(OnigEncoding enc, OnigCaseFoldType flag, const OnigUChar* p, const OnigUChar* end, OnigCaseFoldCodeItem items[]);
ONIG_EXTERN int onigenc_unicode_mbc_case_fold(OnigEncoding enc, OnigCaseFoldType flag, const UChar** pp, const UChar* end, UChar* fold);
ONIG_EXTERN int onigenc_unicode_apply_all_case_fold(OnigCaseFoldType flag, OnigApplyAllCaseFoldFunc f, void* arg, OnigEncoding enc);


#define UTF16_IS_SURROGATE_FIRST(c)    (((c) & 0xfc) == 0xd8)
#define UTF16_IS_SURROGATE_SECOND(c)   (((c) & 0xfc) == 0xdc)

#define ONIGENC_ISO_8859_1_TO_LOWER_CASE(c) \
  OnigEncISO_8859_1_ToLowerCaseTable[c]
#define ONIGENC_ISO_8859_1_TO_UPPER_CASE(c) \
  OnigEncISO_8859_1_ToUpperCaseTable[c]

ONIG_EXTERN const UChar OnigEncISO_8859_1_ToLowerCaseTable[];
ONIG_EXTERN const UChar OnigEncISO_8859_1_ToUpperCaseTable[];

ONIG_EXTERN int
onigenc_with_ascii_strncmp(OnigEncoding enc, const UChar* p, const UChar* end, const UChar* sascii /* ascii */, int n);
ONIG_EXTERN UChar*
onigenc_step(OnigEncoding enc, const UChar* p, const UChar* end, int n);

/* defined in regexec.c, but used in enc/xxx.c */
extern int  onig_is_in_code_range (const UChar* p, OnigCodePoint code);

ONIG_EXTERN OnigEncoding  OnigEncDefaultCharEncoding;
ONIG_EXTERN const UChar  OnigEncAsciiToLowerCaseTable[];
ONIG_EXTERN const UChar  OnigEncAsciiToUpperCaseTable[];
ONIG_EXTERN const unsigned short OnigEncAsciiCtypeTable[];

#define ONIGENC_IS_ASCII_CODE(code)  ((code) < 0x80)
#define ONIGENC_ASCII_CODE_TO_LOWER_CASE(c) OnigEncAsciiToLowerCaseTable[c]
#define ONIGENC_ASCII_CODE_TO_UPPER_CASE(c) OnigEncAsciiToUpperCaseTable[c]
#define ONIGENC_IS_ASCII_CODE_CTYPE(code,ctype) \
  ((OnigEncAsciiCtypeTable[code] & CTYPE_TO_BIT(ctype)) != 0)
#define ONIGENC_IS_ASCII_CODE_CASE_AMBIG(code) \
 (ONIGENC_IS_ASCII_CODE_CTYPE(code, ONIGENC_CTYPE_UPPER) ||\
  ONIGENC_IS_ASCII_CODE_CTYPE(code, ONIGENC_CTYPE_LOWER))

#ifdef ONIG_ENC_REGISTER
extern int ONIG_ENC_REGISTER(const char *, OnigEncodingType*);
#define OnigEncodingName(n) encoding_##n
#define OnigEncodingDeclare(n) static OnigEncodingType OnigEncodingName(n)
#define OnigEncodingDefine(f,n)                      \
    OnigEncodingDeclare(n);                          \
    void Init_##f(void) {                            \
        ONIG_ENC_REGISTER(OnigEncodingName(n).name,  \
                          &OnigEncodingName(n));     \
    }                                                \
    OnigEncodingDeclare(n)
#else
#define OnigEncodingName(n) OnigEncoding##n
#define OnigEncodingDeclare(n) OnigEncodingType OnigEncodingName(n)
#define OnigEncodingDefine(f,n) OnigEncodingDeclare(n)
#endif

/* macros for define replica encoding and encoding alias */
#define ENC_REPLICATE(name, orig)
#define ENC_ALIAS(name, orig)
#define ENC_DUMMY(name)

#endif /* ONIGURUMA_REGENC_H */
