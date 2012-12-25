/*
** encoding.h - Encoding class
**
** See Copyright Notice in mruby.h
*/

#ifndef RUBY_ENCODING_H
#define RUBY_ENCODING_H 1

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdarg.h>
#include "oniguruma.h"
#include "mruby/data.h"

#define FL_USHIFT    12

#define FL_USER0     (((int)1)<<(FL_USHIFT+0))
#define FL_USER1     (((int)1)<<(FL_USHIFT+1))
#define FL_USER2     (((int)1)<<(FL_USHIFT+2))
#define FL_USER3     (((int)1)<<(FL_USHIFT+3))
#define FL_USER4     (((int)1)<<(FL_USHIFT+4))
#define FL_USER5     (((int)1)<<(FL_USHIFT+5))
#define FL_USER6     (((int)1)<<(FL_USHIFT+6))
#define FL_USER7     (((int)1)<<(FL_USHIFT+7))
#define FL_USER8     (((int)1)<<(FL_USHIFT+8))
#define FL_USER9     (((int)1)<<(FL_USHIFT+9))

#define ENCODING_INLINE_MAX 1023
/* 1023 = 0x03FF */
/*#define ENCODING_SHIFT (FL_USHIFT+10)*/
#define ENCODING_SHIFT (10)
#define ENCODING_MASK (((unsigned int)ENCODING_INLINE_MAX)<<ENCODING_SHIFT)

#define ENCODING_SET_INLINED(obj,i) do {\
    mrb_obj_ptr(obj)->flags &= ~ENCODING_MASK;\
    mrb_obj_ptr(obj)->flags |= (unsigned int)(i) << ENCODING_SHIFT;\
} while (0)
#define ENCODING_SET(mrb, obj,i) do {\
    mrb_value mrb_encoding_set_obj = (obj); \
    int encoding_set_enc_index = (i); \
    if (encoding_set_enc_index < ENCODING_INLINE_MAX) \
        ENCODING_SET_INLINED(mrb_encoding_set_obj, encoding_set_enc_index); \
    else \
        mrb_enc_set_index(mrb, mrb_encoding_set_obj, encoding_set_enc_index); \
} while (0)

#define ENCODING_GET_INLINED(obj) (unsigned int)((RSTRING(obj)->flags & ENCODING_MASK)>>ENCODING_SHIFT)
#define ENCODING_GET(mrb, obj) \
    (ENCODING_GET_INLINED(obj) != ENCODING_INLINE_MAX ? \
     ENCODING_GET_INLINED(obj) : \
     mrb_enc_get_index(mrb, obj))

#define ENCODING_IS_ASCII8BIT(obj) (ENCODING_GET_INLINED(obj) == 0)

#define ENCODING_MAXNAMELEN 42

#define ENC_CODERANGE_MASK      ((int)(FL_USER8|FL_USER9))
#define ENC_CODERANGE_UNKNOWN   0
#define ENC_CODERANGE_7BIT      ((int)FL_USER8)
#define ENC_CODERANGE_VALID     ((int)FL_USER9)
#define ENC_CODERANGE_BROKEN    ((int)(FL_USER8|FL_USER9))
#define ENC_CODERANGE(obj) ((int)(RSTRING(obj)->flags & ENC_CODERANGE_MASK))
#define ENC_CODERANGE_ASCIIONLY(obj) (ENC_CODERANGE(obj) == ENC_CODERANGE_7BIT)
#ifdef INCLUDE_ENCODING
#define ENC_CODERANGE_SET(obj,cr) (RSTRING(obj)->flags = \
                                   (RSTRING(obj)->flags & ~ENC_CODERANGE_MASK) | (cr))
#else
#define ENC_CODERANGE_SET(obj,cr)
#endif //INCLUDE_ENCODING
#define ENC_CODERANGE_CLEAR(obj) ENC_CODERANGE_SET(obj,0)

/* assumed ASCII compatibility */
#define ENC_CODERANGE_AND(a, b) \
    (a == ENC_CODERANGE_7BIT ? b : \
     a == ENC_CODERANGE_VALID ? (b == ENC_CODERANGE_7BIT ? ENC_CODERANGE_VALID : b) : \
     ENC_CODERANGE_UNKNOWN)

#define ENCODING_CODERANGE_SET(mrb, obj, encindex, cr) \
    do { \
        mrb_value mrb_encoding_coderange_obj = (obj); \
        ENCODING_SET(mrb, mrb_encoding_coderange_obj, (encindex)); \
        ENC_CODERANGE_SET(mrb_encoding_coderange_obj, (cr)); \
    } while (0)

typedef OnigEncodingType mrb_encoding;

/* mrb_encoding * -> name */
#define mrb_enc_name(enc) (enc)->name
int mrb_enc_get_index(mrb_state *mrb, mrb_value obj);

int mrb_enc_replicate(mrb_state *, const char *, mrb_encoding *);
int mrb_define_dummy_encoding(mrb_state *mrb, const char *);
#define mrb_enc_to_index(enc) ((enc) ? ENC_TO_ENCINDEX(enc) : 0)
void mrb_enc_set_index(mrb_state *mrb, mrb_value obj, int encindex);
int mrb_enc_find_index(mrb_state *mrb, const char *name);
int mrb_to_encoding_index(mrb_state *mrb, mrb_value);
mrb_encoding* mrb_to_encoding(mrb_state *mrb, mrb_value);
mrb_encoding* mrb_enc_get(mrb_state *, mrb_value);
mrb_encoding* mrb_enc_compatible(mrb_state *, mrb_value, mrb_value);
mrb_encoding* mrb_enc_check(mrb_state *, mrb_value, mrb_value);
mrb_value mrb_enc_associate_index(mrb_state *mrb, mrb_value, int);
#ifdef INCLUDE_ENCODING
mrb_value mrb_enc_associate(mrb_state *mrb, mrb_value, mrb_encoding*);
#else
#define mrb_enc_associate(mrb,value,enc)
#endif //INCLUDE_ENCODING
void mrb_enc_copy(mrb_state *mrb, mrb_value dst, mrb_value src);

mrb_value mrb_enc_reg_new(const char*, long, mrb_encoding*, int);
//PRINTF_ARGS(mrb_value rb_enc_sprintf(mrb_encoding *, const char*, ...), 2, 3);
mrb_value mrb_enc_vsprintf(mrb_encoding *, const char*, va_list);
long mrb_enc_strlen(const char*, const char*, mrb_encoding*);
char* mrb_enc_nth(mrb_state *, const char*, const char*, long, mrb_encoding*);
mrb_value mrb_obj_encoding(mrb_state *, mrb_value);
mrb_value mrb_enc_str_buf_cat(mrb_state *mrb, mrb_value str, const char *ptr, long len, mrb_encoding *enc);
mrb_value rb_enc_uint_chr(mrb_state *mrb, unsigned int code, mrb_encoding *enc);

mrb_value mrb_external_str_new_with_enc(mrb_state *mrb, const char *ptr, long len, mrb_encoding *);
mrb_value mrb_str_export_to_enc(mrb_value, mrb_encoding *);

/* index -> mrb_encoding */
mrb_encoding* mrb_enc_from_index(mrb_state *mrb, int idx);

/* name -> mrb_encoding */
mrb_encoding * mrb_enc_find(mrb_state *mrb, const char *name);

/* mrb_encoding * -> name */
#define mrb_enc_name(enc) (enc)->name

/* mrb_encoding * -> minlen/maxlen */
#define mrb_enc_mbminlen(enc) (enc)->min_enc_len
#define mrb_enc_mbmaxlen(enc) (enc)->max_enc_len

/* -> mbclen (no error notification: 0 < ret <= e-p, no exception) */
int mrb_enc_mbclen(const char *p, const char *e, mrb_encoding *enc);

/* -> mbclen (only for valid encoding) */
int mrb_enc_fast_mbclen(const char *p, const char *e, mrb_encoding *enc);

/* -> chlen, invalid or needmore */
int mrb_enc_precise_mbclen(const char *p, const char *e, mrb_encoding *enc);
#define MBCLEN_CHARFOUND_P(ret)     ONIGENC_MBCLEN_CHARFOUND_P(ret)
#define MBCLEN_CHARFOUND_LEN(ret)     ONIGENC_MBCLEN_CHARFOUND_LEN(ret)
#define MBCLEN_INVALID_P(ret)       ONIGENC_MBCLEN_INVALID_P(ret)
#define MBCLEN_NEEDMORE_P(ret)      ONIGENC_MBCLEN_NEEDMORE_P(ret)
#define MBCLEN_NEEDMORE_LEN(ret)      ONIGENC_MBCLEN_NEEDMORE_LEN(ret)

/* -> 0x00..0x7f, -1 */
int mrb_enc_ascget(mrb_state *mrb, const char *p, const char *e, int *len, mrb_encoding *enc);


/* -> code (and len) or raise exception */
unsigned int mrb_enc_codepoint_len(mrb_state *mrb, const char *p, const char *e, int *len, mrb_encoding *enc);

/* prototype for obsolete function */
unsigned int mrb_enc_codepoint(mrb_state *mrb, const char *p, const char *e, mrb_encoding *enc);
/* overriding macro */
#define mrb_enc_codepoint(mrb,p,e,enc) mrb_enc_codepoint_len((mrb),(p),(e),0,(enc))
#define mrb_enc_mbc_to_codepoint(p, e, enc) ONIGENC_MBC_TO_CODE(enc,(UChar*)(p),(UChar*)(e))

/* -> codelen>0 or raise exception */
#ifdef INCLUDE_ENCODING
int mrb_enc_codelen(mrb_state *mrb, int code, mrb_encoding *enc);
#else
#define mrb_enc_codelen(mrb,code,enc) 1
#endif //INCLUDE_ENCODING

/* code,ptr,encoding -> write buf */
#define mrb_enc_mbcput(c,buf,enc) ((*(buf) = (char)(c)),1)

/* start, ptr, end, encoding -> prev_char */
#define mrb_enc_prev_char(s,p,e,enc) (char *)onigenc_get_prev_char_head(enc,(UChar*)(s),(UChar*)(p),(UChar*)(e))
/* start, ptr, end, encoding -> next_char */
#define mrb_enc_left_char_head(s,p,e,enc) (char *)onigenc_get_left_adjust_char_head(enc,(UChar*)(s),(UChar*)(p),(UChar*)(e))
#define mrb_enc_right_char_head(s,p,e,enc) (char *)onigenc_get_right_adjust_char_head(enc,(UChar*)(s),(UChar*)(p),(UChar*)(e))

/* ptr, ptr, encoding -> newline_or_not */
#define mrb_enc_is_newline(p,end,enc)  ONIGENC_IS_MBC_NEWLINE(enc,(UChar*)(p),(UChar*)(end))

#define mrb_enc_isctype(c,t,enc) ONIGENC_IS_CODE_CTYPE(enc,c,t)
#define mrb_enc_isascii(c,enc) ONIGENC_IS_CODE_ASCII(c)
#define mrb_enc_isalpha(c,enc) ONIGENC_IS_CODE_ALPHA(enc,c)
#define mrb_enc_islower(c,enc) ONIGENC_IS_CODE_LOWER(enc,c)
#define mrb_enc_isupper(c,enc) ONIGENC_IS_CODE_UPPER(enc,c)
#define mrb_enc_ispunct(c,enc) ONIGENC_IS_CODE_PUNCT(enc,c)
#define mrb_enc_isalnum(c,enc) ONIGENC_IS_CODE_ALNUM(enc,c)
#define mrb_enc_isprint(c,enc) ONIGENC_IS_CODE_PRINT(enc,c)
#define mrb_enc_isspace(c,enc) ONIGENC_IS_CODE_SPACE(enc,c)
#define mrb_enc_isdigit(c,enc) ONIGENC_IS_CODE_DIGIT(enc,c)

#define mrb_enc_asciicompat(mrb, enc) (mrb_enc_mbminlen(enc)==1 && !mrb_enc_dummy_p(enc))

int mrb_enc_casefold(char *to, const char *p, const char *e, mrb_encoding *enc);
int mrb_enc_toupper(int c, mrb_encoding *enc);
int mrb_enc_tolower(int c, mrb_encoding *enc);
//ID mrb_intern3(const char*, long, mrb_encoding*);
//ID mrb_interned_id_p(const char *, long, mrb_encoding *);
int mrb_enc_symname_p(const char*, mrb_encoding*);
int mrb_enc_symname2_p(const char*, long, mrb_encoding*);
int mrb_enc_str_coderange(mrb_state *mrb, mrb_value);
long mrb_str_coderange_scan_restartable(const char*, const char*, mrb_encoding*, int*);
int mrb_enc_str_asciionly_p(mrb_state *mrb, mrb_value);
#define mrb_enc_str_asciicompat_p(mrb, str) mrb_enc_asciicompat(mrb, mrb_enc_get(mrb, str))
mrb_value mrb_enc_from_encoding(mrb_state *mrb, mrb_encoding *enc);
int mrb_enc_unicode_p(mrb_encoding *enc);
mrb_encoding *mrb_ascii8bit_encoding(mrb_state *mrb);
mrb_encoding *mrb_utf8_encoding(mrb_state *mrb);
mrb_encoding *mrb_usascii_encoding(mrb_state *mrb);
mrb_encoding *mrb_locale_encoding(mrb_state *mrb);
mrb_encoding *mrb_filesystem_encoding(mrb_state *mrb);
mrb_encoding *mrb_default_external_encoding(mrb_state *mrb);
mrb_encoding *mrb_default_internal_encoding(mrb_state *mrb);
int mrb_ascii8bit_encindex(void);
int mrb_utf8_encindex(void);
int mrb_usascii_encindex(void);
int mrb_locale_encindex(mrb_state *mrb);
int mrb_filesystem_encindex(void);
mrb_value mrb_enc_default_external(mrb_state *mrb);
mrb_value mrb_enc_default_internal(mrb_state *mrb);
void mrb_enc_set_default_external(mrb_state *mrb, mrb_value encoding);
void mrb_enc_set_default_internal(mrb_state *mrb, mrb_value encoding);
mrb_value mrb_locale_charmap(mrb_state *mrb, mrb_value klass);
mrb_value mrb_usascii_str_new_cstr(mrb_state *mrb, const char *ptr);
int mrb_str_buf_cat_escaped_char(mrb_state *mrb, mrb_value result, unsigned int c, int unicode_p);

#define ENC_DUMMY_FLAG (1<<24)
#define ENC_INDEX_MASK (~(~0U<<24))

#define ENC_TO_ENCINDEX(enc) (int)((enc)->ruby_encoding_index & ENC_INDEX_MASK)

#define ENC_DUMMY_P(enc) ((enc)->ruby_encoding_index & ENC_DUMMY_FLAG)
#define ENC_SET_DUMMY(enc) ((enc)->ruby_encoding_index |= ENC_DUMMY_FLAG)

static inline int
mrb_enc_dummy_p(mrb_encoding *enc)
{
    return ENC_DUMMY_P(enc) != 0;
}

/* econv stuff */

typedef enum {
    econv_invalid_byte_sequence,
    econv_undefined_conversion,
    econv_destination_buffer_full,
    econv_source_buffer_empty,
    econv_finished,
    econv_after_output,
    econv_incomplete_input
} mrb_econv_result_t;

typedef struct mrb_econv_t mrb_econv_t;

mrb_value mrb_str_encode(mrb_state *mrb, mrb_value str, mrb_value to, int ecflags, mrb_value ecopts);
int mrb_econv_has_convpath_p(mrb_state *mrb, const char* from_encoding, const char* to_encoding);

int mrb_econv_prepare_opts(mrb_state *mrb, mrb_value opthash, mrb_value *ecopts);

mrb_econv_t *mrb_econv_open(mrb_state *mrb, const char *source_encoding, const char *destination_encoding, int ecflags);
mrb_econv_t *mrb_econv_open_opts(mrb_state *mrb, const char *source_encoding, const char *destination_encoding, int ecflags, mrb_value ecopts);

mrb_econv_result_t mrb_econv_convert(mrb_state *mrb, mrb_econv_t *ec,
    const unsigned char **source_buffer_ptr, const unsigned char *source_buffer_end,
    unsigned char **destination_buffer_ptr, unsigned char *destination_buffer_end,
    int flags);
void mrb_econv_close(mrb_econv_t *ec);

/* result: 0:success -1:failure */
int mrb_econv_set_replacement(mrb_state *mrb, mrb_econv_t *ec, const unsigned char *str, size_t len, const char *encname);

/* result: 0:success -1:failure */
int mrb_econv_decorate_at_first(mrb_state *mrb, mrb_econv_t *ec, const char *decorator_name);
int mrb_econv_decorate_at_last(mrb_state *mrb, mrb_econv_t *ec, const char *decorator_name);

mrb_value mrb_econv_open_exc(mrb_state *mrb, const char *senc, const char *denc, int ecflags);

/* result: 0:success -1:failure */
int mrb_econv_insert_output(mrb_state *mrb, mrb_econv_t *ec,
    const unsigned char *str, size_t len, const char *str_encoding);

/* encoding that mrb_econv_insert_output doesn't need conversion */
const char *mrb_econv_encoding_to_insert_output(mrb_econv_t *ec);

/* raise an error if the last mrb_econv_convert is error */
void mrb_econv_check_error(mrb_state *mrb, mrb_econv_t *ec);

/* returns an exception object or nil */
mrb_value mrb_econv_make_exception(mrb_state *mrb, mrb_econv_t *ec);

int mrb_econv_putbackable(mrb_econv_t *ec);
void mrb_econv_putback(mrb_econv_t *ec, unsigned char *p, int n);

/* returns the corresponding ASCII compatible encoding for encname,
 * or NULL if encname is not ASCII incompatible encoding. */
const char *mrb_econv_asciicompat_encoding(const char *encname);

mrb_value mrb_econv_str_convert(mrb_state *mrb, mrb_econv_t *ec, mrb_value src, int flags);
mrb_value mrb_econv_substr_convert(mrb_state *mrb, mrb_econv_t *ec, mrb_value src, long byteoff, long bytesize, int flags);
mrb_value mrb_econv_str_append(mrb_state *mrb, mrb_econv_t *ec, mrb_value src, mrb_value dst, int flags);
mrb_value mrb_econv_substr_append(mrb_state *mrb, mrb_econv_t *ec, mrb_value src, long byteoff, long bytesize, mrb_value dst, int flags);

void mrb_econv_binmode(mrb_econv_t *ec);

/* flags for mrb_econv_open */

#define ECONV_ERROR_HANDLER_MASK                0x000000ff

#define ECONV_INVALID_MASK                      0x0000000f
#define ECONV_INVALID_REPLACE                   0x00000002

#define ECONV_UNDEF_MASK                        0x000000f0
#define ECONV_UNDEF_REPLACE                     0x00000020
#define ECONV_UNDEF_HEX_CHARREF                 0x00000030

#define ECONV_DECORATOR_MASK                    0x0000ff00

#define ECONV_UNIVERSAL_NEWLINE_DECORATOR       0x00000100
#define ECONV_CRLF_NEWLINE_DECORATOR            0x00001000
#define ECONV_CR_NEWLINE_DECORATOR              0x00002000
#define ECONV_XML_TEXT_DECORATOR                0x00004000
#define ECONV_XML_ATTR_CONTENT_DECORATOR        0x00008000

#define ECONV_STATEFUL_DECORATOR_MASK           0x00f00000
#define ECONV_XML_ATTR_QUOTE_DECORATOR          0x00100000

/* end of flags for mrb_econv_open */

/* flags for mrb_econv_convert */
#define ECONV_PARTIAL_INPUT                     0x00010000
#define ECONV_AFTER_OUTPUT                      0x00020000
/* end of flags for mrb_econv_convert */

int mrb_isspace(int c);

#define ENCODE_CLASS (mrb_class_obj_get(mrb, "Encoding"))
#define CONVERTER_CLASS (mrb_class_obj_get(mrb, "Converter"))

#if defined(__cplusplus)
}  /* extern "C" { */
#endif

#endif /* RUBY_ENCODING_H */
