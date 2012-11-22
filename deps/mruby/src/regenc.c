/**********************************************************************
  regenc.c -  Oniguruma (regular expression library)
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
#ifdef INCLUDE_ENCODING
#include <string.h>
#include "regint.h"

OnigEncoding OnigEncDefaultCharEncoding = ONIG_ENCODING_INIT_DEFAULT;

extern int
onigenc_init(void)
{
  return 0;
}

extern OnigEncoding
onigenc_get_default_encoding(void)
{
  return OnigEncDefaultCharEncoding;
}

extern int
onigenc_set_default_encoding(OnigEncoding enc)
{
  OnigEncDefaultCharEncoding = enc;
  return 0;
}

extern int
onigenc_mbclen_approximate(const OnigUChar* p,const OnigUChar* e, struct OnigEncodingTypeST* enc)
{
  int ret = ONIGENC_PRECISE_MBC_ENC_LEN(enc,p,e);
  if (ONIGENC_MBCLEN_CHARFOUND_P(ret))
    return ONIGENC_MBCLEN_CHARFOUND_LEN(ret);
  else if (ONIGENC_MBCLEN_NEEDMORE_P(ret))
    return (int)(e-p)+ONIGENC_MBCLEN_NEEDMORE_LEN(ret);
  return 1;
}

extern UChar*
onigenc_get_right_adjust_char_head(OnigEncoding enc, const UChar* start, const UChar* s, const UChar* end)
{
  UChar* p = ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, start, s, end);
  if (p < s) {
      p += enclen(enc, p, end);
  }
  return p;
}

extern UChar*
onigenc_get_right_adjust_char_head_with_prev(OnigEncoding enc,
                                   const UChar* start, const UChar* s, const UChar* end, const UChar** prev)
{
  UChar* p = ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, start, s, end);

  if (p < s) {
    if (prev) *prev = (const UChar* )p;
    p += enclen(enc, p, end);
  }
  else {
    if (prev) *prev = (const UChar* )NULL; /* Sorry */
  }
  return p;
}

extern UChar*
onigenc_get_prev_char_head(OnigEncoding enc, const UChar* start, const UChar* s, const UChar* end)
{
  if (s <= start)
    return (UChar* )NULL;

  return ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, start, s - 1, end);
}

extern UChar*
onigenc_step_back(OnigEncoding enc, const UChar* start, const UChar* s, const UChar* end, int n)
{
  while (ONIG_IS_NOT_NULL(s) && n-- > 0) {
    if (s <= start)
      return (UChar* )NULL;

    s = ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, start, s - 1, end);
  }
  return (UChar* )s;
}

extern UChar*
onigenc_step(OnigEncoding enc, const UChar* p, const UChar* end, int n)
{
  UChar* q = (UChar* )p;
  while (n-- > 0) {
    q += ONIGENC_MBC_ENC_LEN(enc, q, end);
  }
  return (q <= end ? q : NULL);
}

extern int
onigenc_strlen(OnigEncoding enc, const UChar* p, const UChar* end)
{
  int n = 0;
  UChar* q = (UChar* )p;

  while (q < end) {
    q += ONIGENC_MBC_ENC_LEN(enc, q, end);
    n++;
  }
  return n;
}

extern int
onigenc_strlen_null(OnigEncoding enc, const UChar* s)
{
  int n = 0;
  UChar* p = (UChar* )s;
  UChar* e;

  while (1) {
    if (*p == '\0') {
      UChar* q;
      int len = ONIGENC_MBC_MINLEN(enc);

      if (len == 1) return n;
      q = p + 1;
      while (len > 1) {
        if (*q != '\0') break;
        q++;
        len--;
      }
      if (len == 1) return n;
    }
    e = p + ONIGENC_MBC_MAXLEN(enc);
    p += ONIGENC_MBC_ENC_LEN(enc, p, e);
    n++;
  }
}

extern int
onigenc_str_bytelen_null(OnigEncoding enc, const UChar* s)
{
  UChar* start = (UChar* )s;
  UChar* p = (UChar* )s;
  UChar* e;

  while (1) {
    if (*p == '\0') {
      UChar* q;
      int len = ONIGENC_MBC_MINLEN(enc);

      if (len == 1) return (int )(p - start);
      q = p + 1;
      while (len > 1) {
        if (*q != '\0') break;
        q++;
        len--;
      }
      if (len == 1) return (int )(p - start);
    }
    e = p + ONIGENC_MBC_MAXLEN(enc);
    p += ONIGENC_MBC_ENC_LEN(enc, p, e);
  }
}

const UChar OnigEncAsciiToLowerCaseTable[] = {
  '\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
  '\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
  '\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
  '\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
  '\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
  '\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
  '\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
  '\100', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
  '\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
  '\170', '\171', '\172', '\133', '\134', '\135', '\136', '\137',
  '\140', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
  '\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
  '\170', '\171', '\172', '\173', '\174', '\175', '\176', '\177',
  '\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
  '\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
  '\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
  '\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
  '\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
  '\300', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\312', '\313', '\314', '\315', '\316', '\317',
  '\320', '\321', '\322', '\323', '\324', '\325', '\326', '\327',
  '\330', '\331', '\332', '\333', '\334', '\335', '\336', '\337',
  '\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
  '\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
  '\370', '\371', '\372', '\373', '\374', '\375', '\376', '\377',
};

#ifdef USE_UPPER_CASE_TABLE
const UChar OnigEncAsciiToUpperCaseTable[256] = {
  '\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
  '\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
  '\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
  '\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
  '\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
  '\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
  '\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
  '\100', '\101', '\102', '\103', '\104', '\105', '\106', '\107',
  '\110', '\111', '\112', '\113', '\114', '\115', '\116', '\117',
  '\120', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\132', '\133', '\134', '\135', '\136', '\137',
  '\140', '\101', '\102', '\103', '\104', '\105', '\106', '\107',
  '\110', '\111', '\112', '\113', '\114', '\115', '\116', '\117',
  '\120', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\132', '\173', '\174', '\175', '\176', '\177',
  '\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
  '\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
  '\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
  '\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
  '\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
  '\300', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\312', '\313', '\314', '\315', '\316', '\317',
  '\320', '\321', '\322', '\323', '\324', '\325', '\326', '\327',
  '\330', '\331', '\332', '\333', '\334', '\335', '\336', '\337',
  '\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
  '\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
  '\370', '\371', '\372', '\373', '\374', '\375', '\376', '\377',
};
#endif

const unsigned short OnigEncAsciiCtypeTable[256] = {
  0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008,
  0x4008, 0x420c, 0x4209, 0x4208, 0x4208, 0x4208, 0x4008, 0x4008,
  0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008,
  0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008, 0x4008,
  0x4284, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0,
  0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0,
  0x78b0, 0x78b0, 0x78b0, 0x78b0, 0x78b0, 0x78b0, 0x78b0, 0x78b0,
  0x78b0, 0x78b0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x41a0,
  0x41a0, 0x7ca2, 0x7ca2, 0x7ca2, 0x7ca2, 0x7ca2, 0x7ca2, 0x74a2,
  0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2,
  0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2, 0x74a2,
  0x74a2, 0x74a2, 0x74a2, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x51a0,
  0x41a0, 0x78e2, 0x78e2, 0x78e2, 0x78e2, 0x78e2, 0x78e2, 0x70e2,
  0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2,
  0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2, 0x70e2,
  0x70e2, 0x70e2, 0x70e2, 0x41a0, 0x41a0, 0x41a0, 0x41a0, 0x4008,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

const UChar OnigEncISO_8859_1_ToLowerCaseTable[256] = {
  '\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
  '\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
  '\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
  '\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
  '\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
  '\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
  '\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
  '\100', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
  '\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
  '\170', '\171', '\172', '\133', '\134', '\135', '\136', '\137',
  '\140', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
  '\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
  '\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
  '\170', '\171', '\172', '\173', '\174', '\175', '\176', '\177',
  '\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
  '\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
  '\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
  '\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
  '\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
  '\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
  '\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\327',
  '\370', '\371', '\372', '\373', '\374', '\375', '\376', '\337',
  '\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
  '\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
  '\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
  '\370', '\371', '\372', '\373', '\374', '\375', '\376', '\377'
};

#ifdef USE_UPPER_CASE_TABLE
const UChar OnigEncISO_8859_1_ToUpperCaseTable[256] = {
  '\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
  '\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
  '\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
  '\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
  '\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
  '\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
  '\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
  '\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
  '\100', '\101', '\102', '\103', '\104', '\105', '\106', '\107',
  '\110', '\111', '\112', '\113', '\114', '\115', '\116', '\117',
  '\120', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\132', '\133', '\134', '\135', '\136', '\137',
  '\140', '\101', '\102', '\103', '\104', '\105', '\106', '\107',
  '\110', '\111', '\112', '\113', '\114', '\115', '\116', '\117',
  '\120', '\121', '\122', '\123', '\124', '\125', '\126', '\127',
  '\130', '\131', '\132', '\173', '\174', '\175', '\176', '\177',
  '\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
  '\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
  '\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
  '\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
  '\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
  '\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
  '\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
  '\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
  '\300', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\312', '\313', '\314', '\315', '\316', '\317',
  '\320', '\321', '\322', '\323', '\324', '\325', '\326', '\327',
  '\330', '\331', '\332', '\333', '\334', '\335', '\336', '\337',
  '\300', '\301', '\302', '\303', '\304', '\305', '\306', '\307',
  '\310', '\311', '\312', '\313', '\314', '\315', '\316', '\317',
  '\320', '\321', '\322', '\323', '\324', '\325', '\326', '\367',
  '\330', '\331', '\332', '\333', '\334', '\335', '\336', '\377',
};
#endif

extern void
onigenc_set_default_caseconv_table(const UChar* table ARG_UNUSED)
{
  /* nothing */
  /* obsoleted. */
}

extern UChar*
onigenc_get_left_adjust_char_head(OnigEncoding enc, const UChar* start, const UChar* s, const UChar* end)
{
  return ONIGENC_LEFT_ADJUST_CHAR_HEAD(enc, start, s, end);
}

const OnigPairCaseFoldCodes OnigAsciiLowerMap[] = {
  { 0x41, 0x61 },
  { 0x42, 0x62 },
  { 0x43, 0x63 },
  { 0x44, 0x64 },
  { 0x45, 0x65 },
  { 0x46, 0x66 },
  { 0x47, 0x67 },
  { 0x48, 0x68 },
  { 0x49, 0x69 },
  { 0x4a, 0x6a },
  { 0x4b, 0x6b },
  { 0x4c, 0x6c },
  { 0x4d, 0x6d },
  { 0x4e, 0x6e },
  { 0x4f, 0x6f },
  { 0x50, 0x70 },
  { 0x51, 0x71 },
  { 0x52, 0x72 },
  { 0x53, 0x73 },
  { 0x54, 0x74 },
  { 0x55, 0x75 },
  { 0x56, 0x76 },
  { 0x57, 0x77 },
  { 0x58, 0x78 },
  { 0x59, 0x79 },
  { 0x5a, 0x7a }
};

extern int
onigenc_ascii_apply_all_case_fold(OnigCaseFoldType flag ARG_UNUSED,
                                  OnigApplyAllCaseFoldFunc f, void* arg,
                                  OnigEncoding enc ARG_UNUSED)
{
  OnigCodePoint code;
  int i, r;

  for (i = 0;
       i < (int )(sizeof(OnigAsciiLowerMap)/sizeof(OnigPairCaseFoldCodes));
       i++) {
    code = OnigAsciiLowerMap[i].to;
    r = (*f)(OnigAsciiLowerMap[i].from, &code, 1, arg);
    if (r != 0) return r;

    code = OnigAsciiLowerMap[i].from;
    r = (*f)(OnigAsciiLowerMap[i].to, &code, 1, arg);
    if (r != 0) return r;
  }

  return 0;
}

extern int
onigenc_ascii_get_case_fold_codes_by_str(OnigCaseFoldType flag ARG_UNUSED,
    const OnigUChar* p, const OnigUChar* end ARG_UNUSED, OnigCaseFoldCodeItem items[],
     OnigEncoding enc ARG_UNUSED)
{
  if (0x41 <= *p && *p <= 0x5a) {
    items[0].byte_len = 1;
    items[0].code_len = 1;
    items[0].code[0] = (OnigCodePoint )(*p + 0x20);
    return 1;
  }
  else if (0x61 <= *p && *p <= 0x7a) {
    items[0].byte_len = 1;
    items[0].code_len = 1;
    items[0].code[0] = (OnigCodePoint )(*p - 0x20);
    return 1;
  }
  else
    return 0;
}

static int
ss_apply_all_case_fold(OnigCaseFoldType flag ARG_UNUSED,
                       OnigApplyAllCaseFoldFunc f, void* arg)
{
  OnigCodePoint ss[] = { 0x73, 0x73 };

  return (*f)((OnigCodePoint )0xdf, ss, 2, arg);
}

extern int
onigenc_apply_all_case_fold_with_map(int map_size,
    const OnigPairCaseFoldCodes map[],
    int ess_tsett_flag, OnigCaseFoldType flag,
    OnigApplyAllCaseFoldFunc f, void* arg)
{
  OnigCodePoint code;
  int i, r;

  r = onigenc_ascii_apply_all_case_fold(flag, f, arg, 0);
  if (r != 0) return r;

  for (i = 0; i < map_size; i++) {
    code = map[i].to;
    r = (*f)(map[i].from, &code, 1, arg);
    if (r != 0) return r;

    code = map[i].from;
    r = (*f)(map[i].to, &code, 1, arg);
    if (r != 0) return r;
  }

  if (ess_tsett_flag != 0)
    return ss_apply_all_case_fold(flag, f, arg);

  return 0;
}

extern int
onigenc_get_case_fold_codes_by_str_with_map(int map_size,
    const OnigPairCaseFoldCodes map[],
    int ess_tsett_flag, OnigCaseFoldType flag ARG_UNUSED,
    const OnigUChar* p, const OnigUChar* end, OnigCaseFoldCodeItem items[])
{
  if (0x41 <= *p && *p <= 0x5a) {
    items[0].byte_len = 1;
    items[0].code_len = 1;
    items[0].code[0] = (OnigCodePoint )(*p + 0x20);
    if (*p == 0x53 && ess_tsett_flag != 0 && end > p + 1
        && (*(p+1) == 0x53 || *(p+1) == 0x73)) {
      /* SS */
      items[1].byte_len = 2;
      items[1].code_len = 1;
      items[1].code[0] = (OnigCodePoint )0xdf;
      return 2;
    }
    else
      return 1;
  }
  else if (0x61 <= *p && *p <= 0x7a) {
    items[0].byte_len = 1;
    items[0].code_len = 1;
    items[0].code[0] = (OnigCodePoint )(*p - 0x20);
    if (*p == 0x73 && ess_tsett_flag != 0 && end > p + 1
        && (*(p+1) == 0x73 || *(p+1) == 0x53)) {
      /* ss */
      items[1].byte_len = 2;
      items[1].code_len = 1;
      items[1].code[0] = (OnigCodePoint )0xdf;
      return 2;
    }
    else
      return 1;
  }
  else if (*p == 0xdf && ess_tsett_flag != 0) {
    items[0].byte_len = 1;
    items[0].code_len = 2;
    items[0].code[0] = (OnigCodePoint )'s';
    items[0].code[1] = (OnigCodePoint )'s';

    items[1].byte_len = 1;
    items[1].code_len = 2;
    items[1].code[0] = (OnigCodePoint )'S';
    items[1].code[1] = (OnigCodePoint )'S';

    items[2].byte_len = 1;
    items[2].code_len = 2;
    items[2].code[0] = (OnigCodePoint )'s';
    items[2].code[1] = (OnigCodePoint )'S';

    items[3].byte_len = 1;
    items[3].code_len = 2;
    items[3].code[0] = (OnigCodePoint )'S';
    items[3].code[1] = (OnigCodePoint )'s';

    return 4;
  }
  else {
    int i;

    for (i = 0; i < map_size; i++) {
      if (*p == map[i].from) {
        items[0].byte_len = 1;
        items[0].code_len = 1;
        items[0].code[0] = map[i].to;
        return 1;
      }
      else if (*p == map[i].to) {
        items[0].byte_len = 1;
        items[0].code_len = 1;
        items[0].code[0] = map[i].from;
        return 1;
      }
    }
  }

  return 0;
}


extern int
onigenc_not_support_get_ctype_code_range(OnigCtype ctype,
                       OnigCodePoint* sb_out, const OnigCodePoint* ranges[],
                       OnigEncoding enc)
{
  return ONIG_NO_SUPPORT_CONFIG;
}

extern int
onigenc_is_mbc_newline_0x0a(const UChar* p, const UChar* end, OnigEncoding enc ARG_UNUSED)
{
  if (p < end) {
    if (*p == 0x0a) return 1;
  }
  return 0;
}

/* for single byte encodings */
extern int
onigenc_ascii_mbc_case_fold(OnigCaseFoldType flag ARG_UNUSED, const UChar** p,
                            const UChar*end, UChar* lower, OnigEncoding enc ARG_UNUSED)
{
  *lower = ONIGENC_ASCII_CODE_TO_LOWER_CASE(**p);

  (*p)++;
  return 1; /* return byte length of converted char to lower */
}

extern int
onigenc_single_byte_mbc_enc_len(const UChar* p ARG_UNUSED, const UChar* e ARG_UNUSED,
                                OnigEncoding enc ARG_UNUSED)
{
  return 1;
}

extern OnigCodePoint
onigenc_single_byte_mbc_to_code(const UChar* p, const UChar* end ARG_UNUSED,
                                OnigEncoding enc ARG_UNUSED)
{
  return (OnigCodePoint )(*p);
}

extern int
onigenc_single_byte_code_to_mbclen(OnigCodePoint code ARG_UNUSED, OnigEncoding enc ARG_UNUSED)
{
  return 1;
}

extern int
onigenc_single_byte_code_to_mbc(OnigCodePoint code, UChar *buf, OnigEncoding enc ARG_UNUSED)
{
  *buf = (UChar )(code & 0xff);
  return 1;
}

extern UChar*
onigenc_single_byte_left_adjust_char_head(const UChar* start ARG_UNUSED, const UChar* s,
                                          const UChar* end,
                                          OnigEncoding enc ARG_UNUSED)
{
  return (UChar* )s;
}

extern int
onigenc_always_true_is_allowed_reverse_match(const UChar* s ARG_UNUSED, const UChar* end ARG_UNUSED,
                                             OnigEncoding enc ARG_UNUSED)
{
  return TRUE;
}

extern int
onigenc_always_false_is_allowed_reverse_match(const UChar* s ARG_UNUSED, const UChar* end ARG_UNUSED,
                                              OnigEncoding enc ARG_UNUSED)
{
  return FALSE;
}

extern int
onigenc_ascii_is_code_ctype(OnigCodePoint code, unsigned int ctype,
                            OnigEncoding enc ARG_UNUSED)
{
  if (code < 128)
    return ONIGENC_IS_ASCII_CODE_CTYPE(code, ctype);
  else
    return FALSE;
}

extern OnigCodePoint
onigenc_mbn_mbc_to_code(OnigEncoding enc, const UChar* p, const UChar* end)
{
  int c, i, len;
  OnigCodePoint n;

  len = enclen(enc, p, end);
  n = (OnigCodePoint )(*p++);
  if (len == 1) return n;

  for (i = 1; i < len; i++) {
    if (p >= end) break;
    c = *p++;
    n <<= 8;  n += c;
  }
  return n;
}

extern int
onigenc_mbn_mbc_case_fold(OnigEncoding enc, OnigCaseFoldType flag ARG_UNUSED,
                          const UChar** pp, const UChar* end ARG_UNUSED,
                          UChar* lower)
{
  int len;
  const UChar *p = *pp;

  if (ONIGENC_IS_MBC_ASCII(p)) {
    *lower = ONIGENC_ASCII_CODE_TO_LOWER_CASE(*p);
    (*pp)++;
    return 1;
  }
  else {
    int i;

    len = enclen(enc, p, end);
    for (i = 0; i < len; i++) {
      *lower++ = *p++;
    }
    (*pp) += len;
    return len; /* return byte length of converted to lower char */
  }
}

extern int
onigenc_mb2_code_to_mbclen(OnigCodePoint code, OnigEncoding enc ARG_UNUSED)
{
  if ((code & 0xff00) != 0) return 2;
  else return 1;
}

extern int
onigenc_mb4_code_to_mbclen(OnigCodePoint code, OnigEncoding enc ARG_UNUSED)
{
       if ((code & 0xff000000) != 0) return 4;
  else if ((code & 0xff0000) != 0) return 3;
  else if ((code & 0xff00) != 0) return 2;
  else return 1;
}

extern int
onigenc_mb2_code_to_mbc(OnigEncoding enc, OnigCodePoint code, UChar *buf)
{
  UChar *p = buf;

  if ((code & 0xff00) != 0) {
    *p++ = (UChar )((code >>  8) & 0xff);
  }
  *p++ = (UChar )(code & 0xff);

  if (enclen(enc, buf, p) != (p - buf))
    return ONIGERR_INVALID_CODE_POINT_VALUE;
  return (int)(p - buf);
}

extern int
onigenc_mb4_code_to_mbc(OnigEncoding enc, OnigCodePoint code, UChar *buf)
{
  UChar *p = buf;

  if ((code & 0xff000000) != 0) {
    *p++ = (UChar )((code >> 24) & 0xff);
  }
  if ((code & 0xff0000) != 0 || p != buf) {
    *p++ = (UChar )((code >> 16) & 0xff);
  }
  if ((code & 0xff00) != 0 || p != buf) {
    *p++ = (UChar )((code >> 8) & 0xff);
  }
  *p++ = (UChar )(code & 0xff);

  if (enclen(enc, buf, p) != (p - buf))
    return ONIGERR_INVALID_CODE_POINT_VALUE;
  return (int)(p - buf);
}

extern int
onigenc_minimum_property_name_to_ctype(OnigEncoding enc, UChar* p, UChar* end)
{
  static const PosixBracketEntryType PBS[] = {
    PosixBracketEntryInit("Alnum",  ONIGENC_CTYPE_ALNUM),
    PosixBracketEntryInit("Alpha",  ONIGENC_CTYPE_ALPHA),
    PosixBracketEntryInit("Blank",  ONIGENC_CTYPE_BLANK),
    PosixBracketEntryInit("Cntrl",  ONIGENC_CTYPE_CNTRL),
    PosixBracketEntryInit("Digit",  ONIGENC_CTYPE_DIGIT),
    PosixBracketEntryInit("Graph",  ONIGENC_CTYPE_GRAPH),
    PosixBracketEntryInit("Lower",  ONIGENC_CTYPE_LOWER),
    PosixBracketEntryInit("Print",  ONIGENC_CTYPE_PRINT),
    PosixBracketEntryInit("Punct",  ONIGENC_CTYPE_PUNCT),
    PosixBracketEntryInit("Space",  ONIGENC_CTYPE_SPACE),
    PosixBracketEntryInit("Upper",  ONIGENC_CTYPE_UPPER),
    PosixBracketEntryInit("XDigit", ONIGENC_CTYPE_XDIGIT),
    PosixBracketEntryInit("ASCII",  ONIGENC_CTYPE_ASCII),
    PosixBracketEntryInit("Word",   ONIGENC_CTYPE_WORD),
  };

  const PosixBracketEntryType *pb, *pbe;
  int len;

  len = onigenc_strlen(enc, p, end);
  for (pbe = (pb = PBS) + sizeof(PBS)/sizeof(PBS[0]); pb < pbe; ++pb) {
    if (len == pb->len &&
        onigenc_with_ascii_strncmp(enc, p, end, pb->name, pb->len) == 0)
      return pb->ctype;
  }

  return ONIGERR_INVALID_CHAR_PROPERTY_NAME;
}

extern int
onigenc_mb2_is_code_ctype(OnigEncoding enc, OnigCodePoint code,
                          unsigned int ctype)
{
  if (code < 128)
    return ONIGENC_IS_ASCII_CODE_CTYPE(code, ctype);
  else {
    if (CTYPE_IS_WORD_GRAPH_PRINT(ctype)) {
      return (ONIGENC_CODE_TO_MBCLEN(enc, code) > 1 ? TRUE : FALSE);
    }
  }

  return FALSE;
}

extern int
onigenc_mb4_is_code_ctype(OnigEncoding enc, OnigCodePoint code,
                          unsigned int ctype)
{
  if (code < 128)
    return ONIGENC_IS_ASCII_CODE_CTYPE(code, ctype);
  else {
    if (CTYPE_IS_WORD_GRAPH_PRINT(ctype)) {
      return (ONIGENC_CODE_TO_MBCLEN(enc, code) > 1 ? TRUE : FALSE);
    }
  }

  return FALSE;
}

extern int
onigenc_with_ascii_strncmp(OnigEncoding enc, const UChar* p, const UChar* end,
                           const UChar* sascii /* ascii */, int n)
{
  int x, c;

  while (n-- > 0) {
    if (p >= end) return (int )(*sascii);

    c = (int )ONIGENC_MBC_TO_CODE(enc, p, end);
    x = *sascii - c;
    if (x) return x;

    sascii++;
    p += enclen(enc, p, end);
  }
  return 0;
}

/* Property management */
static int
resize_property_list(int new_size, const OnigCodePoint*** plist, int* psize)
{
  size_t size;
  const OnigCodePoint **list = *plist;

  size = sizeof(OnigCodePoint*) * new_size;
  if (IS_NULL(list)) {
    list = (const OnigCodePoint** )xmalloc(size);
  }
  else {
    list = (const OnigCodePoint** )xrealloc((void* )list, size);
  }

  if (IS_NULL(list)) return ONIGERR_MEMORY;

  *plist = list;
  *psize = new_size;

  return 0;
}

extern int
onigenc_property_list_add_property(UChar* name, const OnigCodePoint* prop,
     hash_table_type **table, const OnigCodePoint*** plist, int *pnum,
     int *psize)
{
#define PROP_INIT_SIZE     16

  int r;

  if (*psize <= *pnum) {
    int new_size = (*psize == 0 ? PROP_INIT_SIZE : *psize * 2);
    r = resize_property_list(new_size, plist, psize);
    if (r != 0) return r;
  }

  (*plist)[*pnum] = prop;

  if (ONIG_IS_NULL(*table)) {
    *table = onig_st_init_strend_table_with_size(PROP_INIT_SIZE);
    if (ONIG_IS_NULL(*table)) return ONIGERR_MEMORY;
  }

  *pnum = *pnum + 1;
  onig_st_insert_strend(*table, name, name + strlen((char* )name),
                        (hash_data_type )(*pnum + ONIGENC_MAX_STD_CTYPE));
  return 0;
}

extern int
onigenc_property_list_init(int (*f)(void))
{
  int r;

  THREAD_ATOMIC_START;

  r = f();

  THREAD_ATOMIC_END;
  return r;
}
#endif //INCLUDE_ENCODING
