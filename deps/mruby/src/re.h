/*
** re.h - Regexp class
**
** See Copyright Notice in mruby.h
*/

#ifndef RE_H
#define RE_H

//#include <sys/types.h>
#include <stdio.h>

#include "node.h"
#include "regex.h"
#include "encoding.h"
#include "st.h"

#define BEG(no) regs->beg[no]
#define END(no) regs->end[no]

struct rmatch_offset {
    long beg;
    long end;
};

struct rmatch {
    struct re_registers regs;

    int char_offset_updated;
    int char_offset_num_allocated;
    struct rmatch_offset *char_offset;
};

struct RMatch {
  MRB_OBJECT_HEADER;
  struct RString *str;
  struct rmatch *rmatch;
  struct RRegexp *regexp;
};

struct RRegexp {
  MRB_OBJECT_HEADER;
  struct re_pattern_buffer *ptr;
  struct RString *src;
  unsigned long usecnt;
};

#define mrb_regex_ptr(r)    ((struct RRegexp*)((r).value.p))
#define RREGEXP(r)          ((struct RRegexp*)((r).value.p))
#define RREGEXP_SRC(r)      (RREGEXP(r)->src)
#define RREGEXP_SRC_PTR(r)  (RREGEXP_SRC(r)->buf)
#define RREGEXP_SRC_LEN(r)  (RREGEXP_SRC(r)->len)
int re_adjust_startpos(struct re_pattern_buffer *bufp, const char *string, int size, int startpos, int range);

typedef struct re_pattern_buffer Regexp;

//#define RMATCH(obj)  (R_CAST(RMatch)(obj))
#define RMATCH_REGS(v)      (&((struct RMatch*)((v).value.p))->rmatch->regs)
#define RMATCH(v)           ((struct RMatch*)((v).value.p))
#define mrb_match_ptr(v)    ((struct RMatch*)((v).value.p))

int mrb_memcmp(const void *p1, const void *p2, int len);

mrb_int mrb_reg_search (mrb_state *mrb, mrb_value, mrb_value, mrb_int, mrb_int);
mrb_value mrb_reg_regsub (mrb_state *mrb, mrb_value, mrb_value, struct re_registers *, mrb_value);
//mrb_value mrb_reg_regsub(mrb_value, mrb_value, struct re_registers *, mrb_value);
mrb_int mrb_reg_adjust_startpos(mrb_state *mrb, mrb_value re, mrb_value str, mrb_int pos, mrb_int reverse);
void mrb_match_busy (mrb_value);

mrb_value mrb_reg_quote(mrb_state *mrb, mrb_value str);
mrb_value mrb_reg_regcomp(mrb_state *mrb, mrb_value str);
mrb_value mrb_reg_match_str(mrb_state *mrb, mrb_value re, mrb_value str);
mrb_value mrb_reg_nth_match(mrb_state *mrb, mrb_int nth, mrb_value match);
mrb_value mrb_backref_get(mrb_state *mrb);
//mrb_int mrb_memsearch(const void *x0, mrb_int m, const void *y0, mrb_int n);
mrb_value mrb_reg_to_s(mrb_state *mrb, mrb_value re);
void mrb_backref_set(mrb_state *mrb, mrb_value val);
mrb_value match_alloc(mrb_state *mrb);
int mrb_reg_backref_number(mrb_state *mrb, mrb_value match, mrb_value backref);

#endif
