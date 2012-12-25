/*
** symbol.c - Symbol class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/khash.h"
#include <string.h>

#include "mruby/string.h"
#include <ctype.h>

/* ------------------------------------------------------ */
typedef struct symbol_name {
  int len;
  const char *name;
} symbol_name;

static inline khint_t
sym_hash_func(mrb_state *mrb, const symbol_name s)
{
  khint_t h = 0;
  size_t i;
  const char *p = s.name;

  for (i=0; i<s.len; i++) {
    h = (h << 5) - h + *p++;
  }
  return h;
}
#define sym_hash_equal(mrb,a, b) (a.len == b.len && memcmp(a.name, b.name, a.len) == 0)

KHASH_DECLARE(n2s, symbol_name, mrb_sym, 1)
KHASH_DEFINE (n2s, symbol_name, mrb_sym, 1, sym_hash_func, sym_hash_equal)
/* ------------------------------------------------------ */
mrb_sym
mrb_intern2(mrb_state *mrb, const char *name, int len)
{
  khash_t(n2s) *h = mrb->name2sym;
  symbol_name sname;
  khiter_t k;
  mrb_sym sym;
  char *p;

  sname.len = len;
  sname.name = name;
  k = kh_get(n2s, h, sname);
  if (k != kh_end(h))
    return kh_value(h, k);

  sym = ++mrb->symidx;
  p = (char *)mrb_malloc(mrb, len+1);
  memcpy(p, name, len);
  p[len] = 0;
  sname.name = (const char*)p;
  k = kh_put(n2s, h, sname);
  kh_value(h, k) = sym;

  return sym;
}

mrb_sym
mrb_intern(mrb_state *mrb, const char *name)
{
  return mrb_intern2(mrb, name, strlen(name));
}

mrb_sym
mrb_intern_str(mrb_state *mrb, mrb_value str)
{
  return mrb_intern2(mrb, RSTRING_PTR(str), RSTRING_LEN(str));
}

const char*
mrb_sym2name_len(mrb_state *mrb, mrb_sym sym, int *lenp)
{
  khash_t(n2s) *h = mrb->name2sym;
  khiter_t k;
  symbol_name sname;

  for (k = kh_begin(h); k != kh_end(h); k++) {
    if (kh_exist(h, k)) {
      if (kh_value(h, k) == sym) break;
    }
  }
  if (k == kh_end(h)) {
    *lenp = 0;
    return NULL;	/* missing */
  }
  sname = kh_key(h, k);
  *lenp = sname.len;
  return sname.name;
}

void
mrb_free_symtbl(mrb_state *mrb)
{
  khash_t(n2s) *h = mrb->name2sym;
  khiter_t k;

  for (k = kh_begin(h); k != kh_end(h); k++)
    if (kh_exist(h, k)) mrb_free(mrb, (char*)kh_key(h, k).name);
  kh_destroy(n2s,mrb->name2sym);
}

void
mrb_init_symtbl(mrb_state *mrb)
{
  mrb->name2sym = kh_init(n2s, mrb);
}

/**********************************************************************
 * Document-class: Symbol
 *
 *  <code>Symbol</code> objects represent names and some strings
 *  inside the Ruby
 *  interpreter. They are generated using the <code>:name</code> and
 *  <code>:"string"</code> literals
 *  syntax, and by the various <code>to_sym</code> methods. The same
 *  <code>Symbol</code> object will be created for a given name or string
 *  for the duration of a program's execution, regardless of the context
 *  or meaning of that name. Thus if <code>Fred</code> is a constant in
 *  one context, a method in another, and a class in a third, the
 *  <code>Symbol</code> <code>:Fred</code> will be the same object in
 *  all three contexts.
 *
 *     module One
 *       class Fred
 *       end
 *       $f1 = :Fred
 *     end
 *     module Two
 *       Fred = 1
 *       $f2 = :Fred
 *     end
 *     def Fred()
 *     end
 *     $f3 = :Fred
 *     $f1.object_id   #=> 2514190
 *     $f2.object_id   #=> 2514190
 *     $f3.object_id   #=> 2514190
 *
 */


/* 15.2.11.3.1  */
/*
 *  call-seq:
 *     sym == obj   -> true or false
 *
 *  Equality---If <i>sym</i> and <i>obj</i> are exactly the same
 *  symbol, returns <code>true</code>.
 */

static mrb_value
sym_equal(mrb_state *mrb, mrb_value sym1)
{
  mrb_value sym2;

  mrb_get_args(mrb, "o", &sym2);
  if (mrb_obj_equal(mrb, sym1, sym2)) return mrb_true_value();
    return mrb_false_value();
}

/* 15.2.11.3.2  */
/* 15.2.11.3.3  */
/*
 *  call-seq:
 *     sym.id2name   -> string
 *     sym.to_s      -> string
 *
 *  Returns the name or string corresponding to <i>sym</i>.
 *
 *     :fred.id2name   #=> "fred"
 */
mrb_value
mrb_sym_to_s(mrb_state *mrb, mrb_value sym)
{
  mrb_sym id = mrb_symbol(sym);
  const char *p;
  int len;

  p = mrb_sym2name_len(mrb, id, &len);
  return mrb_str_new(mrb, p, len);
}

/* 15.2.11.3.4  */
/*
 * call-seq:
 *   sym.to_sym   -> sym
 *   sym.intern   -> sym
 *
 * In general, <code>to_sym</code> returns the <code>Symbol</code> corresponding
 * to an object. As <i>sym</i> is already a symbol, <code>self</code> is returned
 * in this case.
 */

static mrb_value
sym_to_sym(mrb_state *mrb, mrb_value sym)
{
    return sym;
}

/* 15.2.11.3.5(x)  */
/*
 *  call-seq:
 *     sym.inspect    -> string
 *
 *  Returns the representation of <i>sym</i> as a symbol literal.
 *
 *     :fred.inspect   #=> ":fred"
 */

#if __STDC__
# define SIGN_EXTEND_CHAR(c) ((signed char)(c))
#else  /* not __STDC__ */
/* As in Harbison and Steele.  */
# define SIGN_EXTEND_CHAR(c) ((((unsigned char)(c)) ^ 128) - 128)
#endif
#define is_identchar(c) (SIGN_EXTEND_CHAR(c)!=-1&&(ISALNUM(c) || (c) == '_'))

static int
is_special_global_name(const char* m)
{
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
        if (is_identchar(*m)) m += 1;
        break;
      default:
        if (!ISDIGIT(*m)) return FALSE;
        do ++m; while (ISDIGIT(*m));
    }
    return !*m;
}

static int
symname_p(const char *name)
{
    const char *m = name;
    int localid = FALSE;

    if (!m) return FALSE;
    switch (*m) {
      case '\0':
        return FALSE;

      case '$':
        if (is_special_global_name(++m)) return TRUE;
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
	default: break;
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
      case '!':
        if (*++m == '=') ++m;
        break;
      case '+': case '-':
        if (*++m == '@') ++m;
        break;
      case '|':
        if (*++m == '|') ++m;
        break;
      case '&':
        if (*++m == '&') ++m;
        break;

      case '^': case '/': case '%': case '~': case '`':
        ++m;
        break;

      case '[':
        if (*++m != ']') return FALSE;
        if (*++m == '=') ++m;
        break;

      default:
        localid = !ISUPPER(*m);
id:
        if (*m != '_' && !ISALPHA(*m)) return FALSE;
        while (is_identchar(*m)) m += 1;
        if (localid) {
            switch (*m) {
	    case '!': case '?': case '=': ++m;
	    default: break;
            }
        }
        break;
    }
    return *m ? FALSE : TRUE;
}

static mrb_value
sym_inspect(mrb_state *mrb, mrb_value sym)
{
  mrb_value str;
  const char *name;
  int len;
  mrb_sym id = mrb_symbol(sym);

  name = mrb_sym2name_len(mrb, id, &len);
  str = mrb_str_new(mrb, 0, len+1);
  RSTRING(str)->ptr[0] = ':';
  memcpy(RSTRING(str)->ptr+1, name, len);
  if (!symname_p(name) || strlen(name) != len) {
    str = mrb_str_dump(mrb, str);
    memcpy(RSTRING(str)->ptr, ":\"", 2);
  }
  return str;
}

const char*
mrb_sym2name(mrb_state *mrb, mrb_sym sym)
{
  int len;
  const char *name = mrb_sym2name_len(mrb, sym, &len);

  if (!name) return NULL;
  if (symname_p(name) && strlen(name) == len) {
    return name;
  }
  else {
    mrb_value str = mrb_str_dump(mrb, mrb_str_new(mrb, name, len));
    return RSTRING(str)->ptr;
  }
}

#define lesser(a,b) (((a)>(b))?(b):(a))

static mrb_value
sym_cmp(mrb_state *mrb, mrb_value s1)
{
  mrb_value s2;
  mrb_sym sym1, sym2;

  mrb_get_args(mrb, "o", &s2);
  if (mrb_type(s2) != MRB_TT_SYMBOL) return mrb_nil_value();
  sym1 = mrb_symbol(s1);
  sym2 = mrb_symbol(s2);
  if (sym1 == sym2) return mrb_fixnum_value(0);
  else {
    const char *p1, *p2;
    int len, len1, len2, retval;

    p1 = mrb_sym2name_len(mrb, sym1, &len1);
    p2 = mrb_sym2name_len(mrb, sym2, &len2);
    len = lesser(len1, len2);
    retval = memcmp(p1, p2, len);
    if (retval == 0) {
      if (len1 == len2) return mrb_fixnum_value(0);
      if (len1 > len2)  return mrb_fixnum_value(1);
      return mrb_fixnum_value(-1);
    }
    if (retval > 0) return mrb_fixnum_value(1);
    return mrb_fixnum_value(-1);
  }
}

void
mrb_init_symbol(mrb_state *mrb)
{
  struct RClass *sym;

  sym = mrb->symbol_class = mrb_define_class(mrb, "Symbol", mrb->object_class);

  mrb_define_method(mrb, sym, "===",             sym_equal,               ARGS_REQ(1));              /* 15.2.11.3.1  */
  mrb_define_method(mrb, sym, "id2name",         mrb_sym_to_s,            ARGS_NONE());              /* 15.2.11.3.2  */
  mrb_define_method(mrb, sym, "to_s",            mrb_sym_to_s,            ARGS_NONE());              /* 15.2.11.3.3  */
  mrb_define_method(mrb, sym, "to_sym",          sym_to_sym,              ARGS_NONE());              /* 15.2.11.3.4  */
  mrb_define_method(mrb, sym, "inspect",         sym_inspect,             ARGS_NONE());              /* 15.2.11.3.5(x)  */
  mrb_define_method(mrb, sym, "<=>",             sym_cmp,                 ARGS_REQ(1));
  mrb->init_sym = mrb_intern(mrb, "initialize");
}
