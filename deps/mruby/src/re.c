/*
** re.c - Regexp class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include <string.h>
#include "mruby/string.h"
#include "encoding.h"
#include "re.h"
#include "mruby/array.h"
#include "regint.h"
#include "mruby/class.h"
#include "error.h"
#ifdef ENABLE_REGEXP

#define REGEX_CLASS (mrb_class_obj_get(mrb, "Regexp"))
#define MATCH_CLASS (mrb_class_obj_get(mrb, "MatchData"))

//from opcode.h
#define GETARG_A(i)   ((((mrb_code)(i)) >> 24) & 0xff)
#define GETARG_B(i)   ((((mrb_code)(i)) >> 16) & 0xff)
#define GETARG_C(i)   ((((mrb_code)(i)) >>  8) & 0xff)
#define MKARG_A(c)    (((c) & 0xff) << 24)
#define MKARG_B(c)    (((c) & 0xff) << 16)
#define MKARG_C(c)    (((c) & 0xff) <<  8)

#define ARG_REG_OPTION_MASK \
    (ONIG_OPTION_IGNORECASE|ONIG_OPTION_MULTILINE|ONIG_OPTION_EXTEND)
#define ARG_ENCODING_FIXED    16
#define ARG_ENCODING_NONE     32
#define REG_LITERAL FL_USER5
#define REG_ENCODING_NONE FL_USER6
typedef char onig_errmsg_buffer[ONIG_MAX_ERROR_MESSAGE_LEN];
#define mrb_bug printf
#define KCODE_FIXED FL_USER4
#define scan_oct(s,l,e) (int)ruby_scan_oct(s,l,e)
unsigned long ruby_scan_oct(const char*, size_t, size_t*);
#define scan_hex(s,l,e) (int)ruby_scan_hex(s,l,e)
unsigned long ruby_scan_hex(const char*, size_t, size_t*);

static mrb_value mrb_match_to_a(mrb_state *mrb, mrb_value match);
static mrb_value mrb_reg_preprocess(mrb_state *mrb, const char *p, const char *end, onig_errmsg_buffer err);
static void mrb_reg_expr_str(mrb_state *mrb, mrb_value str, const char *s, long len);
static char * option_to_str(char str[4], int options);

//static int may_need_recompile;
//static int reg_kcode = DEFAULT_KCODE;
/* ------------------------------------------------------------------------- */
/* RegExp Class */
/* ------------------------------------------------------------------------- */
/* 15.2.15.6.1  */
/*
 *  call-seq:
 *     class.new(args, ...)    ->  obj
 *
 *  Calls <code>allocate</code> to create a new object of
 *  <i>class</i>'s class, then invokes that object's
 *  <code>initialize</code> method, passing it <i>args</i>.
 *  This is the method that ends up getting called whenever
 *  an object is constructed using .new.
 *
 */
mrb_value
mrb_reg_s_new_instance(mrb_state *mrb, /*int argc, mrb_value *argv, */mrb_value self)
{
  mrb_value argv[16];
  int argc;
  struct RRegexp *re;

  mrb_get_args(mrb, "*", &argv, &argc);
  re = (struct RRegexp*)mrb_obj_alloc(mrb, MRB_TT_REGEX, REGEX_CLASS);
  re->ptr = 0;
  re->src = 0;
  re->usecnt = 0;
  return mrb_funcall_argv(mrb, mrb_obj_value(re), mrb->init_sym, argc, argv);
}

mrb_value
mrb_reg_quote(mrb_state *mrb, mrb_value str)
{
  char *s, *send, *t;
  mrb_value tmp;
  int c;

  s = RSTRING_PTR(str);
  send = s + RSTRING_LEN(str);
  while (s < send) {
    c = *s;
    if (c == -1) {
      s += send - s;
      continue;
    }
    switch (c) {
      case '[': case ']': case '{': case '}':
      case '(': case ')': case '|': case '-':
      case '*': case '.': case '\\':
      case '?': case '+': case '^': case '$':
      case ' ': case '#':
      case '\t': case '\f': case '\n': case '\r':
        goto meta_found;
    }
    s++;
  }
  tmp = mrb_str_new(mrb, RSTRING_PTR(str), RSTRING_LEN(str));
  return tmp;

meta_found:
  tmp = mrb_str_new(mrb, 0, RSTRING_LEN(str)*2);
  t = RSTRING_PTR(tmp);
  /* copy upto metacharacter */
  memcpy(t, RSTRING_PTR(str), s - RSTRING_PTR(str));
  t += s - RSTRING_PTR(str);

  while (s < send) {
    c = *s;
    if (c == -1) {
      int n = send - s;

      while (n--)
        *t++ = *s++;
      continue;
    }
    s++;
    switch (c) {
      case '[': case ']': case '{': case '}':
      case '(': case ')': case '|': case '-':
      case '*': case '.': case '\\':
      case '?': case '+': case '^': case '$':
      case '#':
        t += mrb_enc_mbcput('\\', t, enc);
        break;
      case ' ':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput(' ', t, enc);
        continue;
      case '\t':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput('t', t, enc);
        continue;
      case '\n':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput('n', t, enc);
        continue;
      case '\r':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput('r', t, enc);
        continue;
      case '\f':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput('f', t, enc);
        continue;
      case '\v':
        t += mrb_enc_mbcput('\\', t, enc);
        t += mrb_enc_mbcput('v', t, enc);
        continue;
    }
    t += mrb_enc_mbcput(c, t, enc);
  }
  mrb_str_resize(mrb, tmp, t - RSTRING_PTR(tmp));

  return tmp;
}

static mrb_value
reg_operand(mrb_state *mrb, mrb_value s, int check)
{
  if (mrb_type(s) == MRB_TT_SYMBOL) {
    //return mrb_sym_to_s(s);
    return mrb_obj_inspect(mrb, s);
  }
  else {
    mrb_value tmp = mrb_check_string_type(mrb, s);
    if (check && mrb_nil_p(tmp)) {
      mrb_raise(mrb, E_TYPE_ERROR, "can't convert %s to String",
         mrb_obj_classname(mrb, s));
    }
    return tmp;
  }
}
/* 15.2.15.6.2  */
/* 15.2.15.6.4  */
/*
 *  call-seq:
 *     Regexp.escape(str)   -> string
 *     Regexp.quote(str)    -> string
 *
 *  Escapes any characters that would have special meaning in a regular
 *  expression. Returns a new escaped string, or self if no characters are
 *  escaped.  For any string,
 *  <code>Regexp.new(Regexp.escape(<i>str</i>))=~<i>str</i></code> will be true.
 *
 *     Regexp.escape('\*?{}.')   #=> \\\*\?\{\}\.
 *
 */

static mrb_value
mrb_reg_s_quote(mrb_state *mrb, mrb_value c/*, mrb_value str*/)
{
    mrb_value str;

    mrb_get_args(mrb, "o", &str);
    return mrb_reg_quote(mrb, reg_operand(mrb, str, 1/*TRUE*/));
}

static void
match_check(mrb_state *mrb, mrb_value match)
{
  struct RMatch  *m = mrb_match_ptr(match);
  if (!m->str) {
    mrb_raise(mrb, E_TYPE_ERROR, "uninitialized Match");
  }
}

mrb_value
mrb_reg_nth_match(mrb_state *mrb, mrb_int nth, mrb_value match)
{
  mrb_value str;
  long start, end, len;
  struct RMatch  *m = mrb_match_ptr(match);

  if (mrb_nil_p(match)) return mrb_nil_value();
  match_check(mrb, match);
  if (nth >= m->rmatch->regs.num_regs) {
    return mrb_nil_value();
  }
  if (nth < 0) {
    nth += m->rmatch->regs.num_regs;
    if (nth <= 0) return mrb_nil_value();
  }
  start = m->rmatch->regs.beg[nth];
  if (start == -1) return mrb_nil_value();
  end = m->rmatch->regs.end[nth];
  len = end - start;
  str = mrb_str_subseq(mrb, mrb_obj_value(m->str), start, len);

  return str;
}

mrb_value
mrb_reg_last_match(mrb_state *mrb, mrb_value match)
{
  return mrb_reg_nth_match(mrb, 0, match);
}


static int
match_backref_number(mrb_state *mrb, mrb_value match, mrb_value backref)
{
  const char *name;
  int num;

  struct re_registers *regs = RMATCH_REGS(match);
  struct RRegexp *regexp = RMATCH(match)->regexp;

  match_check(mrb, match);
  switch(mrb_type(backref)) {
    default:
      return mrb_fixnum(backref);

    case MRB_TT_SYMBOL:
      name = mrb_sym2name(mrb, mrb_symbol(backref));
      break;

    case MRB_TT_STRING:
      //name = StringValueCStr(backref);
      name = mrb_string_value_cstr(mrb, &backref);
      break;
  }
  num = onig_name_to_backref_number(regexp->ptr,
              (const unsigned char*)name,
              (const unsigned char*)name + strlen(name),
              regs);
  if (num < 1) {
    mrb_raise(mrb, E_INDEX_ERROR, "undefined group name reference: %s", name);
  }

  return num;
}
/* 15.2.15.6.3  */
/*
 *  call-seq:
 *     Regexp.last_match           -> matchdata
 *     Regexp.last_match(n)        -> str
 *
 *  The first form returns the <code>MatchData</code> object generated by the
 *  last successful pattern match. Equivalent to reading the global variable
 *  <code>$~</code>. The second form returns the <i>n</i>th field in this
 *  <code>MatchData</code> object.
 *  <em>n</em> can be a string or symbol to reference a named capture.
 *
 *  Note that the <code>last_match</code> is local to the thread and method scope
 *  of the method that did the pattern match.
 *
 *     /c(.)t/ =~ 'cat'        #=> 0
 *     Regexp.last_match       #=> #<MatchData "cat" 1:"a">
 *     Regexp.last_match(0)    #=> "cat"
 *     Regexp.last_match(1)    #=> "a"
 *     Regexp.last_match(2)    #=> nil
 *
 *     /(?<lhs>\w+)\s*=\s*(?<rhs>\w+)/ =~ "var = val"
 *     Regexp.last_match       #=> #<MatchData "var = val" lhs:"var" rhs:"val">
 *     Regexp.last_match(:lhs) #=> "var"
 *     Regexp.last_match(:rhs) #=> "val"
 */
static mrb_value
mrb_reg_s_last_match(mrb_state *mrb, mrb_value self/*int argc, mrb_value *argv*/)
{
  //mrb_value nth;
  mrb_value argv[16];
  int argc;
  mrb_value match = mrb_backref_get(mrb);

  //if (argc > 0 && mrb_scan_args(argc, argv, "01", &nth) == 1) {
  mrb_get_args(mrb, "*", &argv, &argc);
  if (argc != 0) {
    int n;
    if (mrb_nil_p(match)) return mrb_nil_value();
    n = match_backref_number(mrb, match, argv[0]);
    return mrb_reg_nth_match(mrb, n, match);
  }
  return match;//match_getter();
}

static void
mrb_reg_check(mrb_state *mrb, mrb_value re)
{
  //struct RRegexp *r = mrb_regex_ptr(re);

  //if (!(RREGEXP(re)->ptr) || !RREGEXP_SRC(re) || !RREGEXP_SRC_PTR(re)) {
  if (!(RREGEXP(re)->ptr)) {
    mrb_raise(mrb, E_TYPE_ERROR, "uninitialized Regexp");
  }
  if (RREGEXP(re)->src == 0) {
    mrb_raise(mrb, E_TYPE_ERROR, "uninitialized Regexp");
  }
}

int
mrb_reg_options(mrb_state *mrb, mrb_value re)
{
    int options;

    mrb_reg_check(mrb, re);
    options = RREGEXP(re)->ptr->options & ARG_REG_OPTION_MASK;
    if (mrb_basic(re)->flags & KCODE_FIXED) options |= ARG_ENCODING_FIXED;
    if (mrb_basic(re)->flags & REG_ENCODING_NONE) options |= ARG_ENCODING_NONE;
    return options;
}

static mrb_value
mrb_reg_desc(mrb_state *mrb, const char *s, long len, mrb_value re)
{
  mrb_value str = mrb_str_new(mrb, "/", 1);

  mrb_reg_expr_str(mrb, str, s, len);
  mrb_str_buf_cat(mrb, str, "/", 1);
  if (re.tt) {
    char opts[4];
    mrb_reg_check(mrb, re);
    if (*option_to_str(opts, RREGEXP(re)->ptr->options))
        mrb_str_buf_cat(mrb, str, opts, strlen(opts));//mrb_str_buf_cat2(str, opts);
    if (mrb_basic(re)->flags & REG_ENCODING_NONE)
        mrb_str_buf_cat(mrb, str, "n", 1);
  }

  return str;
}
static void
mrb_reg_raise(mrb_state *mrb, const char *s, long len, const char *err, mrb_value re)
{
    mrb_value desc = mrb_reg_desc(mrb, s, len, re);

    mrb_raise(mrb, E_REGEXP_ERROR, "%s: %s", err, RSTRING_PTR(desc));
}

regex_t *
mrb_reg_prepare_re(mrb_state *mrb, mrb_value re, mrb_value str)
{
  regex_t *reg = RREGEXP(re)->ptr;
  onig_errmsg_buffer err = "";
  int r;
  OnigErrorInfo einfo;
  const char *pattern;
  mrb_value unescaped;
  mrb_encoding *enc = mrb_ascii8bit_encoding(mrb);

  mrb_reg_check(mrb, re);
  reg = RREGEXP(re)->ptr;
  pattern = RREGEXP_SRC_PTR(re);

  unescaped = mrb_reg_preprocess(mrb,
    pattern, pattern + RREGEXP(re)->src->len, err);

  if (mrb_nil_p(unescaped)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "regexp preprocess failed: %s", err);
  }

  r = onig_new(&reg, (UChar* )RSTRING_PTR(unescaped),
       (UChar* )(RSTRING_PTR(unescaped) + RSTRING_LEN(unescaped)),
       reg->options, enc,
       OnigDefaultSyntax, &einfo);
  if (r) {
    onig_error_code_to_str((UChar*)err, r, &einfo);
    mrb_reg_raise(mrb, pattern, RREGEXP_SRC_LEN(re), err, re);
  }

  //RB_GC_GUARD(unescaped);
  return reg;
}


mrb_int
mrb_reg_search(mrb_state *mrb, mrb_value re, mrb_value str, mrb_int pos, mrb_int reverse)
{
  long result;
  mrb_value match;
  struct re_registers regi, *regs = &regi;
  char *range = RSTRING_PTR(str);
  regex_t *reg;
  int tmpreg;

  if (pos > RSTRING_LEN(str) || pos < 0) {
    mrb_backref_set(mrb, mrb_nil_value());
    return -1;
  }

  reg = mrb_reg_prepare_re(mrb, re, str);
  tmpreg = reg != RREGEXP(re)->ptr;
  if (!tmpreg) RREGEXP(re)->usecnt++;

  match = mrb_backref_get(mrb);
  if (!mrb_nil_p(match)) {
    /*if (FL_TEST(match, MATCH_BUSY)) {
      match = Qnil;
    }
    else {
      regs = RMATCH_REGS(match);
    }*/
    regs = RMATCH_REGS(match);
  }
  if (mrb_nil_p(match)) {
    memset(regs, 0, sizeof(struct re_registers));
  }
//-->
  if (!reverse) {
    range += RSTRING_LEN(str);
  }
  result = onig_search(reg,
           (UChar*)(RSTRING_PTR(str)),
           ((UChar*)(RSTRING_PTR(str)) + RSTRING_LEN(str)),
           ((UChar*)(RSTRING_PTR(str)) + pos),
           ((UChar*)range),
           regs, ONIG_OPTION_NONE);
  if (!tmpreg) RREGEXP(re)->usecnt--;
  if (tmpreg) {
    if (RREGEXP(re)->usecnt) {
      onig_free(reg);
    }
    else {
        onig_free(RREGEXP(re)->ptr);
        RREGEXP(re)->ptr = reg;
    }
  }
  if (result < 0) {
    if (regs == &regi)
        onig_region_free(regs, 0);
    if (result == ONIG_MISMATCH) {
        mrb_backref_set(mrb, mrb_nil_value());
        return result;
    }
    else {
        onig_errmsg_buffer err = "";
        onig_error_code_to_str((UChar*)err, (int)result);
        mrb_reg_raise(mrb, RREGEXP_SRC_PTR(re), RREGEXP_SRC_LEN(re), err, re);
    }
  }
//--<
  if (mrb_nil_p(match) ) {
    match = match_alloc(mrb);
    onig_region_copy(RMATCH_REGS(match), regs);
    onig_region_free(regs, 0);
  }

  RMATCH(match)->str = mrb_str_ptr(str);
  RMATCH(match)->regexp = mrb_regex_ptr(re);
  RMATCH(match)->rmatch->char_offset_updated = 0;
  mrb_backref_set(mrb, match);

  return result;
}

mrb_int
mrb_reg_adjust_startpos(mrb_state *mrb, mrb_value re, mrb_value str, mrb_int pos, mrb_int reverse)
{
  mrb_int range;
  struct RString *s = mrb_str_ptr(str);
  struct RRegexp *r = mrb_regex_ptr(re);

  mrb_reg_check(mrb, re);
  /*if (may_need_recompile) mrb_reg_prepare_re(re);*/

  /* if (FL_TEST(re, KCODE_FIXED))
    mrb_kcode_set_option(re);
  else if (reg_kcode != curr_kcode)
    mrb_kcode_reset_option(); */

  if (reverse) {
    range = -pos;
  }
  else {
    range = s->len - pos;
  }
  return re_adjust_startpos(r->ptr,
            s->buf, s->len,
            pos, range);
}

static int
onig_new_with_source(regex_t** reg, const UChar* pattern, const UChar* pattern_end,
    OnigOptionType option, OnigEncoding enc, const OnigSyntaxType* syntax,
    OnigErrorInfo* einfo, const char *sourcefile, int sourceline)
{
  int r;

  *reg = (regex_t* )malloc/*xmalloc*/(sizeof(regex_t));
  if ((void*)(*reg) == (void*)0) return ONIGERR_MEMORY;

  r = onig_reg_init(*reg, option, ONIGENC_CASE_FOLD_DEFAULT, enc, syntax);
  if (r) goto err;
  r = onig_compile(*reg, pattern, pattern_end, einfo, sourcefile, sourceline);
  if (r) {
  err:
    onig_free(*reg);
    *reg = 0/*NULL*/;
  }
  return r;
}

static Regexp*
make_regexp(const char *s, long len, mrb_encoding *enc, int flags, onig_errmsg_buffer err,
  const char *sourcefile, int sourceline)
{
  Regexp *rp;
  int r;
  OnigErrorInfo einfo;

  /* Handle escaped characters first. */

  /* Build a copy of the string (in dest) with the
     escaped characters translated,  and generate the regex
     from that.
  */

  r = onig_new_with_source(&rp, (UChar*)s, (UChar*)(s + len), flags,
   enc, OnigDefaultSyntax, &einfo, sourcefile, sourceline);
  if (r) {
    onig_error_code_to_str((UChar*)err, r, &einfo);
    return 0;
  }
  return rp;
}

unsigned long
ruby_scan_hex(const char *start, size_t len, size_t *retlen)
{
    static const char hexdigit[] = "0123456789abcdef0123456789ABCDEF";
    register const char *s = start;
    register unsigned long retval = 0;
    const char *tmp;

    while (len-- && *s && (tmp = strchr(hexdigit, *s))) {
    retval <<= 4;
    retval |= (tmp - hexdigit) & 15;
    s++;
    }
    *retlen = (int)(s - start);    /* less than len */
    return retval;
}

#define BYTEWIDTH 8

int
mrb_uv_to_utf8(mrb_state *mrb, char buf[6], unsigned long uv)
{
  if (uv <= 0x7f) {
    buf[0] = (char)uv;
    return 1;
  }
  if (uv <= 0x7ff) {
    buf[0] = (char)((uv>>6)&0xff)|0xc0;
    buf[1] = (char)(uv&0x3f)|0x80;
    return 2;
  }
  if (uv <= 0xffff) {
    buf[0] = (char)((uv>>12)&0xff)|0xe0;
    buf[1] = (char)((uv>>6)&0x3f)|0x80;
    buf[2] = (char)(uv&0x3f)|0x80;
    return 3;
  }
  if (uv <= 0x1fffff) {
    buf[0] = (char)((uv>>18)&0xff)|0xf0;
    buf[1] = (char)((uv>>12)&0x3f)|0x80;
    buf[2] = (char)((uv>>6)&0x3f)|0x80;
    buf[3] = (char)(uv&0x3f)|0x80;
    return 4;
  }
  if (uv <= 0x3ffffff) {
    buf[0] = (char)((uv>>24)&0xff)|0xf8;
    buf[1] = (char)((uv>>18)&0x3f)|0x80;
    buf[2] = (char)((uv>>12)&0x3f)|0x80;
    buf[3] = (char)((uv>>6)&0x3f)|0x80;
    buf[4] = (char)(uv&0x3f)|0x80;
    return 5;
  }
  if (uv <= 0x7fffffff) {
    buf[0] = (char)((uv>>30)&0xff)|0xfc;
    buf[1] = (char)((uv>>24)&0x3f)|0x80;
    buf[2] = (char)((uv>>18)&0x3f)|0x80;
    buf[3] = (char)((uv>>12)&0x3f)|0x80;
    buf[4] = (char)((uv>>6)&0x3f)|0x80;
    buf[5] = (char)(uv&0x3f)|0x80;
    return 6;
  }
  mrb_raise(mrb, E_RANGE_ERROR, "pack(U): value out of range");
  return 0;
}

unsigned long
ruby_scan_oct(const char *start, size_t len, size_t *retlen)
{
    register const char *s = start;
    register unsigned long retval = 0;

    while (len-- && *s >= '0' && *s <= '7') {
    retval <<= 3;
    retval |= *s++ - '0';
    }
    *retlen = (int)(s - start);    /* less than len */
    return retval;
}

static mrb_value
mrb_reg_preprocess(mrb_state *mrb, const char *p, const char *end, onig_errmsg_buffer err)
{
  return mrb_nil_value();
}

static int
mrb_reg_initialize(mrb_state *mrb, mrb_value obj, const char *s, long len, 
      int options, onig_errmsg_buffer err,
      const char *sourcefile, int sourceline)
{
  struct RRegexp *re = RREGEXP(obj);
  mrb_value unescaped;
  mrb_encoding *enc = mrb_ascii8bit_encoding(mrb);
  if (re->ptr)
    mrb_raise(mrb, E_TYPE_ERROR, "already initialized regexp");
  re->ptr = 0;

  unescaped = mrb_reg_preprocess(mrb, s, s+len, err);
  if (mrb_nil_p(unescaped))
    return -1;

  if ((options & ARG_ENCODING_FIXED)) {
    //re->basic.flags |= KCODE_FIXED;
    re->flags|= KCODE_FIXED;
  }
  if (options & ARG_ENCODING_NONE) {
    re->flags |= REG_ENCODING_NONE;
  }

  re->ptr = make_regexp(RSTRING_PTR(unescaped), RSTRING_LEN(unescaped), enc,
      options & ARG_REG_OPTION_MASK, err,
      sourcefile, sourceline);
  if (!re->ptr) return -1;
  re->src = mrb_str_ptr(mrb_str_new(mrb, s, len));

  return 0;
}

static int
mrb_reg_initialize_str(mrb_state *mrb, mrb_value obj, mrb_value str, int options, onig_errmsg_buffer err,
    const char *sourcefile, int sourceline)
{
  int ret;

#if 0
  if (options & ARG_ENCODING_NONE) {
      mrb_encoding *ascii8bit = mrb_ascii8bit_encoding(mrb);
      if (enc != ascii8bit) {
          if (mrb_enc_str_coderange(mrb, str) != ENC_CODERANGE_7BIT) {
              //errcpy(err, "/.../n has a non escaped non ASCII character in non ASCII-8BIT script");
              printf("/.../n has a non escaped non ASCII character in non ASCII-8BIT script");
              return -1;
          }
          enc = ascii8bit;
      }
  }
#endif

  ret = mrb_reg_initialize(mrb, obj, RSTRING_PTR(str), RSTRING_LEN(str), 
        options, err, sourcefile, sourceline);

  return ret;
}

/* 15.2.15.7.1  */
/*
 *  call-seq:
 *     Regexp.initialize(string, [options [, lang]])        -> regexp
 *     Regexp.initialize(regexp)                            -> regexp
 *
 *  Constructs a new regular expression from <i>pattern</i>, which can be either
 *  a <code>String</code> or a <code>Regexp</code> (in which case that regexp's
 *  options are propagated, and new options may not be specified (a change as of
 *  Ruby 1.8). If <i>options</i> is a <code>Fixnum</code>, it should be one or
 *  more of the constants <code>Regexp::EXTENDED</code>,
 *  <code>Regexp::IGNORECASE</code>, and <code>Regexp::MULTILINE</code>,
 *  <em>or</em>-ed together. Otherwise, if <i>options</i> is not
 *  <code>nil</code>, the regexp will be case insensitive.
 *  When the <i>lang</i> parameter is `n' or `N' sets the regexp no encoding.
 *
 *     r1 = Regexp.initialize('^a-z+:\\s+\w+')           #=> /^a-z+:\s+\w+/
 *     r2 = Regexp.initialize('cat', true)               #=> /cat/i
 *     r3 = Regexp.initialize('dog', Regexp::EXTENDED)   #=> /dog/x
 *     r4 = Regexp.initialize(r2)                        #=> /cat/i
 */

static mrb_value
mrb_reg_initialize_m(mrb_state *mrb, /*int argc, mrb_value *argv,*/ mrb_value self)
{
  mrb_value argv[16];
  int argc;
  onig_errmsg_buffer err = "";
  int flags = 0;
  mrb_value str;
  const char *ptr;
  long len;

  mrb_get_args(mrb, "*", &argv, &argc);
  if (argc == 0 || argc > 3) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments (%d for 1..3)", argc);
  }
  if (mrb_type(argv[0]) ==  MRB_TT_REGEX) {
    mrb_value re = argv[0];

    if (argc > 1) {
      /* mrb_warn("flags ignored"); */
      printf("flags ignored");
    }
    mrb_reg_check(mrb, re);
    flags = mrb_reg_options(mrb, re);
    ptr = RREGEXP_SRC_PTR(re);
    len = RREGEXP_SRC_LEN(re);
    if (mrb_reg_initialize(mrb, self, ptr, len, flags, err, NULL, 0)) {
        printf("mrb_reg_raise_str(str, flags, err);");
    }
  }
  else {
    if (argc >= 2) {
      if (mrb_type(argv[1]) == MRB_TT_FIXNUM) flags = mrb_fixnum(argv[1]);
      else if (mrb_test(argv[1])) flags = ONIG_OPTION_IGNORECASE;
    }
    if (argc == 3 && !mrb_nil_p(argv[2])) {
      //char *kcode = StringValuePtr(argv[2]);
      char *kcode = mrb_string_value_ptr(mrb, argv[2]);
      if (kcode[0] == 'n' || kcode[0] == 'N') {
        flags |= ARG_ENCODING_NONE;
      }
      else {
        /*mrb_warn("encoding option is ignored - %s", kcode); */
        printf("mrb_warn:encoding option is ignored - %s", kcode);
      }
    }
    str = argv[0];
    //ptr = StringValuePtr(str);
    ptr = mrb_string_value_ptr(mrb, str);
    if (mrb_reg_initialize_str(mrb, self, str, flags, err, NULL, 0)) {
       //mrb_reg_raise_str(str, flags, err);
    }
  }
  return self;
}

/* 15.2.15.7.2  */
/* :nodoc: */
static mrb_value
mrb_reg_init_copy(mrb_state *mrb, mrb_value re/*, mrb_value copy*/)
{
  mrb_value argv[16];
  int argc;
  onig_errmsg_buffer err = "";
  const char *s;
  long len;
  mrb_value copy;

  mrb_get_args(mrb, "*", &argv, &argc);
  copy = argv[0];
  if (mrb_obj_equal(mrb, copy, re)) return copy;
  /*mrb_check_frozen(copy);*/
  /* need better argument type check */
  if (!mrb_obj_is_instance_of(mrb, re, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument type");
  }
  mrb_reg_check(mrb, copy);
  s = RREGEXP_SRC_PTR(copy);
  len = RREGEXP_SRC_LEN(copy);
  if (mrb_reg_initialize(mrb, re, s, len, mrb_reg_options(mrb, copy),
      err, 0/*NULL*/, 0) != 0) {
    mrb_reg_raise(mrb, s, len, err, re);
  }
  return re;
}

static int
reg_equal(mrb_state *mrb, struct RRegexp *re1, struct RRegexp *re2)
{
  if (re1->ptr->options != re2->ptr->options) return FALSE;
  if (!mrb_equal(mrb, mrb_obj_value(re1->src), mrb_obj_value(re2->src)))
    return FALSE;
  return TRUE;
}

static int
mrb_reg_equal(mrb_state *mrb, mrb_value re1, mrb_value re2)
{
  if (mrb_obj_equal(mrb, re1, re2)) return TRUE;

  if (mrb_type(re2) != MRB_TT_REGEX) return FALSE;
  mrb_reg_check(mrb, re1);
  mrb_reg_check(mrb, re2);
  return reg_equal(mrb, RREGEXP(re1), RREGEXP(re2));
}

/* 15.2.15.7.3  */
/*
 *  call-seq:
 *     rxp == other_rxp      -> true or false
 *     rxp.eql?(other_rxp)   -> true or false
 *
 *  Equality---Two regexps are equal if their patterns are identical, they have
 *  the same character set code, and their <code>casefold?</code> values are the
 *  same.
 *
 *     /abc/  == /abc/x   #=> false
 *     /abc/  == /abc/i   #=> false
 *     /abc/  == /abc/n   #=> false
 *     /abc/u == /abc/n   #=> false
 */

static mrb_value
mrb_reg_equal_m(mrb_state *mrb, mrb_value re1/*, mrb_value re2*/)
{
  mrb_value re2;

  mrb_get_args(mrb, "o", &re2);
  if (mrb_reg_equal(mrb, re1, re2))
    return mrb_true_value();
  return mrb_false_value();
}

/* 15.2.15.7.4  */
/*
 *  call-seq:
 *     rxp === str   -> true or false
 *
 *  Case Equality---Synonym for <code>Regexp#=~</code> used in case statements.
 *
 *     a = "HELLO"
 *     case a
 *     when /^[a-z]*$/; print "Lower case\n"
 *     when /^[A-Z]*$/; print "Upper case\n"
 *     else;            print "Mixed case\n"
 *     end
 *
 *  <em>produces:</em>
 *
 *     Upper case
 */

mrb_value
mrb_reg_eqq(mrb_state *mrb, mrb_value re/*, mrb_value str*/)
{
  long start;
  mrb_value str;

  mrb_get_args(mrb, "o", &str);
  str = reg_operand(mrb, str, 0/*FALSE*/);
  if (mrb_nil_p(str)) {
    mrb_backref_set(mrb, mrb_nil_value());
    return mrb_false_value();
  }
  start = mrb_reg_search(mrb, re, str, 0, 0);
  if (start < 0) {
    return mrb_false_value();
  }
  return mrb_true_value();
}

static long
reg_match_pos(mrb_state *mrb, mrb_value re, mrb_value *strp, long pos)
{
  mrb_value str = *strp;

  if (mrb_nil_p(str)) {
    mrb_backref_set(mrb, mrb_nil_value());
    return -1;
  }
  *strp = str = reg_operand(mrb, str, 1/*TRUE*/);
  if (pos != 0) {
    if (pos < 0) {
      mrb_value l = mrb_str_size(mrb, str);
      pos += mrb_fixnum(l);
      if (pos < 0) {
        return pos;
      }
    }
    pos = mrb_str_offset(mrb, str, pos);
  }
  return mrb_reg_search(mrb, re, str, pos, 0);
}

mrb_value
mrb_reg_match_str(mrb_state *mrb, mrb_value re, mrb_value str)
{
  mrb_int pos = reg_match_pos(mrb, re, &str, 0);
  if (pos < 0) return mrb_nil_value();
  pos = mrb_str_sublen(mrb, str, pos);
  return mrb_fixnum_value(pos);
}
/* 15.2.15.7.5  */
/*
 *  call-seq:
 *     rxp =~ str    -> integer or nil
 *
 *  Match---Matches <i>rxp</i> against <i>str</i>.
 *
 *     /at/ =~ "input data"   #=> 7
 *     /ax/ =~ "input data"   #=> nil
 *
 *  If <code>=~</code> is used with a regexp literal with named captures,
 *  captured strings (or nil) is assigned to local variables named by
 *  the capture names.
 *
 *     /(?<lhs>\w+)\s*=\s*(?<rhs>\w+)/ =~ "  x = y  "
 *     p lhs    #=> "x"
 *     p rhs    #=> "y"
 *
 *  If it is not matched, nil is assigned for the variables.
 *
 *     /(?<lhs>\w+)\s*=\s*(?<rhs>\w+)/ =~ "  x = "
 *     p lhs    #=> nil
 *     p rhs    #=> nil
 *
 *  This assignment is implemented in the Ruby parser.
 *  The parser detects 'regexp-literal =~ expression' for the assignment.
 *  The regexp must be a literal without interpolation and placed at left hand side.
 *
 *  The assignment is not occur if the regexp is not a literal.
 *
 *     re = /(?<lhs>\w+)\s*=\s*(?<rhs>\w+)/
 *     re =~ "  x = y  "
 *     p lhs    # undefined local variable
 *     p rhs    # undefined local variable
 *
 *  A regexp interpolation, <code>#{}</code>, also disables
 *  the assignment.
 *
 *     rhs_pat = /(?<rhs>\w+)/
 *     /(?<lhs>\w+)\s*=\s*#{rhs_pat}/ =~ "x = y"
 *     p lhs    # undefined local variable
 *
 *  The assignment is not occur if the regexp is placed at right hand side.
 *
 *    "  x = y  " =~ /(?<lhs>\w+)\s*=\s*(?<rhs>\w+)/
 *    p lhs, rhs # undefined local variable
 *
 */
mrb_value
mrb_reg_match(mrb_state *mrb, mrb_value re/*, mrb_value str*/)
{
  mrb_value str;

  mrb_get_args(mrb, "o", &str);
  return mrb_reg_match_str(mrb, re, str);
}

/* 15.2.15.7.6  */
/*
 *  call-seq:
 *     rxp.casefold?   -> true or false
 *
 *  Returns the value of the case-insensitive flag.
 *
 *      /a/.casefold?           #=> false
 *      /a/i.casefold?          #=> true
 *      /(?i:a)/.casefold?      #=> false
 */

static mrb_value
mrb_reg_casefold_p(mrb_state *mrb, mrb_value re)
{
  mrb_reg_check(mrb, re);
  if (RREGEXP(re)->ptr->options & ONIG_OPTION_IGNORECASE) return mrb_true_value();
  return mrb_false_value();
}

/* 15.2.15.7.7  */
/*
 *  call-seq:
 *     rxp.match(str)       -> matchdata or nil
 *     rxp.match(str,pos)   -> matchdata or nil
 *
 *  Returns a <code>MatchData</code> object describing the match, or
 *  <code>nil</code> if there was no match. This is equivalent to retrieving the
 *  value of the special variable <code>$~</code> following a normal match.
 *  If the second parameter is present, it specifies the position in the string
 *  to begin the search.
 *
 *     /(.)(.)(.)/.match("abc")[2]   #=> "b"
 *     /(.)(.)/.match("abc", 1)[2]   #=> "c"
 *
 *  If a block is given, invoke the block with MatchData if match succeed, so
 *  that you can write
 *
 *     pat.match(str) {|m| ...}
 *
 *  instead of
 *
 *     if m = pat.match(str)
 *       ...
 *     end
 *
 *  The return value is a value from block execution in this case.
 */

static mrb_value
mrb_reg_match_m(mrb_state *mrb, /*int argc, mrb_value *argv,*/ mrb_value re)
{
  mrb_value argv[16];
  int argc;
  mrb_value result, str, initpos, b;
  long pos;

  //if (mrb_scan_args(argc, argv, "11", &str, &initpos) == 2) {
  mrb_get_args(mrb, "*&", &argv, &argc, &b);
  if (argc == 2) {
    initpos = argv[1];
    pos = mrb_fixnum(initpos);
  }
  else {
    pos = 0;
  }
  str = argv[0];
  pos = reg_match_pos(mrb, re, &str, pos);
  if (pos < 0) {
    mrb_backref_set(mrb, mrb_nil_value());
    return mrb_nil_value();
  }
  result = mrb_backref_get(mrb);
  /*mrb_match_busy(result);*/
  if (!mrb_nil_p(result) && mrb_block_given_p()) {
    return mrb_yield(mrb, result, b);
  }
  return result;
}

/* 15.2.15.7.8  */

/*
 *  call-seq:
 *      rxp.source   -> str
 *
 *  Returns the original string of the pattern.
 *
 *      /ab+c/ix.source #=> "ab+c"
 *
 *  Note that escape sequences are retained as is.
 *
 *     /\x20\+/.source  #=> "\\x20\\+"
 *
 */

static mrb_value
mrb_reg_source(mrb_state *mrb, mrb_value re)
{
    mrb_value str;

    mrb_reg_check(mrb, re);
    str = mrb_str_new(mrb, RREGEXP_SRC_PTR(re),RREGEXP_SRC_LEN(re));
    return str;
}

static int
name_to_backref_number(mrb_state *mrb, struct re_registers *regs, struct RRegexp*regexp, const char* name, const char* name_end)
{
  int num;

  num = onig_name_to_backref_number(regexp->ptr,
           (const unsigned char* )name, (const unsigned char* )name_end, regs);
  if (num >= 1) {
    return num;
  }
  else {
    mrb_value s = mrb_str_new(mrb, name, (long )(name_end - name));//mrb_str_new(name, (long )(name_end - name));
    mrb_raise(mrb, E_INDEX_ERROR, "undefined group name reference: %s",
        mrb_string_value_ptr(mrb, s));
    return num; /* not reach */
  }
}

/*
 *  Document-class: MatchData
 *
 *  <code>MatchData</code> is the type of the special variable <code>$~</code>,
 *  and is the type of the object returned by <code>Regexp#match</code> and
 *  <code>Regexp.last_match</code>. It encapsulates all the results of a pattern
 *  match, results normally accessed through the special variables
 *  <code>$&</code>, <code>$'</code>, <code>$`</code>, <code>$1</code>,
 *  <code>$2</code>, and so on.
 *
 */

mrb_value
match_alloc(mrb_state *mrb)
{
  struct RMatch* m;

  m = (struct RMatch*)mrb_obj_alloc(mrb, MRB_TT_MATCH, MATCH_CLASS);

  m->str    = 0;
  m->rmatch = 0;
  m->regexp = 0;
  m->rmatch = mrb_malloc(mrb, sizeof(struct rmatch));//ALLOC(struct rmatch);
  memset(m->rmatch, 0, sizeof(struct rmatch));

  return mrb_obj_value(m);
}

/* ------------------------------------------------------------------------- */
/* MatchData Class */
/* ------------------------------------------------------------------------- */
/* 15.2.16.3.1  */
/*
 *  call-seq:
 *     mtch[i]               -> str or nil
 *     mtch[start, length]   -> array
 *     mtch[range]           -> array
 *     mtch[name]            -> str or nil
 *
 *  Match Reference---<code>MatchData</code> acts as an array, and may be
 *  accessed using the normal array indexing techniques.  <i>mtch</i>[0] is
 *  equivalent to the special variable <code>$&</code>, and returns the entire
 *  matched string.  <i>mtch</i>[1], <i>mtch</i>[2], and so on return the values
 *  of the matched backreferences (portions of the pattern between parentheses).
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m          #=> #<MatchData "HX1138" 1:"H" 2:"X" 3:"113" 4:"8">
 *     m[0]       #=> "HX1138"
 *     m[1, 2]    #=> ["H", "X"]
 *     m[1..3]    #=> ["H", "X", "113"]
 *     m[-3, 2]   #=> ["X", "113"]
 *
 *     m = /(?<foo>a+)b/.match("ccaaab")
 *     m          #=> #<MatchData "aaab" foo:"aaa">
 *     m["foo"]   #=> "aaa"
 *     m[:foo]    #=> "aaa"
 */

static mrb_value
mrb_match_aref(mrb_state *mrb, /*int argc, mrb_value *argv,*/ mrb_value match)
{
  mrb_value argv[16];
  int argc;
  mrb_value idx;

  match_check(mrb, match);
  //mrb_scan_args(argc, argv, "11", &idx, &rest);
  mrb_get_args(mrb, "*", &argv, &argc);
  idx = argv[0];
  if (argc<2) {
    if (mrb_type(idx) == MRB_TT_FIXNUM) {
      if (mrb_fixnum(idx) >= 0) {
        return mrb_reg_nth_match(mrb, mrb_fixnum(idx), match);
      }
    }
    else {
      const char *p;
      int num;

      switch (mrb_type(idx)) {
        case MRB_TT_SYMBOL:
          p = mrb_sym2name(mrb, mrb_symbol(idx));
          goto name_to_backref;
          break;
        case MRB_TT_STRING:
          //p = StringValuePtr(idx);
          p = mrb_string_value_ptr(mrb, idx);
name_to_backref:
          num = name_to_backref_number(mrb, RMATCH_REGS(match),
               RMATCH(match)->regexp, p, p + strlen(p));
          return mrb_reg_nth_match(mrb, num, match);
          break;
        default:
          break;
      }
    }
  }

  return mrb_ary_aget(mrb, /*argc, argv,*/ mrb_match_to_a(mrb, match));
}

typedef struct {
    long byte_pos;
    long char_pos;
} pair_t;

static void
update_char_offset(mrb_state *mrb, mrb_value match)
{
    struct rmatch *rm = RMATCH(match)->rmatch;
    struct re_registers *regs;
    int i, num_regs;

    if (rm->char_offset_updated)
        return;

    regs = &rm->regs;
    num_regs = rm->regs.num_regs;

    if (rm->char_offset_num_allocated < num_regs) {
        //REALLOC_N(rm->char_offset, struct rmatch_offset, num_regs);
        rm->char_offset = mrb_realloc(mrb, rm->char_offset, sizeof(struct rmatch_offset)*num_regs);
        rm->char_offset_num_allocated = num_regs;
    }

    for (i = 0; i < num_regs; i++) {
      rm->char_offset[i].beg = BEG(i);
      rm->char_offset[i].end = END(i);
    }
    rm->char_offset_updated = 1;
    return;
}

/* 15.2.16.3.2  */
/*
 *  call-seq:
 *     mtch.begin(n)   -> integer
 *
 *  Returns the offset of the start of the <em>n</em>th element of the match
 *  array in the string.
 *  <em>n</em> can be a string or symbol to reference a named capture.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.begin(0)       #=> 1
 *     m.begin(2)       #=> 2
 *
 *     m = /(?<foo>.)(.)(?<bar>.)/.match("hoge")
 *     p m.begin(:foo)  #=> 0
 *     p m.begin(:bar)  #=> 2
 */

static mrb_value
mrb_match_begin(mrb_state *mrb, mrb_value match/*, mrb_value n*/)
{
  mrb_value argv[16];
  int argc;
  mrb_value n = argv[0];
  int i;
  struct re_registers *regs;

  match_check(mrb, match);
  mrb_get_args(mrb, "*", &argv, &argc);
  n = argv[0];
  i = match_backref_number(mrb, match, n);
  regs = RMATCH_REGS(match);

  if (i < 0 || regs->num_regs <= i)
    mrb_raise(mrb, E_INDEX_ERROR, "index %d out of matches", i);

  if (BEG(i) < 0)
    return mrb_nil_value();

  update_char_offset(mrb, match);
  return mrb_fixnum_value(RMATCH(match)->rmatch->char_offset[i].beg);
}

static mrb_value
match_array(mrb_state *mrb, mrb_value match, int start)
{
  struct re_registers *regs;
  mrb_value ary;
  struct RString *target;
  int i;

  match_check(mrb, match);
  regs = RMATCH_REGS(match);
  ary = mrb_ary_new_capa(mrb, regs->num_regs);//mrb_ary_new2(regs->num_regs);
  target = RMATCH(match)->str;

  for (i=start; i<regs->num_regs; i++) {
    if (regs->beg[i] == -1) {
      mrb_ary_push(mrb, ary, mrb_nil_value());
    }
    else {
      mrb_value str = mrb_str_subseq(mrb, mrb_obj_value(target), regs->beg[i], regs->end[i]-regs->beg[i]);
      mrb_ary_push(mrb, ary, str);
    }
  }
  return ary;
}

/* 15.2.16.3.3  */
/*
 *  call-seq:
 *     mtch.captures   -> array
 *
 *  Returns the array of captures; equivalent to <code>mtch.to_a[1..-1]</code>.
 *
 *     f1,f2,f3,f4 = /(.)(.)(\d+)(\d)/.match("THX1138.").captures
 *     f1    #=> "H"
 *     f2    #=> "X"
 *     f3    #=> "113"
 *     f4    #=> "8"
 */
static mrb_value
mrb_match_captures(mrb_state *mrb, mrb_value match)
{
    return match_array(mrb, match, 1);
}

/* 15.2.16.3.4  */
/*
 *  call-seq:
 *     mtch.end(n)   -> integer
 *
 *  Returns the offset of the character immediately following the end of the
 *  <em>n</em>th element of the match array in the string.
 *  <em>n</em> can be a string or symbol to reference a named capture.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.end(0)         #=> 7
 *     m.end(2)         #=> 3
 *
 *     m = /(?<foo>.)(.)(?<bar>.)/.match("hoge")
 *     p m.end(:foo)    #=> 1
 *     p m.end(:bar)    #=> 3
 */

static mrb_value
mrb_match_end(mrb_state *mrb, mrb_value match/*, mrb_value n*/)
{
  mrb_value argv[16];
  int argc;
  mrb_value n;
  int i;
  struct re_registers *regs;

  match_check(mrb, match);
  mrb_get_args(mrb, "*", &argv, &argc);
  n = argv[0];
  i = match_backref_number(mrb, match, n);
  regs = RMATCH_REGS(match);

  if (i < 0 || regs->num_regs <= i)
    mrb_raise(mrb, E_INDEX_ERROR, "index %d out of matches", i);

  if (BEG(i) < 0)
    return mrb_nil_value();

  update_char_offset(mrb, match);
  return mrb_fixnum_value(RMATCH(match)->rmatch->char_offset[i].end);
}

/* 15.2.16.3.5  */
/* :nodoc: */
static mrb_value
mrb_match_init_copy(mrb_state *mrb, mrb_value obj/*, mrb_value orig*/)
{
  mrb_value argv[16];
  int argc;
  struct rmatch *rm;
  mrb_value orig;

  mrb_get_args(mrb, "*", &argv, &argc);
  orig = argv[0];

  if (mrb_obj_equal(mrb, obj, orig)) return obj;

  if (!mrb_obj_is_instance_of(mrb, orig, mrb_obj_class(mrb, obj))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }

  RMATCH(obj)->str = RMATCH(orig)->str;
  RMATCH(obj)->regexp = RMATCH(orig)->regexp;

  if (RMATCH(obj)->rmatch == 0) {
      RMATCH(obj)->rmatch = mrb_malloc(mrb, sizeof(struct rmatch));//ALLOC(struct rmatch);
      memset(RMATCH(obj)->rmatch, 0, sizeof(struct rmatch));
  }
  rm = RMATCH(obj)->rmatch;
  onig_region_copy(&rm->regs, RMATCH_REGS(orig));

  if (!RMATCH(orig)->rmatch->char_offset_updated) {
      rm->char_offset_updated = 0;
  }
  else {
      if (rm->char_offset_num_allocated < rm->regs.num_regs) {
          //REALLOC_N(rm->char_offset, struct rmatch_offset, rm->regs.num_regs);
          rm->char_offset = mrb_realloc(mrb, rm->char_offset, sizeof(struct rmatch_offset)* rm->regs.num_regs);
          rm->char_offset_num_allocated = rm->regs.num_regs;
      }
      memcpy(rm->char_offset, RMATCH(orig)->rmatch->char_offset,
             sizeof(struct rmatch_offset) * rm->regs.num_regs);
      rm->char_offset_updated = 1;
  }

  return obj;
}

/* 15.2.16.3.6  */
/* 15.2.16.3.10 */
/*
 *  call-seq:
 *     mtch.length   -> integer
 *     mtch.size     -> integer
 *
 *  Returns the number of elements in the match array.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.length   #=> 5
 *     m.size     #=> 5
 */

static mrb_value
mrb_match_size(mrb_state *mrb, mrb_value match)
{
    match_check(mrb, match);
    return mrb_fixnum_value(RMATCH_REGS(match)->num_regs);
}

/* 15.2.16.3.7  */
/*
 *  call-seq:
 *     mtch.offset(n)   -> array
 *
 *  Returns a two-element array containing the beginning and ending offsets of
 *  the <em>n</em>th match.
 *  <em>n</em> can be a string or symbol to reference a named capture.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.offset(0)      #=> [1, 7]
 *     m.offset(4)      #=> [6, 7]
 *
 *     m = /(?<foo>.)(.)(?<bar>.)/.match("hoge")
 *     p m.offset(:foo) #=> [0, 1]
 *     p m.offset(:bar) #=> [2, 3]
 *
 */

static mrb_value
mrb_match_offset(mrb_state *mrb, mrb_value match/*, mrb_value n*/)
{
  mrb_value n;
  struct re_registers *regs = RMATCH_REGS(match);
  int i;

  match_check(mrb, match);
  mrb_get_args(mrb, "o", &n);
  i = match_backref_number(mrb, match, n);

  if (i < 0 || regs->num_regs <= i)
    mrb_raise(mrb, E_INDEX_ERROR, "index %d out of matches", i);

  if (BEG(i) < 0)
    return mrb_assoc_new(mrb, mrb_nil_value(), mrb_nil_value());

  update_char_offset(mrb, match);
  return mrb_assoc_new(mrb, mrb_fixnum_value(RMATCH(match)->rmatch->char_offset[i].beg),
      mrb_fixnum_value(RMATCH(match)->rmatch->char_offset[i].end));
}

/* 15.2.16.3.8  */
/*
 *  call-seq:
 *     mtch.post_match   -> str
 *
 *  Returns the portion of the original string after the current match.
 *  Equivalent to the special variable <code>$'</code>.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138: The Movie")
 *     m.post_match   #=> ": The Movie"
 */
mrb_value
mrb_reg_match_post(mrb_state *mrb, mrb_value match)
{
  struct RString *str;
  long pos;
  struct re_registers *regs;

  if (mrb_nil_p(match)) return mrb_nil_value();
  match_check(mrb, match);
  regs = RMATCH_REGS(match);
  if (BEG(0) == -1) return mrb_nil_value();
  str = RMATCH(match)->str;
  pos = END(0);
  return mrb_str_subseq(mrb, mrb_obj_value(str), pos, str->len - pos);
}

/* 15.2.16.3.9  */
/*
 *  call-seq:
 *     mtch.pre_match   -> str
 *
 *  Returns the portion of the original string before the current match.
 *  Equivalent to the special variable <code>$`</code>.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.pre_match   #=> "T"
 */

mrb_value
mrb_reg_match_pre(mrb_state *mrb, mrb_value match)
{
  mrb_value str;
  struct re_registers *regs;

  if (mrb_nil_p(match)) return mrb_nil_value();
  match_check(mrb, match);
  regs = RMATCH_REGS(match);
  if (BEG(0) == -1) return mrb_nil_value();
  str = mrb_str_subseq(mrb, mrb_obj_value(RMATCH(match)->str), 0, BEG(0));

  return str;
}

/* 15.2.16.3.11 */
/*
 *  call-seq:
 *     mtch.string   -> str
 *
 *  Returns a frozen copy of the string passed in to <code>match</code>.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.string   #=> "THX1138."
 */

static mrb_value
mrb_match_string(mrb_state *mrb, mrb_value match)
{
    match_check(mrb, match);
    return mrb_obj_value(RMATCH(match)->str);
}

/* 15.2.16.3.12 */
/*
 *  call-seq:
 *     mtch.to_a   -> anArray
 *
 *  Returns the array of matches.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.to_a   #=> ["HX1138", "H", "X", "113", "8"]
 *
 *  Because <code>to_a</code> is called when expanding
 *  <code>*</code><em>variable</em>, there's a useful assignment
 *  shortcut for extracting matched fields. This is slightly slower than
 *  accessing the fields directly (as an intermediate array is
 *  generated).
 *
 *     all,f1,f2,f3 = *(/(.)(.)(\d+)(\d)/.match("THX1138."))
 *     all   #=> "HX1138"
 *     f1    #=> "H"
 *     f2    #=> "X"
 *     f3    #=> "113"
 */

static mrb_value
mrb_match_to_a(mrb_state *mrb, mrb_value match)
{
    return match_array(mrb, match, 0);
}

/* 15.2.16.3.13 */
/*
 *  call-seq:
 *     mtch.to_s   -> str
 *
 *  Returns the entire matched string.
 *
 *     m = /(.)(.)(\d+)(\d)/.match("THX1138.")
 *     m.to_s   #=> "HX1138"
 */

static mrb_value
mrb_match_to_s(mrb_state *mrb, mrb_value match)
{
    mrb_value str = mrb_reg_last_match(mrb, match);

    match_check(mrb, match);
    if (mrb_nil_p(str)) str = mrb_str_new(mrb, 0, 0);//mrb_str_new(0,0);

    return str;
}

static int
char_to_option(int c)
{
  int val;

  switch (c) {
    case 'i':
      val = ONIG_OPTION_IGNORECASE;
      break;
    case 'x':
      val = ONIG_OPTION_EXTEND;
      break;
    case 'm':
      val = ONIG_OPTION_MULTILINE;
      break;
    default:
      val = 0;
      break;
  }
  return val;
}

static char *
option_to_str(char str[4], int options)
{
  char *p = str;
  if (options & ONIG_OPTION_MULTILINE) *p++ = 'm';
  if (options & ONIG_OPTION_IGNORECASE) *p++ = 'i';
  if (options & ONIG_OPTION_EXTEND) *p++ = 'x';
  *p = 0;
  return str;
}

#define CHAR_ESC_LEN 13 /* sizeof(\x{ hex of 32bit unsigned int } \0) */

static void
mrb_reg_expr_str(mrb_state *mrb, mrb_value str, const char *s, long len)
{
  const char *p, *pend;
  int need_escape = 0;
  int c;

  p = s; pend = p + len;
  while (p < pend) {
    c = *p;
    if (c == -1) {
      p += pend - p;
    }
    else if (c != '/' && ISPRINT(c)) {
      p++;
    }
    else {
      need_escape = 1;
      break;
    }
  }

  if (!need_escape) {
    mrb_str_buf_cat(mrb, str, s, len);
  }
  else {
    p = s;
    while (p<pend) {
      c = *p;
      if (c == '\\' && p+1 < pend) {
        int n = 1 + pend - (p+1);
        mrb_str_buf_cat(mrb, str, p, n);
        p += n;
        continue;
      }
      else if (c == '/') {
        char c = '\\';
        mrb_str_buf_cat(mrb, str, &c, 1);
        mrb_str_buf_cat(mrb, str, p, 1);
      }
      else if (ISPRINT(c)) {
        mrb_str_buf_cat(mrb, str, p, 1);
      }
      else if (!ISSPACE(c)) {
        char b[8];
	int n;

        n = snprintf(b, sizeof(b), "\\x%02X", c);
        mrb_str_buf_cat(mrb, str, b, n);
      }
      else {
        mrb_str_buf_cat(mrb, str, p, 1);
      }
      p++;
    }
  }
}

/* 15.2.15.7.9 (x) */
/*
 *  call-seq:
 *     rxp.to_s   -> str
 *
 *  Returns a string containing the regular expression and its options (using the
 *  <code>(?opts:source)</code> notation. This string can be fed back in to
 *  <code>Regexp::new</code> to a regular expression with the same semantics as
 *  the original. (However, <code>Regexp#==</code> may not return true when
 *  comparing the two, as the source of the regular expression itself may
 *  differ, as the example shows).  <code>Regexp#inspect</code> produces a
 *  generally more readable version of <i>rxp</i>.
 *
 *      r1 = /ab+c/ix           #=> /ab+c/ix
 *      s1 = r1.to_s            #=> "(?ix-m:ab+c)"
 *      r2 = Regexp.new(s1)     #=> /(?ix-m:ab+c)/
 *      r1 == r2                #=> false
 *      r1.source               #=> "ab+c"
 *      r2.source               #=> "(?ix-m:ab+c)"
 */

mrb_value
mrb_reg_to_s(mrb_state *mrb, mrb_value re)
{
  int options, opt;
  const int embeddable = ONIG_OPTION_MULTILINE|ONIG_OPTION_IGNORECASE|ONIG_OPTION_EXTEND;
  long len;
  const UChar* ptr;
  mrb_value str = mrb_str_new(mrb, "(?", 2);
  char optbuf[5];
  mrb_encoding *enc = mrb_enc_get(mrb, re);

  mrb_reg_check(mrb, re);
  memset(optbuf, 0, 5);
  options = RREGEXP(re)->ptr->options;
  ptr = (UChar*)RREGEXP_SRC_PTR(re);
  len = RREGEXP_SRC_LEN(re);
again:
  if (len >= 4 && ptr[0] == '(' && ptr[1] == '?') {
    int err = 1;
    ptr += 2;
    if ((len -= 2) > 0) {
        do {
                opt = char_to_option((int )*ptr);
                if (opt != 0) {
                    options |= opt;
                }
                else {
                    break;
                }
        ++ptr;
        } while (--len > 0);
    }
    if (len > 1 && *ptr == '-') {
        ++ptr;
        --len;
        do {
                opt = char_to_option((int )*ptr);
                if (opt != 0) {
                    options &= ~opt;
                }
                else {
                    break;
                }
        ++ptr;
        } while (--len > 0);
    }
    if (*ptr == ')') {
        --len;
        ++ptr;
        goto again;
    }
    if (*ptr == ':' && ptr[len-1] == ')') {
        Regexp *rp;

        ++ptr;
        len -= 2;
	err = onig_new(&rp, ptr, ptr + len, ONIG_OPTION_DEFAULT,
               enc, OnigDefaultSyntax, NULL);
        onig_free(rp);
    }
    if (err) {
        options = RREGEXP(re)->ptr->options;
        ptr = (UChar*)RREGEXP_SRC_PTR(re);
        len = RREGEXP_SRC_LEN(re);
    }
  }

  if (*option_to_str(optbuf, options)) mrb_str_buf_cat(mrb, str, optbuf, strlen(optbuf));

  if ((options & embeddable) != embeddable) {
    optbuf[0] = '-';
    option_to_str(optbuf + 1, ~options);
    mrb_str_buf_cat(mrb, str, optbuf, strlen(optbuf));
  }

  mrb_str_buf_cat(mrb, str, ":", 1);
  mrb_reg_expr_str(mrb, str, (char*)ptr, len);
  mrb_str_buf_cat(mrb, str, ")", 1);

  return str;
}

/* 15.2.15.7.10(x) */
/*
 * call-seq:
 *    rxp.inspect   -> string
 *
 * Produce a nicely formatted string-version of _rxp_. Perhaps surprisingly,
 * <code>#inspect</code> actually produces the more natural version of
 * the string than <code>#to_s</code>.
 *
 *      /ab+c/ix.inspect        #=> "/ab+c/ix"
 *
 */

static mrb_value
mrb_reg_inspect(mrb_state *mrb, mrb_value re)
{
  if (!RREGEXP(re)->ptr || !RREGEXP_SRC(re) || !RREGEXP_SRC_PTR(re)) {
      return mrb_any_to_s(mrb, re);
  }
  return mrb_reg_desc(mrb, RREGEXP_SRC_PTR(re), RREGEXP_SRC_LEN(re), re);
}

static mrb_value
mrb_reg_s_alloc(mrb_state *mrb, mrb_value dummy)
{
  struct RRegexp* re;

  //NEWOBJ(re, struct RRegexp);
  //OBJSETUP(re, klass, T_REGEXP);
  re = (struct RRegexp*)mrb_obj_alloc(mrb, MRB_TT_REGEX, REGEX_CLASS);

  re->ptr = 0;
  re->src = 0;
  re->usecnt = 0;

  return mrb_obj_value(re);
}

mrb_value
mrb_reg_match_last(mrb_state *mrb, mrb_value match)
{
    int i;

    if (mrb_nil_p(match)) return mrb_nil_value();
    match_check(mrb, match);
    if (RMATCH(match)->rmatch->char_offset[0].beg == -1) return mrb_nil_value();

    for (i=RMATCH(match)->rmatch->regs.num_regs-1; RMATCH(match)->rmatch->char_offset[i].beg == -1 && i > 0; i--)
        ;
    if (i == 0) return mrb_nil_value();
    return mrb_reg_nth_match(mrb, i, match);
}

/* 15.2.16.3.14(x) */
/*
 * call-seq:
 *    mtch.inspect   -> str
 *
 * Returns a printable version of <i>mtch</i>.
 *
 *     puts /.$/.match("foo").inspect
 *     #=> #<MatchData "o">
 *
 *     puts /(.)(.)(.)/.match("foo").inspect
 *     #=> #<MatchData "foo" 1:"f" 2:"o" 3:"o">
 *
 *     puts /(.)(.)?(.)/.match("fo").inspect
 *     #=> #<MatchData "fo" 1:"f" 2:nil 3:"o">
 *
 *     puts /(?<foo>.)(?<bar>.)(?<baz>.)/.match("hoge").inspect
 *     #=> #<MatchData "hog" foo:"h" bar:"o" baz:"g">
 *
 */
struct backref_name_tag {
    const UChar *name;
    long len;
};

static int
match_inspect_name_iter(const OnigUChar *name, const OnigUChar *name_end,
          int back_num, int *back_refs, OnigRegex regex, void *arg0)
{
    struct backref_name_tag *arg = (struct backref_name_tag*)arg0;
    int i;

    for (i = 0; i < back_num; i++) {
        arg[back_refs[i]].name = name;
        arg[back_refs[i]].len = name_end - name;
    }
    return 0;
}

static mrb_value
mrb_match_inspect(mrb_state *mrb, mrb_value match)
{
    const char *cname = mrb_obj_classname(mrb, match);
    mrb_value str;
    int i;
    struct re_registers *regs = RMATCH_REGS(match);
    int num_regs = regs->num_regs;
    struct backref_name_tag *names;
    struct RRegexp *regexp = RMATCH(match)->regexp;

    if (!regexp) {
        return mrb_sprintf(mrb, "#<%s:%p>", cname, (void*)&match);
    }

    //names = ALLOCA_N(struct backref_name_tag, num_regs);
    //MEMZERO(names, struct backref_name_tag, num_regs);
    names = mrb_malloc(mrb, sizeof(struct backref_name_tag)*num_regs);
    memset(names, 0, sizeof(struct backref_name_tag)*num_regs);

    onig_foreach_name(regexp->ptr,
            match_inspect_name_iter, names);

    str = mrb_str_new(mrb, "#<", 2);
    mrb_str_buf_cat(mrb, str, cname, strlen(cname));

    for (i = 0; i < num_regs; i++) {
        char buf[sizeof(num_regs)*3+1];
        mrb_value v;
        mrb_str_buf_cat(mrb, str, " ", 1);
        if (0 < i) {
            if (names[i].name)
                mrb_str_buf_cat(mrb, str, (const char*)names[i].name, names[i].len);
            else {
	      int n = sprintf(buf, "%d", i);
                mrb_str_buf_cat(mrb, str, (const char*)buf, n);
            }
            mrb_str_buf_cat(mrb, str, ":", 1);
        }
        v = mrb_reg_nth_match(mrb, i, match);
        if (mrb_nil_p(v))
            mrb_str_buf_cat(mrb, str, "nil", 3);
        else
            mrb_str_buf_append(mrb, str, mrb_str_inspect(mrb, v));
    }
    mrb_str_buf_cat(mrb, str, ">", 1);

    return str;
}

/* 15.2.16.3.15(x) */
/* 15.2.16.3.16(x) */
/*
 * call-seq:
 *    mtch == mtch2   -> true or false
 *
 *  Equality---Two matchdata are equal if their target strings,
 *  patterns, and matched positions are identical.
 */

static mrb_value
mrb_match_equal(mrb_state *mrb, mrb_value match1)
{
  const struct re_registers *regs1, *regs2;
  mrb_value match2;

  mrb_get_args(mrb, "o", &match2);
  if (mrb_obj_equal(mrb, match1, match2)) return mrb_true_value();
  if (mrb_type(match2) != MRB_TT_MATCH) return mrb_false_value();
  if (!mrb_str_equal(mrb, mrb_obj_value(RMATCH(match1)->str), mrb_obj_value(RMATCH(match2)->str)))
    return mrb_false_value();
  if (!reg_equal(mrb, RMATCH(match1)->regexp, RMATCH(match2)->regexp)) return mrb_false_value();
  regs1 = RMATCH_REGS(match1);
  regs2 = RMATCH_REGS(match2);
  if (regs1->num_regs != regs2->num_regs) return mrb_false_value();
  if (memcmp(regs1->beg, regs2->beg, regs1->num_regs * sizeof(*regs1->beg))) return mrb_false_value();
  if (memcmp(regs1->end, regs2->end, regs1->num_regs * sizeof(*regs1->end))) return mrb_false_value();
  return mrb_true_value();
}

/*
 *  Document-class: RegexpError
 *
 *  Raised when given an invalid regexp expression.
 *
 *     Regexp.new("?")
 *
 *  <em>raises the exception:</em>
 *
 *     RegexpError: target of repeat operator is not specified: /?/
 */

/*
 *  Document-class: Regexp
 *
 *  A <code>Regexp</code> holds a regular expression, used to match a pattern
 *  against strings. Regexps are created using the <code>/.../</code> and
 *  <code>%r{...}</code> literals, and by the <code>Regexp::new</code>
 *  constructor.
 *
 *  :include: doc/re.rdoc
 */

void
mrb_init_regexp(mrb_state *mrb)
{
  struct RClass *s;
  s = mrb_define_class(mrb, "Regexp", mrb->object_class);

    mrb_define_class_method(mrb, s, "compile",         mrb_reg_s_new_instance, ARGS_ANY());              /* 15.2.15.6.1  */
    mrb_define_class_method(mrb, s, "escape",          mrb_reg_s_quote,        ARGS_REQ(1));             /* 15.2.15.6.2  */
    mrb_define_class_method(mrb, s, "last_match",      mrb_reg_s_last_match,   ARGS_ANY());              /* 15.2.15.6.3  */
    mrb_define_class_method(mrb, s, "quote",           mrb_reg_s_quote,        ARGS_REQ(1));             /* 15.2.15.6.4  */
    //mrb_define_singleton_method(rb_cRegexp, "union", rb_reg_s_union_m, -2);
    //mrb_define_singleton_method(rb_cRegexp, "try_convert", rb_reg_s_try_convert, 1);

    mrb_define_method(mrb, s, "initialize",      mrb_reg_initialize_m, ARGS_ANY());                      /* 15.2.15.7.1  */
    mrb_define_method(mrb, s, "initialize_copy", mrb_reg_init_copy,    ARGS_REQ(1));                     /* 15.2.15.7.2  */
    mrb_define_method(mrb, s, "==",              mrb_reg_equal_m,      ARGS_REQ(1));                     /* 15.2.15.7.3  */
    mrb_define_method(mrb, s, "===",             mrb_reg_eqq,          ARGS_REQ(1));                     /* 15.2.15.7.4  */
    mrb_define_method(mrb, s, "=~",              mrb_reg_match,        ARGS_REQ(1));                     /* 15.2.15.7.5  */
    mrb_define_method(mrb, s, "casefold?",       mrb_reg_casefold_p,   ARGS_NONE());                     /* 15.2.15.7.6  */
    mrb_define_method(mrb, s, "match",           mrb_reg_match_m,      ARGS_ANY());                      /* 15.2.15.7.7  */
    mrb_define_method(mrb, s, "source",          mrb_reg_source,       ARGS_NONE());                     /* 15.2.15.7.8  */
    //mrb_define_method(rb_cRegexp, "hash",            rb_reg_hash, 0);
    //mrb_define_method(rb_cRegexp, "~",               rb_reg_match2, 0);
    mrb_define_method(mrb, s, "to_s",            mrb_reg_to_s,         ARGS_NONE());                     /* 15.2.15.7.9 (x) */
    mrb_define_method(mrb, s, "inspect",         mrb_reg_inspect,      ARGS_NONE());                     /* 15.2.15.7.10(x) */
    mrb_define_method(mrb, s, "eql?",            mrb_reg_equal_m,      ARGS_REQ(1));                     /* 15.2.15.7.11(x) */
    //mrb_define_method(rb_cRegexp, "options",         mrb_reg_options_m, 0);
    //mrb_define_method(rb_cRegexp, "encoding",        rb_obj_encoding, 0); /* in encoding.c */
    //mrb_define_method(rb_cRegexp, "fixed_encoding?", mrb_reg_fixed_encoding_p, 0);
    //mrb_define_method(rb_cRegexp, "names",           rb_reg_names, 0);
    //mrb_define_method(rb_cRegexp, "named_captures",  rb_reg_named_captures, 0);

    //mrb_define_const(rb_cRegexp, "IGNORECASE", INT2FIX(ONIG_OPTION_IGNORECASE));
    //mrb_define_const(rb_cRegexp, "EXTENDED", INT2FIX(ONIG_OPTION_EXTEND));
    //mrb_define_const(rb_cRegexp, "MULTILINE", INT2FIX(ONIG_OPTION_MULTILINE));
    //mrb_define_const(rb_cRegexp, "FIXEDENCODING", INT2FIX(ARG_ENCODING_FIXED));
    mrb_define_const(mrb, s, "IGNORECASE", mrb_fixnum_value(ONIG_OPTION_IGNORECASE));
    mrb_define_const(mrb, s, "EXTENDED", mrb_fixnum_value(ONIG_OPTION_EXTEND));
    mrb_define_const(mrb, s, "MULTILINE", mrb_fixnum_value(ONIG_OPTION_MULTILINE));
    mrb_define_const(mrb, s, "FIXEDENCODING", mrb_fixnum_value(ARG_ENCODING_FIXED));

    s = mrb_define_class(mrb, "MatchData", mrb->object_class);
    //mrb_undef_class_method(CLASS_OF(rb_cMatch), "new");

    mrb_define_method(mrb, s, "[]",              mrb_match_aref,        ARGS_ANY());                     /* 15.2.16.3.1  */
    mrb_define_method(mrb, s, "begin",           mrb_match_begin,       ARGS_REQ(1));                    /* 15.2.16.3.2  */
    mrb_define_method(mrb, s, "captures",        mrb_match_captures,    ARGS_NONE());                    /* 15.2.16.3.3  */
    mrb_define_method(mrb, s, "end",             mrb_match_end,         ARGS_REQ(1));                    /* 15.2.16.3.4  */
    mrb_define_method(mrb, s, "initialize_copy", mrb_match_init_copy,   ARGS_REQ(1));                    /* 15.2.16.3.5  */
    mrb_define_method(mrb, s, "length",          mrb_match_size,        ARGS_NONE());                    /* 15.2.16.3.6  */
    mrb_define_method(mrb, s, "offset",          mrb_match_offset,      ARGS_REQ(1));                    /* 15.2.16.3.7  */
    mrb_define_method(mrb, s, "post_match",      mrb_reg_match_post,    ARGS_NONE());                    /* 15.2.16.3.8  */
    mrb_define_method(mrb, s, "pre_match",       mrb_reg_match_pre,     ARGS_NONE());                    /* 15.2.16.3.9  */
    mrb_define_method(mrb, s, "size",            mrb_match_size,        ARGS_NONE());                    /* 15.2.16.3.10 */
    mrb_define_method(mrb, s, "string",          mrb_match_string,      ARGS_NONE());                    /* 15.2.16.3.11 */
    mrb_define_method(mrb, s, "to_a",            mrb_match_to_a,        ARGS_NONE());                    /* 15.2.16.3.12 */
    mrb_define_method(mrb, s, "to_s",            mrb_match_to_s,        ARGS_NONE());                    /* 15.2.16.3.13 */
    mrb_define_method(mrb, s, "inspect",         mrb_match_inspect,     ARGS_NONE());                    /* 15.2.16.3.14(x) */
    mrb_define_method(mrb, s, "==",              mrb_match_equal,       ARGS_REQ(1));                    /* 15.2.16.3.15(x) */
    mrb_define_method(mrb, s, "eql?",            mrb_match_equal,       ARGS_REQ(1));                    /* 15.2.16.3.16(x) */
    //mrb_define_method(rb_cMatch, "regexp",          match_regexp,      0);
    //mrb_define_method(rb_cMatch, "names",           match_names,       0);
    //mrb_define_method(rb_cMatch, "values_at",       match_values_at,   -1);
    //mrb_define_method(rb_cMatch, "hash",            match_hash,        0);
    //mrb_define_method(rb_cMatch, "==",              match_equal,       1);
}
/* ----------------1_8_7---------------------------------------- */
//`mrb_check_type'
//`mrb_reg_regsub'
//`mrb_backref_get'
//`mrb_memsearch'
//`mrb_reg_mbclen2'
//`mrb_reg_regcomp'
//`mrb_yield'


mrb_value
mrb_reg_regsub(mrb_state *mrb, mrb_value str, mrb_value src, struct re_registers *regs, mrb_value regexp)
{
  mrb_value val;
  char *p, *s, *e;
  struct RString *ps = mrb_str_ptr(str);
  int no;

  val.tt = 0;
  p = s = ps->buf;
  e = s + ps->len;

  while (s < e) {
    int c = *s;
    char *ss;

    if (c == -1) {
      s += e - s;
      continue;
    }
    ss = s;
    s++;

    if (c != '\\' || s == e) continue;

    //if (!val) {
    if (!val.tt) {
      val = mrb_str_buf_new(mrb, ss-p);
    }
    mrb_str_buf_cat(mrb, val, p, ss-p);

    c = *s;
    if (c == -1) {
      s += e - s;
      mrb_str_buf_cat(mrb, val, ss, s-ss);
      p = s;
      continue;
    }
    s++;

    p = s;
    switch (c) {
      case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        if (onig_noname_group_capture_is_active(RREGEXP(regexp)->ptr)) {
            no = c - '0';
        }
        else {
            continue;
        }
        break;

      case 'k':
        if (s < e && *s == '<') {
            char *name, *name_end;

            name_end = name = s + 1;
            while (name_end < e) {
                c = *name_end;
                if (c == '>') break;
                name_end += c == -1 ? e - name_end : 1;
            }
            if (name_end < e) {
	        no = name_to_backref_number(mrb, regs, RREGEXP(regexp), name, name_end);
                p = s = name_end + 1;
                break;
            }
            else {
                mrb_raise(mrb, E_RUNTIME_ERROR, "invalid group name reference format");
            }
        }

        mrb_str_buf_cat(mrb, val, ss, s-ss);
        continue;

      case '0':
      case '&':
        no = 0;
        break;

      case '`':
        mrb_str_buf_cat(mrb, val, RSTRING_PTR(src), BEG(0));
        continue;

      case '\'':
        mrb_str_buf_cat(mrb, val, RSTRING_PTR(src)+END(0), RSTRING_LEN(src)-END(0));
        continue;

      case '+':
        no = regs->num_regs-1;
        while (BEG(no) == -1 && no > 0) no--;
        if (no == 0) continue;
        break;

      case '\\':
        mrb_str_buf_cat(mrb, val, s-1, 1);
        continue;

      default:
        mrb_str_buf_cat(mrb, val, ss, s-ss);
        continue;
    }

    if (no >= 0) {
        if (no >= regs->num_regs) continue;
        if (BEG(no) == -1) continue;
        mrb_str_buf_cat(mrb, val, RSTRING_PTR(src)+BEG(no), END(no)-BEG(no));
    }
  } /* while (s < e) { */


  if (!val.tt) return str;
  if (p < e) {
    mrb_str_buf_cat(mrb, val, p, e-p);
  }
  return val;
}

static inline NODE *
lfp_svar_place(mrb_state *mrb, /*mrb_thread_t *th,*/ mrb_value *lfp)
{
    NODE *svar;

    /*if (lfp && th->local_lfp != lfp) {
      svar = &lfp[-1];
    }
    else {
      svar = mrb->&th->local_svar;
    }*/
    svar = mrb->local_svar;
    /*if (mrb_nil_p(*svar)) {
      *svar = mrb_obj_value(NEW_IF(0, 0, 0));
    }*/
    return svar;//(NODE *)((*svar).value.p);
}

static mrb_value
lfp_svar_get(mrb_state *mrb, /*mrb_thread_t *th,*/ mrb_value *lfp, mrb_int key)
{
  //mrb_value *regs;
  NODE *svar = lfp_svar_place(mrb, /*th,*/ lfp);
  //regs = mrb->stack;

  switch (key) {
    case 0:
      return svar->u1.value;
    case 1:
      return svar->u2.value;
    default: {
      return svar->u3.value;
      /*const mrb_value hash = regs[GETARG_C(*svar)];//svar->u3.value;

      if (mrb_nil_p(hash)) {
          return mrb_nil_value();
      }
      else {
          return mrb_hash_get(mrb, hash, mrb_fixnum_value(key));//mrb_hash_lookup(hash, key);
      }*/
    }
  }
}

static void
lfp_svar_set(mrb_state *mrb, /*mrb_thread_t *th,*/ mrb_value *lfp, mrb_int key, mrb_value val)
{
  //mrb_value *regs;
  NODE *svar = lfp_svar_place(mrb, /*th,*/ lfp);
  //regs = mrb->stack;

  switch (key) {
    case 0:
      svar->u1.value = val;
      return;
    case 1:
      svar->u2.value = val;
      return;
    default: {
      svar->u3.value = val;
      //mrb_value hash = *svar;//svar->u3.value;

      //if (mrb_nil_p(hash)) {
      //    svar->u3.value = hash = mrb_hash_new(mrb, 0);
      //}
      //mrb_hash_aset(hash, key, val);
      //mrb_hash_set(mrb, hash, mrb_fixnum_value(key), val);
    }
  }
}

static mrb_value
vm_cfp_svar_get(mrb_state *mrb, /*mrb_thread_t *th, mrb_control_frame_t *cfp,*/ mrb_int key)
{
  //cfp = vm_normal_frame(th, cfp);
  return lfp_svar_get(mrb, /*th, cfp ? cfp->lfp :*/ 0, key);
}

static void
vm_cfp_svar_set(mrb_state *mrb, /*mrb_thread_t *th, mrb_control_frame_t *cfp,*/ mrb_int key, const mrb_value val)
{
  //cfp = vm_normal_frame(th, cfp);
  lfp_svar_set(mrb, /*th, cfp ? cfp->lfp : */0, key, val);
}

static mrb_value
vm_svar_get(mrb_state *mrb, mrb_int key)
{
  //mrb_thread_t *th = GET_THREAD();
  return vm_cfp_svar_get(mrb,/*th, th->cfp,*/ key);
}

static void
vm_svar_set(mrb_state *mrb, mrb_int key, mrb_value val)
{
  //mrb_thread_t *th = GET_THREAD();
  vm_cfp_svar_set(mrb,/*th, th->cfp,*/ key, val);
}


int
mrb_reg_backref_number(mrb_state *mrb, mrb_value match, mrb_value backref)
{
    return match_backref_number(mrb, match, backref);
}

mrb_value
mrb_backref_get(mrb_state *mrb)
{
  return vm_svar_get(mrb, 1);
}

void
mrb_backref_set(mrb_state *mrb, mrb_value val)
{
  vm_svar_set(mrb, 1, val);
}
#endif //ENABLE_REGEXP

#ifdef INCLUDE_ENCODING
static inline long
mrb_memsearch_qs(const unsigned char *xs, long m, const unsigned char *ys, long n)
{
  const unsigned char *x = xs, *xe = xs + m;
  const unsigned char *y = ys;
  int i, qstable[256];

  /* Preprocessing */
  for (i = 0; i < 256; ++i)
    qstable[i] = m + 1;
  for (; x < xe; ++x)
    qstable[*x] = xe - x;
  /* Searching */
  for (; y + m <= ys + n; y += *(qstable + y[m])) {
    if (*xs == *y && memcmp(xs, y, m) == 0)
        return y - ys;
  }
  return -1;
}

static inline unsigned int
mrb_memsearch_qs_utf8_hash(const unsigned char *x)
{
  register const unsigned int mix = 8353;
  register unsigned int h = *x;
  if (h < 0xC0) {
    return h + 256;
  }
  else if (h < 0xE0) {
    h *= mix;
    h += x[1];
  }
  else if (h < 0xF0) {
    h *= mix;
    h += x[1];
    h *= mix;
    h += x[2];
  }
  else if (h < 0xF5) {
    h *= mix;
    h += x[1];
    h *= mix;
    h += x[2];
    h *= mix;
    h += x[3];
  }
  else {
    return h + 256;
  }
  return (unsigned char)h;
}

static inline long
mrb_memsearch_qs_utf8(const unsigned char *xs, long m, const unsigned char *ys, long n)
{
  const unsigned char *x = xs, *xe = xs + m;
  const unsigned char *y = ys;
  int i, qstable[512];

  /* Preprocessing */
  for (i = 0; i < 512; ++i) {
    qstable[i] = m + 1;
  }
  for (; x < xe; ++x) {
    qstable[mrb_memsearch_qs_utf8_hash(x)] = xe - x;
  }
  /* Searching */
  for (; y + m <= ys + n; y += qstable[mrb_memsearch_qs_utf8_hash(y+m)]) {
    if (*xs == *y && memcmp(xs, y, m) == 0)
        return y - ys;
  }
  return -1;
}

int
mrb_memsearch(mrb_state *mrb, const void *x0, int m, const void *y0, int n, mrb_encoding *enc)
{
  const unsigned char *x = x0, *y = y0;

  if (m > n) return -1;
  else if (m == n) {
    return memcmp(x0, y0, m) == 0 ? 0 : -1;
  }
  else if (m < 1) {
    return 0;
  }
  else if (m == 1) {
    const unsigned char *ys = y, *ye = ys + n;
    for (; y < ye; ++y) {
      if (*x == *y)
        return y - ys;
    }
    return -1;
  }
  else {
    return mrb_memsearch_qs(x0, m, y0, n);
  }
}
#endif //INCLUDE_ENCODING

#ifdef ENABLE_REGEXP
mrb_value
mrb_reg_init_str(mrb_state *mrb, mrb_value re, mrb_value s, int options)
{
  onig_errmsg_buffer err = "";

  if (mrb_reg_initialize_str(mrb, re, s, options, err, NULL, 0) != 0) {
    //mrb_reg_raise_str(s, options, err);
    printf("mrb_reg_raise_str(s, options, err);");
  }

  return re;
}

mrb_value
mrb_reg_alloc(mrb_state *mrb)
{
  mrb_value dummy = mrb_nil_value();
  return mrb_reg_s_alloc(mrb, dummy);
}

mrb_value
mrb_reg_new_str(mrb_state *mrb, mrb_value s, int options)
{
  return mrb_reg_init_str(mrb, mrb_reg_alloc(mrb), s, options);
}

mrb_value
mrb_reg_regcomp(mrb_state *mrb, mrb_value str)
{
  return mrb_reg_new_str(mrb, str, 0);
}

int
re_adjust_startpos(struct re_pattern_buffer *bufp, const char *string, int size, int startpos, int range)
{
  /* Update the fastmap now if not correct already.  */
  /*if (!bufp->fastmap_accurate) {
    int ret = re_compile_fastmap0(bufp);
    if (ret) return ret;
  }*/

  /* Adjust startpos for mbc string */
  /*if (current_mbctype && startpos>0 && !(bufp->options&RE_OPTIMIZE_BMATCH)) {
    startpos = re_mbc_startpos(string, size, startpos, range);
  }*/
  return startpos;
}
#endif //ENABLE_REGEXP

#ifdef INCLUDE_ENCODING
static const unsigned char mbctab_ascii[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
const unsigned char *re_mbctab = mbctab_ascii;

#define is_identchar(p,e,enc) (mrb_enc_isalnum(*p,enc) || (*p) == '_' || !ISASCII(*p))

static int
is_special_global_name(const char *m, const char *e, mrb_encoding *enc)
{
    int mb = 0;

    if (m >= e) return 0;
    switch (*m) {
      case '~': case '*': case '$': case '?': case '!': case '@':
      case '/': case '\\': case ';': case ',': case '.': case '=':
      case ':': case '<': case '>': case '\"':
      case '&': case '`': case '\'': case '+':
      case '0':
        ++m;
        break;
      case '-':
        ++m;
        if (m < e && is_identchar(m, e, enc)) {
            if (!ISASCII(*m)) mb = 1;
            m += e - m;
        }
        break;
      default:
        if (!mrb_enc_isdigit(*m, enc)) return 0;
        do {
            if (!ISASCII(*m)) mb = 1;
            ++m;
        } while (m < e && mrb_enc_isdigit(*m, enc));
    }
    return m == e ? mb + 1 : 0;
}

int
mrb_enc_symname2_p(const char *name, long len, mrb_encoding *enc)
{
    const char *m = name;
    const char *e = m + len;
    int localid = FALSE;

    if (!m) return FALSE;
    switch (*m) {
      case '\0':
        return FALSE;

      case '$':
        if (is_special_global_name(++m, e, enc)) return TRUE;
        goto id;

      case '@':
        if (*++m == '@') ++m;
        goto id;

      case '<':
        switch (*++m) {
          case '<': ++m; break;
          case '=': if (*++m == '>') ++m; break;
          default: break;
        }
        break;

      case '>':
        switch (*++m) {
          case '>': case '=': ++m; break;
        }
        break;

      case '=':
        switch (*++m) {
          case '~': ++m; break;
          case '=': if (*++m == '=') ++m; break;
          default: return FALSE;
        }
        break;

      case '*':
        if (*++m == '*') ++m;
        break;

      case '+': case '-':
        if (*++m == '@') ++m;
        break;

      case '|': case '^': case '&': case '/': case '%': case '~': case '`':
        ++m;
        break;

      case '[':
        if (*++m != ']') return FALSE;
        if (*++m == '=') ++m;
        break;

      case '!':
        switch (*++m) {
          case '\0': return TRUE;
          case '=': case '~': ++m; break;
          default: return FALSE;
        }
        break;

      default:
        localid = !mrb_enc_isupper(*m, enc);
id:
        if (m >= e || (*m != '_' && !mrb_enc_isalpha(*m, enc) && ISASCII(*m)))
            return FALSE;
        while (m < e && is_identchar(m, e, enc)) m += e - m;
        if (localid) {
            switch (*m) {
              case '!': case '?': case '=': ++m;
            }
        }
        break;
    }
    return m == e;
}

int
mrb_enc_symname_p(const char *name, mrb_encoding *enc)
{
    return mrb_enc_symname2_p(name, strlen(name), enc);
}
#endif //INCLUDE_ENCODING
