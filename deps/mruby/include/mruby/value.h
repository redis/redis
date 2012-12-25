/*
** mruby/value.h - mrb_value definition
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_VALUE_H
#define MRUBY_VALUE_H

#ifndef MRB_NAN_BOXING

enum mrb_vtype {
  MRB_TT_FALSE = 0,   /*   0 */
  MRB_TT_FREE,        /*   1 */
  MRB_TT_TRUE,        /*   2 */
  MRB_TT_FIXNUM,      /*   3 */
  MRB_TT_SYMBOL,      /*   4 */
  MRB_TT_UNDEF,       /*   5 */
  MRB_TT_FLOAT,       /*   6 */
  MRB_TT_VOIDP,       /*   7 */
  MRB_TT_MAIN,        /*   8 */
  MRB_TT_OBJECT,      /*   9 */
  MRB_TT_CLASS,       /*  10 */
  MRB_TT_MODULE,      /*  11 */
  MRB_TT_ICLASS,      /*  12 */
  MRB_TT_SCLASS,      /*  13 */
  MRB_TT_PROC,        /*  14 */
  MRB_TT_ARRAY,       /*  15 */
  MRB_TT_HASH,        /*  16 */
  MRB_TT_STRING,      /*  17 */
  MRB_TT_RANGE,       /*  18 */
  MRB_TT_REGEX,       /*  19 */
  MRB_TT_STRUCT,      /*  20 */
  MRB_TT_EXCEPTION,   /*  21 */
  MRB_TT_MATCH,       /*  22 */
  MRB_TT_FILE,        /*  23 */
  MRB_TT_ENV,         /*  24 */
  MRB_TT_DATA,        /*  25 */
  MRB_TT_MAXDEFINE    /*  26 */
};

typedef struct mrb_value {
  union {
    mrb_float f;
    void *p;
    mrb_int i;
    mrb_sym sym;
  } value;
  enum mrb_vtype tt:8;
} mrb_value;

#define mrb_type(o)   (o).tt
#define mrb_float(o)  (o).value.f

#define MRB_SET_VALUE(o, ttt, attr, v) do {\
  (o).tt = ttt;\
  (o).attr = v;\
} while (0);

static inline mrb_value
mrb_float_value(mrb_float f)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_FLOAT, value.f, f);
  return v;
}
#else  /* MRB_NAN_BOXING */

#ifdef MRB_USE_FLOAT
# error ---->> MRB_NAN_BOXING and MRB_USE_FLOAT conflict <<----
#endif

enum mrb_vtype {
  MRB_TT_FALSE = 1,   /*   1 */
  MRB_TT_FREE,        /*   2 */
  MRB_TT_TRUE,        /*   3 */
  MRB_TT_FIXNUM,      /*   4 */
  MRB_TT_SYMBOL,      /*   5 */
  MRB_TT_UNDEF,       /*   6 */
  MRB_TT_FLOAT,       /*   7 */
  MRB_TT_VOIDP,       /*   8 */
  MRB_TT_MAIN,        /*   9 */
  MRB_TT_OBJECT,      /*  10 */
  MRB_TT_CLASS,       /*  11 */
  MRB_TT_MODULE,      /*  12 */
  MRB_TT_ICLASS,      /*  13 */
  MRB_TT_SCLASS,      /*  14 */
  MRB_TT_PROC,        /*  15 */
  MRB_TT_ARRAY,       /*  16 */
  MRB_TT_HASH,        /*  17 */
  MRB_TT_STRING,      /*  18 */
  MRB_TT_RANGE,       /*  19 */
  MRB_TT_REGEX,       /*  20 */
  MRB_TT_STRUCT,      /*  21 */
  MRB_TT_EXCEPTION,   /*  22 */
  MRB_TT_MATCH,       /*  23 */
  MRB_TT_FILE,        /*  24 */
  MRB_TT_ENV,         /*  25 */
  MRB_TT_DATA,        /*  26 */
  MRB_TT_MAXDEFINE    /*  27 */
};

#ifdef MRB_ENDIAN_BIG
#define MRB_ENDIAN_LOHI(a,b) a b
#else
#define MRB_ENDIAN_LOHI(a,b) b a
#endif

typedef struct mrb_value {
  union {
    mrb_float f;
    struct {
      MRB_ENDIAN_LOHI(
 	uint32_t ttt;
        ,union {
	  void *p;
	  mrb_int i;
	  mrb_sym sym;
	} value;
       )
    };
  };
} mrb_value;

#define mrb_tt(o)     ((o).ttt & 0xff)
#define mrb_mktt(tt)  (0xfff00000|(tt))
#define mrb_type(o)   ((uint32_t)0xfff00000 < (o).ttt ? mrb_tt(o) : MRB_TT_FLOAT)
#define mrb_float(o)  (o).f

#define MRB_SET_VALUE(o, tt, attr, v) do {\
  (o).ttt = mrb_mktt(tt);\
  (o).attr = v;\
} while (0);

static inline mrb_value
mrb_float_value(mrb_float f)
{
  mrb_value v;

  if (f != f) {
    v.ttt = 0x7ff80000;
    v.value.i = 0;
  } else {
    v.f = f;
  }
  return v;
}
#endif	/* MRB_NAN_BOXING */

#define mrb_fixnum(o) (o).value.i
#define mrb_symbol(o) (o).value.sym
#define mrb_object(o) ((struct RBasic *) (o).value.p)
#define mrb_voidp(o) (o).value.p
#define mrb_fixnum_p(o) (mrb_type(o) == MRB_TT_FIXNUM)
#define mrb_float_p(o) (mrb_type(o) == MRB_TT_FLOAT)
#define mrb_undef_p(o) (mrb_type(o) == MRB_TT_UNDEF)
#define mrb_nil_p(o)  (mrb_type(o) == MRB_TT_FALSE && !(o).value.i)
#define mrb_symbol_p(o) (mrb_type(o) == MRB_TT_SYMBOL)
#define mrb_array_p(o) (mrb_type(o) == MRB_TT_ARRAY)
#define mrb_string_p(o) (mrb_type(o) == MRB_TT_STRING)
#define mrb_hash_p(o) (mrb_type(o) == MRB_TT_HASH)
#define mrb_voidp_p(o) (mrb_type(o) == MRB_TT_VOIDP)
#define mrb_test(o)   (mrb_type(o) != MRB_TT_FALSE)

#define MRB_OBJECT_HEADER \
  enum mrb_vtype tt:8;\
  unsigned int color:3;\
  unsigned int flags:21;\
  struct RClass *c;\
  struct RBasic *gcnext

/* white: 011, black: 100, gray: 000 */
#define MRB_GC_GRAY 0
#define MRB_GC_WHITE_A 1
#define MRB_GC_WHITE_B (1 << 1)
#define MRB_GC_BLACK (1 << 2)
#define MRB_GC_WHITES (MRB_GC_WHITE_A | MRB_GC_WHITE_B)
#define MRB_GC_COLOR_MASK 7

#define paint_gray(o) ((o)->color = MRB_GC_GRAY)
#define paint_black(o) ((o)->color = MRB_GC_BLACK)
#define paint_white(o) ((o)->color = MRB_GC_WHITES)
#define paint_partial_white(s, o) ((o)->color = (s)->current_white_part)
#define is_gray(o) ((o)->color == MRB_GC_GRAY)
#define is_white(o) ((o)->color & MRB_GC_WHITES)
#define is_black(o) ((o)->color & MRB_GC_BLACK)
#define is_dead(s, o) (((o)->color & other_white_part(s) & MRB_GC_WHITES) || (o)->tt == MRB_TT_FREE)
#define flip_white_part(s) ((s)->current_white_part = other_white_part(s))
#define other_white_part(s) ((s)->current_white_part ^ MRB_GC_WHITES)

struct RBasic {
  MRB_OBJECT_HEADER;
};

#define mrb_basic(v)     ((struct RBasic*)((v).value.p))

struct RObject {
  MRB_OBJECT_HEADER;
  struct iv_tbl *iv;
};

#define mrb_obj_ptr(v)   ((struct RObject*)((v).value.p))
#define mrb_immediate_p(x) (mrb_type(x) <= MRB_TT_MAIN)
#define mrb_special_const_p(x) mrb_immediate_p(x)

static inline mrb_value
mrb_fixnum_value(mrb_int i)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_FIXNUM, value.i, i);
  return v;
}

static inline mrb_value
mrb_symbol_value(mrb_sym i)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_SYMBOL, value.sym, i);
  return v;
}

static inline mrb_value
mrb_obj_value(void *p)
{
  mrb_value v;
  struct RBasic *b = (struct RBasic*)p;

  MRB_SET_VALUE(v, b->tt, value.p, p);
  return v;
}

static inline mrb_value
mrb_voidp_value(void *p)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_VOIDP, value.p, p);
  return v;
}

static inline mrb_value
mrb_false_value(void)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_FALSE, value.i, 1);
  return v;
}

static inline mrb_value
mrb_nil_value(void)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_FALSE, value.i, 0);
  return v;
}

static inline mrb_value
mrb_true_value(void)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_TRUE, value.i, 1);
  return v;
}

static inline mrb_value
mrb_undef_value(void)
{
  mrb_value v;

  MRB_SET_VALUE(v, MRB_TT_UNDEF, value.i, 0);
  return v;
}

#endif  /* MRUBY_OBJECT_H */
