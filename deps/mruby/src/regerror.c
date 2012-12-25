/**********************************************************************
  regerror.c -  Oniguruma (regular expression library)
**********************************************************************/
/*-
 * Copyright (c) 2002-2007  K.Kosako  <sndgk393 AT ybb DOT ne DOT jp>
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

#include "mruby.h"
#ifdef ENABLE_REGEXP
#include <string.h>
#include "regint.h"
#include <stdio.h> /* for vsnprintf() */
#include <stdarg.h>

extern UChar*
onig_error_code_to_format(int code)
{
  const char *p;

  if (code >= 0) return (UChar* )0;

  switch (code) {
  case ONIG_MISMATCH:
    p = "mismatch"; break;
  case ONIG_NO_SUPPORT_CONFIG:
    p = "no support in this configuration"; break;
  case ONIGERR_MEMORY:
    p = "failed to allocate memory"; break;
  case ONIGERR_MATCH_STACK_LIMIT_OVER:
    p = "match-stack limit over"; break;
  case ONIGERR_TYPE_BUG:
    p = "undefined type (bug)"; break;
  case ONIGERR_PARSER_BUG:
    p = "internal parser error (bug)"; break;
  case ONIGERR_STACK_BUG:
    p = "stack error (bug)"; break;
  case ONIGERR_UNDEFINED_BYTECODE:
    p = "undefined bytecode (bug)"; break;
  case ONIGERR_UNEXPECTED_BYTECODE:
    p = "unexpected bytecode (bug)"; break;
  case ONIGERR_DEFAULT_ENCODING_IS_NOT_SETTED:
    p = "default multibyte-encoding is not setted"; break;
  case ONIGERR_SPECIFIED_ENCODING_CANT_CONVERT_TO_WIDE_CHAR:
    p = "can't convert to wide-char on specified multibyte-encoding"; break;
  case ONIGERR_INVALID_ARGUMENT:
    p = "invalid argument"; break;
  case ONIGERR_END_PATTERN_AT_LEFT_BRACE:
    p = "end pattern at left brace"; break;
  case ONIGERR_END_PATTERN_AT_LEFT_BRACKET:
    p = "end pattern at left bracket"; break;
  case ONIGERR_EMPTY_CHAR_CLASS:
    p = "empty char-class"; break;
  case ONIGERR_PREMATURE_END_OF_CHAR_CLASS:
    p = "premature end of char-class"; break;
  case ONIGERR_END_PATTERN_AT_ESCAPE:
    p = "end pattern at escape"; break;
  case ONIGERR_END_PATTERN_AT_META:
    p = "end pattern at meta"; break;
  case ONIGERR_END_PATTERN_AT_CONTROL:
    p = "end pattern at control"; break;
  case ONIGERR_META_CODE_SYNTAX:
    p = "invalid meta-code syntax"; break;
  case ONIGERR_CONTROL_CODE_SYNTAX:
    p = "invalid control-code syntax"; break;
  case ONIGERR_CHAR_CLASS_VALUE_AT_END_OF_RANGE:
    p = "char-class value at end of range"; break;
  case ONIGERR_CHAR_CLASS_VALUE_AT_START_OF_RANGE:
    p = "char-class value at start of range"; break;
  case ONIGERR_UNMATCHED_RANGE_SPECIFIER_IN_CHAR_CLASS:
    p = "unmatched range specifier in char-class"; break;
  case ONIGERR_TARGET_OF_REPEAT_OPERATOR_NOT_SPECIFIED:
    p = "target of repeat operator is not specified"; break;
  case ONIGERR_TARGET_OF_REPEAT_OPERATOR_INVALID:
    p = "target of repeat operator is invalid"; break;
  case ONIGERR_NESTED_REPEAT_OPERATOR:
    p = "nested repeat operator"; break;
  case ONIGERR_UNMATCHED_CLOSE_PARENTHESIS:
    p = "unmatched close parenthesis"; break;
  case ONIGERR_END_PATTERN_WITH_UNMATCHED_PARENTHESIS:
    p = "end pattern with unmatched parenthesis"; break;
  case ONIGERR_END_PATTERN_IN_GROUP:
    p = "end pattern in group"; break;
  case ONIGERR_UNDEFINED_GROUP_OPTION:
    p = "undefined group option"; break;
  case ONIGERR_INVALID_POSIX_BRACKET_TYPE:
    p = "invalid POSIX bracket type"; break;
  case ONIGERR_INVALID_LOOK_BEHIND_PATTERN:
    p = "invalid pattern in look-behind"; break;
  case ONIGERR_INVALID_REPEAT_RANGE_PATTERN:
    p = "invalid repeat range {lower,upper}"; break;
  case ONIGERR_TOO_BIG_NUMBER:
    p = "too big number"; break;
  case ONIGERR_TOO_BIG_NUMBER_FOR_REPEAT_RANGE:
    p = "too big number for repeat range"; break;
  case ONIGERR_UPPER_SMALLER_THAN_LOWER_IN_REPEAT_RANGE:
    p = "upper is smaller than lower in repeat range"; break;
  case ONIGERR_EMPTY_RANGE_IN_CHAR_CLASS:
    p = "empty range in char class"; break;
  case ONIGERR_MISMATCH_CODE_LENGTH_IN_CLASS_RANGE:
    p = "mismatch multibyte code length in char-class range"; break;
  case ONIGERR_TOO_MANY_MULTI_BYTE_RANGES:
    p = "too many multibyte code ranges are specified"; break;
  case ONIGERR_TOO_SHORT_MULTI_BYTE_STRING:
    p = "too short multibyte code string"; break;
  case ONIGERR_TOO_BIG_BACKREF_NUMBER:
    p = "too big backref number"; break;
  case ONIGERR_INVALID_BACKREF:
#ifdef USE_NAMED_GROUP
    p = "invalid backref number/name"; break;
#else
    p = "invalid backref number"; break;
#endif
  case ONIGERR_NUMBERED_BACKREF_OR_CALL_NOT_ALLOWED:
    p = "numbered backref/call is not allowed. (use name)"; break;
  case ONIGERR_TOO_BIG_WIDE_CHAR_VALUE:
    p = "too big wide-char value"; break;
  case ONIGERR_TOO_LONG_WIDE_CHAR_VALUE:
    p = "too long wide-char value"; break;
  case ONIGERR_INVALID_CODE_POINT_VALUE:
    p = "invalid code point value"; break;
  case ONIGERR_EMPTY_GROUP_NAME:
    p = "group name is empty"; break;
  case ONIGERR_INVALID_GROUP_NAME:
    p = "invalid group name <%n>"; break;
  case ONIGERR_INVALID_CHAR_IN_GROUP_NAME:
#ifdef USE_NAMED_GROUP
    p = "invalid char in group name <%n>"; break;
#else
    p = "invalid char in group number <%n>"; break;
#endif
  case ONIGERR_UNDEFINED_NAME_REFERENCE:
    p = "undefined name <%n> reference"; break;
  case ONIGERR_UNDEFINED_GROUP_REFERENCE:
    p = "undefined group <%n> reference"; break;
  case ONIGERR_MULTIPLEX_DEFINED_NAME:
    p = "multiplex defined name <%n>"; break;
  case ONIGERR_MULTIPLEX_DEFINITION_NAME_CALL:
    p = "multiplex definition name <%n> call"; break;
  case ONIGERR_NEVER_ENDING_RECURSION:
    p = "never ending recursion"; break;
  case ONIGERR_GROUP_NUMBER_OVER_FOR_CAPTURE_HISTORY:
    p = "group number is too big for capture history"; break;
  case ONIGERR_INVALID_CHAR_PROPERTY_NAME:
    p = "invalid character property name {%n}"; break;
  case ONIGERR_NOT_SUPPORTED_ENCODING_COMBINATION:
    p = "not supported encoding combination"; break;
  case ONIGERR_INVALID_COMBINATION_OF_OPTIONS:
    p = "invalid combination of options"; break;
  case ONIGERR_OVER_THREAD_PASS_LIMIT_COUNT:
    p = "over thread pass limit count"; break;

  default:
    p = "undefined error code"; break;
  }

  return (UChar* )p;
}

static void sprint_byte(char* s, unsigned int v)
{
  sprintf(s, "%02x", (v & 0377));
}

static void sprint_byte_with_x(char* s, unsigned int v)
{
  sprintf(s, "\\x%02x", (v & 0377));
}

static int to_ascii(OnigEncoding enc, UChar *s, UChar *end,
                    UChar buf[], int buf_size, int *is_over)
{
  int len;
  UChar *p;
  OnigCodePoint code;

  if (ONIGENC_MBC_MINLEN(enc) > 1) {
    p = s;
    len = 0;
    while (p < end) {
      code = ONIGENC_MBC_TO_CODE(enc, p, end);
      if (code >= 0x80) {
        if (code > 0xffff && len + 10 <= buf_size) {
          sprint_byte_with_x((char*)(&(buf[len])), (unsigned int)(code >> 24));
          sprint_byte((char*)(&(buf[len+4])),      (unsigned int)(code >> 16));
          sprint_byte((char*)(&(buf[len+6])),      (unsigned int)(code >>  8));
          sprint_byte((char*)(&(buf[len+8])),      (unsigned int)code);
          len += 10;
        }
        else if (len + 6 <= buf_size) {
          sprint_byte_with_x((char*)(&(buf[len])), (unsigned int)(code >> 8));
          sprint_byte((char*)(&(buf[len+4])),      (unsigned int)code);
          len += 6;
        }
        else {
          break;
        }
      }
      else {
        buf[len++] = (UChar )code;
      }

      p += enclen(enc, p, end);
      if (len >= buf_size) break;
    }

    *is_over = ((p < end) ? 1 : 0);
  }
  else {
    len = (int)MIN((end - s), buf_size);
    xmemcpy(buf, s, (size_t )len);
    *is_over = ((buf_size < (end - s)) ? 1 : 0);
  }

  return len;
}


/* for ONIG_MAX_ERROR_MESSAGE_LEN */
#define MAX_ERROR_PAR_LEN   30

extern int
onig_error_code_to_str(UChar* s, int code, ...)
{
  UChar *p, *q;
  OnigErrorInfo* einfo;
  size_t len;
  int is_over;
  UChar parbuf[MAX_ERROR_PAR_LEN];
  va_list vargs;

  va_start(vargs, code);

  switch (code) {
  case ONIGERR_UNDEFINED_NAME_REFERENCE:
  case ONIGERR_UNDEFINED_GROUP_REFERENCE:
  case ONIGERR_MULTIPLEX_DEFINED_NAME:
  case ONIGERR_MULTIPLEX_DEFINITION_NAME_CALL:
  case ONIGERR_INVALID_GROUP_NAME:
  case ONIGERR_INVALID_CHAR_IN_GROUP_NAME:
  case ONIGERR_INVALID_CHAR_PROPERTY_NAME:
    einfo = va_arg(vargs, OnigErrorInfo*);
    len = to_ascii(einfo->enc, einfo->par, einfo->par_end,
                   parbuf, MAX_ERROR_PAR_LEN - 3, &is_over);
    q = onig_error_code_to_format(code);
    p = s;
    while (*q != '\0') {
      if (*q == '%') {
        q++;
        if (*q == 'n') { /* '%n': name */
          xmemcpy(p, parbuf, len);
          p += len;
          if (is_over != 0) {
            xmemcpy(p, "...", 3);
            p += 3;
          }
          q++;
        }
        else
          goto normal_char;
      }
      else {
      normal_char:
        *p++ = *q++;
      }
    }
    *p = '\0';
    len = p - s;
    break;

  default:
    q = onig_error_code_to_format(code);
    len = onigenc_str_bytelen_null(ONIG_ENCODING_ASCII, q);
    xmemcpy(s, q, len);
    s[len] = '\0';
    break;
  }

  va_end(vargs);
  return (int)len;
}

void
onig_vsnprintf_with_pattern(UChar buf[], int bufsize, OnigEncoding enc,
                           UChar* pat, UChar* pat_end, const UChar *fmt, va_list args)
{
  size_t need;
  int n, len;
  UChar *p, *s, *bp;
  UChar bs[6];

  n = xvsnprintf((char* )buf, bufsize, (const char* )fmt, args);

  need = (pat_end - pat) * 4 + 4;

  if (n + need < (size_t)bufsize) {
    strcat((char* )buf, ": /");
    s = buf + onigenc_str_bytelen_null(ONIG_ENCODING_ASCII, buf);

    p = pat;
    while (p < pat_end) {
      if (*p == '\\') {
        *s++ = *p++;
        len = enclen(enc, p, pat_end);
        while (len-- > 0) *s++ = *p++;
      }
      else if (*p == '/') {
        *s++ = (unsigned char )'\\';
        *s++ = *p++;
      }
      else if (ONIGENC_IS_MBC_HEAD(enc, p, pat_end)) {
        len = enclen(enc, p, pat_end);
        if (ONIGENC_MBC_MINLEN(enc) == 1) {
          while (len-- > 0) *s++ = *p++;
        }
        else { /* for UTF16 */
          int blen;

          while (len-- > 0) {
            sprint_byte_with_x((char* )bs, (unsigned int )(*p++));
            blen = onigenc_str_bytelen_null(ONIG_ENCODING_ASCII, bs);
            bp = bs;
            while (blen-- > 0) *s++ = *bp++;
          }
        }
      }
      else if (!ONIGENC_IS_CODE_PRINT(enc, *p) &&
               !ONIGENC_IS_CODE_SPACE(enc, *p)) {
        sprint_byte_with_x((char* )bs, (unsigned int )(*p++));
        len = onigenc_str_bytelen_null(ONIG_ENCODING_ASCII, bs);
        bp = bs;
        while (len-- > 0) *s++ = *bp++;
      }
      else {
        *s++ = *p++;
      }
    }

    *s++ = '/';
    *s   = '\0';
  }
}

void
onig_snprintf_with_pattern(UChar buf[], int bufsize, OnigEncoding enc,
                           UChar* pat, UChar* pat_end, const UChar *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  onig_vsnprintf_with_pattern(buf, bufsize, enc,
          pat, pat_end, fmt, args);
  va_end(args);
}
#endif //ENABLE_REGEXP
