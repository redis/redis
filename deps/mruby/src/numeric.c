/*
** numeric.c - Numeric, Integer, Float, Fixnum class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/numeric.h"
#include "mruby/string.h"
#include "mruby/array.h"

#include <math.h>
#include <stdio.h>
#include <assert.h>

#if defined(__FreeBSD__) && __FreeBSD__ < 4
#include <floatingpoint.h>
#endif

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef HAVE_IEEEFP_H
#include <ieeefp.h>
#endif

#define RSHIFT(x,y) ((x)>>(int)(y))

#ifdef MRB_USE_FLOAT
#define floor(f) floorf(f)
#define ceil(f) ceilf(f)
#define floor(f) floorf(f)
#define fmod(x,y) fmodf(x,y)
#endif

static mrb_float
mrb_to_flo(mrb_state *mrb, mrb_value val)
{
  switch (mrb_type(val)) {
  case MRB_TT_FIXNUM:
    return (mrb_float)mrb_fixnum(val);
  case MRB_TT_FLOAT:
    break;
  default:
    mrb_raise(mrb, E_TYPE_ERROR, "non float value");
  }
  return mrb_float(val);
}

/*
 *  call-seq:
 *     +num  ->  num
 *
 *  Unary Plus---Returns the receiver's value.
 */

static mrb_value
num_uplus(mrb_state *mrb, mrb_value num)
{
  return num;
}

/*
 *  call-seq:
 *     -num  ->  numeric
 *
 *  Unary Minus---Returns the receiver's value, negated.
 */

static mrb_value
num_uminus(mrb_state *mrb, mrb_value num)
{
  return mrb_float_value((mrb_float)0 - mrb_to_flo(mrb, num));
}

static mrb_value
fix_uminus(mrb_state *mrb, mrb_value num)
{
  return mrb_fixnum_value(0 - mrb_fixnum(num));
}

/*
 * call-seq:
 *
 *  num ** other  ->  num
 *
 * Raises <code>num</code> the <code>other</code> power.
 *
 *    2.0**3      #=> 8.0
 */
static mrb_value
num_pow(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  int both_int = FALSE;
  mrb_float d;

  mrb_get_args(mrb, "o", &y);
  if (mrb_fixnum_p(x) && mrb_fixnum_p(y)) both_int = TRUE;
  d = pow(mrb_to_flo(mrb, x), mrb_to_flo(mrb, y));
  if (both_int && FIXABLE(d))
    return mrb_fixnum_value((mrb_int)d);
  return mrb_float_value(d);
}

/* 15.2.8.3.4  */
/* 15.2.9.3.4  */
/*
 * call-seq:
 *   num / other  ->  num
 *
 * Performs division: the class of the resulting object depends on
 * the class of <code>num</code> and on the magnitude of the
 * result.
 */

mrb_value
mrb_num_div(mrb_state *mrb, mrb_value x, mrb_value y)
{
  return mrb_float_value(mrb_to_flo(mrb, x) / mrb_to_flo(mrb, y));
}

/* 15.2.9.3.19(x) */
/*
 *  call-seq:
 *     num.quo(numeric)  ->  real
 *
 *  Returns most exact division.
 */

static mrb_value
num_div(mrb_state *mrb, mrb_value x)
{
  mrb_float y;

  mrb_get_args(mrb, "f", &y);
  return mrb_float_value(mrb_to_flo(mrb, x) / y);
}

/*
 *  call-seq:
 *     num.abs        ->  numeric
 *     num.magnitude  ->  numeric
 *
 *  Returns the absolute value of <i>num</i>.
 *
 *     12.abs         #=> 12
 *     (-34.56).abs   #=> 34.56
 *     -34.56.abs     #=> 34.56
 */

static mrb_value
num_abs(mrb_state *mrb, mrb_value num)
{
  if (mrb_to_flo(mrb, num) < 0) {
    return num_uminus(mrb, num);
  }
  return num;
}

/********************************************************************
 *
 * Document-class: Float
 *
 *  <code>Float</code> objects represent inexact real numbers using
 *  the native architecture's double-precision floating point
 *  representation.
 */

/* 15.2.9.3.16(x) */
/*
 *  call-seq:
 *     flt.to_s  ->  string
 *
 *  Returns a string containing a representation of self. As well as a
 *  fixed or exponential form of the number, the call may return
 *  ``<code>NaN</code>'', ``<code>Infinity</code>'', and
 *  ``<code>-Infinity</code>''.
 */

static mrb_value
flo_to_s(mrb_state *mrb, mrb_value flt)
{
  char buf[32];
  int n;
  mrb_float value = mrb_float(flt);

  if (isinf(value)) {
    static const char s[2][5] = { "-inf", "inf" };
    static const int n[] = { 4, 3 };
    int idx;
    idx = (value < 0) ? 0 : 1;
    return mrb_str_new(mrb, s[idx], n[idx]);
  } else if(isnan(value))
    return mrb_str_new(mrb, "NaN", 3);

#ifdef MRB_USE_FLOAT
  n = sprintf(buf, "%.7g", value);
#else
  n = sprintf(buf, "%.14g", value);
#endif
  assert(n >= 0);
  return mrb_str_new(mrb, buf, n);
}

/* 15.2.9.3.2  */
/*
 * call-seq:
 *   float - other  ->  float
 *
 * Returns a new float which is the difference of <code>float</code>
 * and <code>other</code>.
 */

static mrb_value
flo_minus(mrb_state *mrb, mrb_value x)
{
  mrb_value y;

  mrb_get_args(mrb, "o", &y);
  return mrb_float_value(mrb_float(x) - mrb_to_flo(mrb, y));
}

/* 15.2.9.3.3  */
/*
 * call-seq:
 *   float * other  ->  float
 *
 * Returns a new float which is the product of <code>float</code>
 * and <code>other</code>.
 */

static mrb_value
flo_mul(mrb_state *mrb, mrb_value x)
{
  mrb_value y;

  mrb_get_args(mrb, "o", &y);
  return mrb_float_value(mrb_float(x) * mrb_to_flo(mrb, y));
}

static void
flodivmod(mrb_state *mrb, mrb_float x, mrb_float y, mrb_float *divp, mrb_float *modp)
{
  mrb_float div, mod;

  if (y == 0.0) {
    *divp = str_to_mrb_float("inf");
    *modp = str_to_mrb_float("nan");
    return;
  }
  mod = fmod(x, y);
  if (isinf(x) && !isinf(y) && !isnan(y))
    div = x;
  else
    div = (x - mod) / y;
  if (y*mod < 0) {
    mod += y;
    div -= 1.0;
  }
  if (modp) *modp = mod;
  if (divp) *divp = div;
}

/* 15.2.9.3.5  */
/*
 *  call-seq:
 *     flt % other        ->  float
 *     flt.modulo(other)  ->  float
 *
 *  Return the modulo after division of <code>flt</code> by <code>other</code>.
 *
 *     6543.21.modulo(137)      #=> 104.21
 *     6543.21.modulo(137.24)   #=> 92.9299999999996
 */

static mrb_value
flo_mod(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_float fy, mod;
  mrb_get_args(mrb, "o", &y);

  fy = mrb_to_flo(mrb, y);
  flodivmod(mrb, mrb_float(x), fy, 0, &mod);
  return mrb_float_value(mod);
}

/* 15.2.8.3.16 */
/*
 *  call-seq:
 *     num.eql?(numeric)  ->  true or false
 *
 *  Returns <code>true</code> if <i>num</i> and <i>numeric</i> are the
 *  same type and have equal values.
 *
 *     1 == 1.0          #=> true
 *     1.eql?(1.0)       #=> false
 *     (1.0).eql?(1.0)   #=> true
 */
static mrb_value
num_eql(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  
  mrb_get_args(mrb, "o", &y);
  if (mrb_type(x) != mrb_type(y)) return mrb_false_value();
  if (mrb_equal(mrb, x, y)) {
      return mrb_true_value();
  }
  else {
      return mrb_false_value();
  }
}

static mrb_value
num_equal(mrb_state *mrb, mrb_value x, mrb_value y)
{
  if (mrb_obj_equal(mrb, x, y)) return mrb_true_value();
  return mrb_funcall(mrb, y, "==", 1, x);
}

/* 15.2.9.3.7  */
/*
 *  call-seq:
 *     flt == obj  ->  true or false
 *
 *  Returns <code>true</code> only if <i>obj</i> has the same value
 *  as <i>flt</i>. Contrast this with <code>Float#eql?</code>, which
 *  requires <i>obj</i> to be a <code>Float</code>.
 *
 *     1.0 == 1   #=> true
 *
 */

static mrb_value
flo_eq(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  volatile mrb_float a, b;
  mrb_get_args(mrb, "o", &y);

  switch (mrb_type(y)) {
  case MRB_TT_FIXNUM:
    b = (mrb_float)mrb_fixnum(y);
    break;
  case MRB_TT_FLOAT:
    b = mrb_float(y);
    break;
  default:
    return num_equal(mrb, x, y);
  }
  a = mrb_float(x);
  return (a == b)?mrb_true_value():mrb_false_value();
}

/* 15.2.8.3.18 */
/*
 * call-seq:
 *   flt.hash  ->  integer
 *
 * Returns a hash code for this float.
 */
static mrb_value
flo_hash(mrb_state *mrb, mrb_value num)
{
  mrb_float d;
  char *c;
  int i, hash;

  d = (mrb_float)mrb_fixnum(num);
  /* normalize -0.0 to 0.0 */
  if (d == 0) d = 0.0;
  c = (char*)&d;
  for (hash=0, i=0; i<sizeof(mrb_float);i++) {
    hash = (hash * 971) ^ (unsigned char)c[i];
  }
  if (hash < 0) hash = -hash;
  return mrb_fixnum_value(hash);
}

/* 15.2.9.3.13 */
/*
 * call-seq:
 *   flt.to_f  ->  self
 *
 * As <code>flt</code> is already a float, returns +self+.
 */

static mrb_value
flo_to_f(mrb_state *mrb, mrb_value num)
{
  return num;
}

/* 15.2.9.3.11 */
/*
 *  call-seq:
 *     flt.infinite?  ->  nil, -1, +1
 *
 *  Returns <code>nil</code>, -1, or +1 depending on whether <i>flt</i>
 *  is finite, -infinity, or +infinity.
 *
 *     (0.0).infinite?        #=> nil
 *     (-1.0/0.0).infinite?   #=> -1
 *     (+1.0/0.0).infinite?   #=> 1
 */

static mrb_value
flo_infinite_p(mrb_state *mrb, mrb_value num)
{
  mrb_float value = mrb_float(num);

  if (isinf(value)) {
    return mrb_fixnum_value( value < 0 ? -1 : 1 );
  }
  return mrb_nil_value();
}

/* 15.2.9.3.9  */
/*
 *  call-seq:
 *     flt.finite?  ->  true or false
 *
 *  Returns <code>true</code> if <i>flt</i> is a valid IEEE floating
 *  point number (it is not infinite, and <code>nan?</code> is
 *  <code>false</code>).
 *
 */

static mrb_value
flo_finite_p(mrb_state *mrb, mrb_value num)
{
  mrb_float value = mrb_float(num);

  if (isinf(value) || isnan(value))
    return mrb_false_value();
  return mrb_true_value();
}

/* 15.2.9.3.10 */
/*
 *  call-seq:
 *     flt.floor  ->  integer
 *
 *  Returns the largest integer less than or equal to <i>flt</i>.
 *
 *     1.2.floor      #=> 1
 *     2.0.floor      #=> 2
 *     (-1.2).floor   #=> -2
 *     (-2.0).floor   #=> -2
 */

static mrb_value
flo_floor(mrb_state *mrb, mrb_value num)
{
  mrb_float f = floor(mrb_float(num));

  if (!FIXABLE(f)) {
    return mrb_float_value(f);
  }
  return mrb_fixnum_value((mrb_int)f);
}

/* 15.2.9.3.8  */
/*
 *  call-seq:
 *     flt.ceil  ->  integer
 *
 *  Returns the smallest <code>Integer</code> greater than or equal to
 *  <i>flt</i>.
 *
 *     1.2.ceil      #=> 2
 *     2.0.ceil      #=> 2
 *     (-1.2).ceil   #=> -1
 *     (-2.0).ceil   #=> -2
 */

static mrb_value
flo_ceil(mrb_state *mrb, mrb_value num)
{
  mrb_float f = ceil(mrb_float(num));

  if (!FIXABLE(f)) {
    return mrb_float_value(f);
  }
  return mrb_fixnum_value((mrb_int)f);
}

/* 15.2.9.3.12 */
/*
 *  call-seq:
 *     flt.round([ndigits])  ->  integer or float
 *
 *  Rounds <i>flt</i> to a given precision in decimal digits (default 0 digits).
 *  Precision may be negative.  Returns a floating point number when ndigits
 *  is more than zero.
 *
 *     1.4.round      #=> 1
 *     1.5.round      #=> 2
 *     1.6.round      #=> 2
 *     (-1.5).round   #=> -2
 *
 *     1.234567.round(2)  #=> 1.23
 *     1.234567.round(3)  #=> 1.235
 *     1.234567.round(4)  #=> 1.2346
 *     1.234567.round(5)  #=> 1.23457
 *
 *     34567.89.round(-5) #=> 0
 *     34567.89.round(-4) #=> 30000
 *     34567.89.round(-3) #=> 35000
 *     34567.89.round(-2) #=> 34600
 *     34567.89.round(-1) #=> 34570
 *     34567.89.round(0)  #=> 34568
 *     34567.89.round(1)  #=> 34567.9
 *     34567.89.round(2)  #=> 34567.89
 *     34567.89.round(3)  #=> 34567.89
 *
 */

static mrb_value
flo_round(mrb_state *mrb, mrb_value num)
{
  double number, f;
  mrb_int ndigits = 0;
  int i;

  mrb_get_args(mrb, "|i", &ndigits);
  number = mrb_float(num);
  f = 1.0;
  i = abs(ndigits);
  while  (--i >= 0)
    f = f*10.0;

  if (isinf(f)) {
    if (ndigits < 0) number = 0;
  }
  else {
    double d;

    if (ndigits < 0) number /= f;
    else number *= f;

    /* home-made inline implementation of round(3) */
    if (number > 0.0) {
        d = floor(number);
        number = d + (number - d >= 0.5);
    }
    else if (number < 0.0) {
        d = ceil(number);
        number = d - (d - number >= 0.5);
    }

    if (ndigits < 0) number *= f;
    else number /= f;
  }
  if (ndigits > 0) return mrb_float_value(number);
  return mrb_fixnum_value((mrb_int)number);
}

/* 15.2.9.3.14 */
/* 15.2.9.3.15 */
/*
 *  call-seq:
 *     flt.to_i      ->  integer
 *     flt.to_int    ->  integer
 *     flt.truncate  ->  integer
 *
 *  Returns <i>flt</i> truncated to an <code>Integer</code>.
 */

static mrb_value
flo_truncate(mrb_state *mrb, mrb_value num)
{
  mrb_float f = mrb_float(num);

  if (f > 0.0) f = floor(f);
  if (f < 0.0) f = ceil(f);

  if (!FIXABLE(f)) {
    return mrb_float_value(f);
  }
  return mrb_fixnum_value((mrb_int)f);
}

/* 15.2.8.3.17 */
/*
 *  call-seq:
 *     num.floor  ->  integer
 *
 *  Returns the largest integer less than or equal to <i>num</i>.
 *  <code>Numeric</code> implements this by converting <i>anInteger</i>
 *  to a <code>Float</code> and invoking <code>Float#floor</code>.
 *
 *     1.floor      #=> 1
 *     (-1).floor   #=> -1
 */

static mrb_value
num_floor(mrb_state *mrb, mrb_value num)
{
  return flo_floor(mrb, mrb_Float(mrb, num));
}

/* 15.2.8.3.20 */
/*
 *  call-seq:
 *     num.round([ndigits])  ->  integer or float
 *
 *  Rounds <i>num</i> to a given precision in decimal digits (default 0 digits).
 *  Precision may be negative.  Returns a floating point number when ndigits
 *  is more than zero.  <code>Numeric</code> implements this by converting itself
 *  to a <code>Float</code> and invoking <code>Float#round</code>.
 */

static mrb_value
num_round(mrb_state *mrb, mrb_value num)
{
  return flo_round(mrb, mrb_Float(mrb, num));
}

/*
 * Document-class: Integer
 *
 *  <code>Integer</code> is the basis for the two concrete classes that
 *  hold whole numbers, <code>Bignum</code> and <code>Fixnum</code>.
 *
 */


/* 15.2.8.3.14 */
/* 15.2.8.3.24 */
/* 15.2.8.3.26 */
/*
 *  call-seq:
 *     int.to_i      ->  integer
 *     int.to_int    ->  integer
 *     int.floor     ->  integer
 *     int.ceil      ->  integer
 *     int.round     ->  integer
 *     int.truncate  ->  integer
 *
 *  As <i>int</i> is already an <code>Integer</code>, all these
 *  methods simply return the receiver.
 */

static mrb_value
int_to_i(mrb_state *mrb, mrb_value num)
{
  return num;
}

/* 15.2.8.3.21 */
/*
 *  call-seq:
 *     fixnum.next  ->  integer
 *     fixnum.succ  ->  integer
 *
 *  Returns the <code>Integer</code> equal to <i>int</i> + 1.
 *
 *     1.next      #=> 2
 *     (-1).next   #=> 0
 */

static mrb_value
fix_succ(mrb_state *mrb, mrb_value num)
{
  return mrb_fixnum_value(mrb_fixnum(num)+1);
}

/* 15.2.8.3.19 */
/*
 *  call-seq:
 *     int.next  ->  integer
 *     int.succ  ->  integer
 *
 *  Returns the <code>Integer</code> equal to <i>int</i> + 1.
 *
 *     1.next      #=> 2
 *     (-1).next   #=> 0
 */
static mrb_value
int_succ(mrb_state *mrb, mrb_value num)
{
  if (mrb_fixnum_p(num)) return fix_succ(mrb, num);
  return mrb_funcall(mrb, num, "+", 1, mrb_fixnum_value(1));
}

#define SQRT_INT_MAX ((mrb_int)1<<((sizeof(mrb_int)*CHAR_BIT-1)/2))
/*tests if N*N would overflow*/
#define FIT_SQRT_INT(n) (((n)<SQRT_INT_MAX)&&((n)>=-SQRT_INT_MAX))

mrb_value
mrb_fixnum_mul(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;
  
  a = mrb_fixnum(x);
  if (a == 0) return x;
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    b = mrb_fixnum(y);
    if (FIT_SQRT_INT(a) && FIT_SQRT_INT(b))
      return mrb_fixnum_value(a*b);
    c = a * b;
    if (a != 0 && c/a != b) {
      return mrb_float_value((mrb_float)a*(mrb_float)b);
    }
    return mrb_fixnum_value(c);;
  }
  return mrb_float_value((mrb_float)a * mrb_to_flo(mrb, y));
}

/* 15.2.8.3.3  */
/*
 * call-seq:
 *   fix * numeric  ->  numeric_result
 *
 * Performs multiplication: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */

static mrb_value
fix_mul(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_get_args(mrb, "o", &y);
  return mrb_fixnum_mul(mrb, x, y);
}

static void
fixdivmod(mrb_state *mrb, mrb_int x, mrb_int y, mrb_int *divp, mrb_int *modp)
{
  mrb_int div, mod;

  if (y < 0) {
    if (x < 0)
      div = -x / -y;
    else
      div = - (x / -y);
  }
  else {
    if (x < 0)
      div = - (-x / y);
    else
      div = x / y;
  }
  mod = x - div*y;
  if ((mod < 0 && y > 0) || (mod > 0 && y < 0)) {
    mod += y;
    div -= 1;
  }
  if (divp) *divp = div;
  if (modp) *modp = mod;
}

/* 15.2.8.3.5  */
/*
 *  call-seq:
 *    fix % other        ->  real
 *    fix.modulo(other)  ->  real
 *
 *  Returns <code>fix</code> modulo <code>other</code>.
 *  See <code>numeric.divmod</code> for more information.
 */

static mrb_value
fix_mod(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int a, b;

  mrb_get_args(mrb, "o", &y);
  a = mrb_fixnum(x);
  if (mrb_fixnum_p(y) && (b=mrb_fixnum(y)) != 0) {
    mrb_int mod;

    if (mrb_fixnum(y) == 0) {
      return mrb_float_value(str_to_mrb_float("nan"));
    }
    fixdivmod(mrb, a, mrb_fixnum(y), 0, &mod);
    return mrb_fixnum_value(mod);
  }
  else {
    mrb_float mod;

    flodivmod(mrb, (mrb_float)a, mrb_to_flo(mrb, y), 0, &mod);
    return mrb_float_value(mod);
  }
}

/*
 *  call-seq:
 *     fix.divmod(numeric)  ->  array
 *
 *  See <code>Numeric#divmod</code>.
 */
static mrb_value
fix_divmod(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_get_args(mrb, "o", &y);

  if (mrb_fixnum_p(y)) {
    mrb_int div, mod;

    if (mrb_fixnum(y) == 0) {
      return mrb_assoc_new(mrb, mrb_float_value(str_to_mrb_float("inf")),
			        mrb_float_value(str_to_mrb_float("nan")));
    }
    fixdivmod(mrb, mrb_fixnum(x), mrb_fixnum(y), &div, &mod);
    return mrb_assoc_new(mrb, mrb_fixnum_value(div), mrb_fixnum_value(mod));
  }
  else {
    mrb_float div, mod;
    mrb_value a, b;

    flodivmod(mrb, (mrb_float)mrb_fixnum(x), mrb_to_flo(mrb, y), &div, &mod);
    a = mrb_float_value((mrb_int)div);
    b = mrb_float_value(mod);
    return mrb_assoc_new(mrb, a, b);
  }
}

/* 15.2.8.3.7  */
/*
 * call-seq:
 *   fix == other  ->  true or false
 *
 * Return <code>true</code> if <code>fix</code> equals <code>other</code>
 * numerically.
 *
 *   1 == 2      #=> false
 *   1 == 1.0    #=> true
 */

static mrb_value
fix_equal(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_get_args(mrb, "o", &y);

  if (mrb_obj_equal(mrb, x, y)) return mrb_true_value();
  switch (mrb_type(y)) {
  case MRB_TT_FLOAT:
    if ((mrb_float)mrb_fixnum(x) == mrb_float(y))
      return mrb_true_value();
    /* fall through */
  case MRB_TT_FIXNUM:
  default:
    return mrb_false_value();
  }
}

/* 15.2.8.3.8  */
/*
 * call-seq:
 *   ~fix  ->  integer
 *
 * One's complement: returns a number where each bit is flipped.
 *   ex.0---00001 (1)-> 1---11110 (-2)
 *   ex.0---00010 (2)-> 1---11101 (-3)
 *   ex.0---00100 (4)-> 1---11011 (-5)
 */

static mrb_value
fix_rev(mrb_state *mrb, mrb_value num)
{
    mrb_int val = mrb_fixnum(num);

    val = ~val;
    return mrb_fixnum_value(val);
}

static mrb_value
bit_coerce(mrb_state *mrb, mrb_value x)
{
    while (!mrb_fixnum_p(x)) {
        if (mrb_float_p(x)) {
            mrb_raise(mrb, E_TYPE_ERROR, "can't convert Float into Integer");
        }
        x = mrb_to_int(mrb, x);
    }
    return x;
}

/* 15.2.8.3.9  */
/*
 * call-seq:
 *   fix & integer  ->  integer_result
 *
 * Bitwise AND.
 */

static mrb_value
fix_and(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int val;
  mrb_get_args(mrb, "o", &y);

  y = bit_coerce(mrb, y);
  val = mrb_fixnum(x) & mrb_fixnum(y);
  return mrb_fixnum_value(val);
}

/* 15.2.8.3.10 */
/*
 * call-seq:
 *   fix | integer  ->  integer_result
 *
 * Bitwise OR.
 */

static mrb_value
fix_or(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int val;
  mrb_get_args(mrb, "o", &y);

  y = bit_coerce(mrb, y);
  val = mrb_fixnum(x) | mrb_fixnum(y);
  return mrb_fixnum_value(val);
}

/* 15.2.8.3.11 */
/*
 * call-seq:
 *   fix ^ integer  ->  integer_result
 *
 * Bitwise EXCLUSIVE OR.
 */

static mrb_value
fix_xor(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int val;

  mrb_get_args(mrb, "o", &y);
  y = bit_coerce(mrb, y);
  val = mrb_fixnum(x) ^ mrb_fixnum(y);
  return mrb_fixnum_value(val);
}

static mrb_value
lshift(mrb_state *mrb, mrb_int val, int width)
{
  if (width > (sizeof(mrb_int)*CHAR_BIT-1)) {
      mrb_raisef(mrb, E_RANGE_ERROR, "width(%d) > (%d:sizeof(mrb_int)*CHAR_BIT-1)", width,
		sizeof(mrb_int)*CHAR_BIT-1);
  }
  val = val << width;
  return mrb_fixnum_value(val);
}

static mrb_value
rshift(mrb_int val, int i)
{
    if (i >= sizeof(mrb_int)*CHAR_BIT-1) {
        if (val < 0) return mrb_fixnum_value(-1);
        return mrb_fixnum_value(0);
    }
    val = RSHIFT(val, i);
    return mrb_fixnum_value(val);
}

/* 15.2.8.3.12 */
/*
 * call-seq:
 *   fix << count  ->  integer
 *
 * Shifts _fix_ left _count_ positions (right if _count_ is negative).
 */

static mrb_value
fix_lshift(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int val, width;

  mrb_get_args(mrb, "o", &y);
  val = mrb_fixnum(x);
  y = bit_coerce(mrb, y);
  width = mrb_fixnum(y);
  if (width < 0)
      return rshift(val, -width);
  return lshift(mrb, val, width);
}

/* 15.2.8.3.13 */
/*
 * call-seq:
 *   fix >> count  ->  integer
 *
 * Shifts _fix_ right _count_ positions (left if _count_ is negative).
 */

static mrb_value
fix_rshift(mrb_state *mrb, mrb_value x)
{
  mrb_value y;
  mrb_int i, val;

  mrb_get_args(mrb, "o", &y);
  val = mrb_fixnum(x);
  y = bit_coerce(mrb, y);
  i = mrb_fixnum(y);
    if (i == 0) return x;
    if (i < 0)
        return lshift(mrb, val, -i);
    return rshift(val, i);
}

/* 15.2.8.3.23 */
/*
 *  call-seq:
 *     fix.to_f  ->  float
 *
 *  Converts <i>fix</i> to a <code>Float</code>.
 *
 */

static mrb_value
fix_to_f(mrb_state *mrb, mrb_value num)
{
    mrb_float val;

    val = (mrb_float)mrb_fixnum(num);

    return mrb_float_value(val);
}

/*
 *  Document-class: FloatDomainError
 *
 *  Raised when attempting to convert special float values
 *  (in particular infinite or NaN)
 *  to numerical classes which don't support them.
 *
 *     Float::INFINITY.to_r
 *
 *  <em>raises the exception:</em>
 *
 *     FloatDomainError: Infinity
 */
/* ------------------------------------------------------------------------*/
static mrb_int
flt2big(mrb_state *mrb, mrb_float d)
{
  mrb_int z;

  if (isinf(d)) {
    mrb_raise(mrb, E_FLOATDOMAIN_ERROR, d < 0 ? "-Infinity" : "Infinity");
  }
  if (isnan(d)) {
    mrb_raise(mrb, E_FLOATDOMAIN_ERROR, "NaN");
  }
  z = (mrb_int)d;
  return z;
}

mrb_value
mrb_flt2big(mrb_state *mrb, mrb_float d)
{
  return mrb_fixnum_value(flt2big(mrb, d));
}

mrb_value
mrb_fixnum_plus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;
  
  a = mrb_fixnum(x);
  if (a == 0) return y;
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    b = mrb_fixnum(y);
    c = a + b;
    if (((a < 0) ^ (b < 0)) == 0 && (a < 0) != (c < 0)) {
      /* integer overflow */
      return mrb_float_value((mrb_float)a + (mrb_float)b);
    }
    return mrb_fixnum_value(c);
  }
  return mrb_float_value((mrb_float)a + mrb_to_flo(mrb, y));
}

/* 15.2.8.3.1  */
/*
 * call-seq:
 *   fix + numeric  ->  numeric_result
 *
 * Performs addition: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */
static mrb_value
fix_plus(mrb_state *mrb, mrb_value self)
{
  mrb_value other;

  mrb_get_args(mrb, "o", &other);
  return mrb_fixnum_plus(mrb, self, other);
}

mrb_value
mrb_fixnum_minus(mrb_state *mrb, mrb_value x, mrb_value y)
{
  mrb_int a;
  
  a = mrb_fixnum(x);
  if (mrb_fixnum_p(y)) {
    mrb_int b, c;

    b = mrb_fixnum(y);
    c = a - b;
    if (((a < 0) ^ (b < 0)) != 0 && (a < 0) != (c < 0)) {
      /* integer overflow */
      return mrb_float_value((mrb_float)a - (mrb_float)b);
    }
    return mrb_fixnum_value(c);
  }
  return mrb_float_value((mrb_float)a - mrb_to_flo(mrb, y));
}

/* 15.2.8.3.2  */
/* 15.2.8.3.16 */
/*
 * call-seq:
 *   fix - numeric  ->  numeric_result
 *
 * Performs subtraction: the class of the resulting object depends on
 * the class of <code>numeric</code> and on the magnitude of the
 * result.
 */
static mrb_value
fix_minus(mrb_state *mrb, mrb_value self)
{
  mrb_value other;

  mrb_get_args(mrb, "o", &other);
  return mrb_fixnum_minus(mrb, self, other);
}

/* 15.2.8.3.29 (x) */
/*
 * call-seq:
 *   fix > other     => true or false
 *
 * Returns <code>true</code> if the value of <code>fix</code> is
 * greater than that of <code>other</code>.
 */

mrb_value
mrb_fix2str(mrb_state *mrb, mrb_value x, int base)
{
  char buf[64], *b = buf + sizeof buf;
  mrb_int val = mrb_fixnum(x);
  int neg = 0;

  if (base < 2 || 36 < base) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid radix %d", base);
  }
  if (val == 0) {
    return mrb_str_new(mrb, "0", 1);
  }
  if (val < 0) {
    val = -val;
    neg = 1;
  }
  *--b = '\0';
  if (neg && val < 0) {
    do {
      *--b = mrb_digitmap[abs(val % base)];
    } while (val /= base);
  }
  else {
    do {
      *--b = mrb_digitmap[(int)(val % base)];
    } while (val /= base);
  }
  if (neg) {
    *--b = '-';
  }

  return mrb_str_new2(mrb, b);
}

/* 15.2.8.3.25 */
/*
 *  call-seq:
 *     fix.to_s(base=10)  ->  string
 *
 *  Returns a string containing the representation of <i>fix</i> radix
 *  <i>base</i> (between 2 and 36).
 *
 *     12345.to_s       #=> "12345"
 *     12345.to_s(2)    #=> "11000000111001"
 *     12345.to_s(8)    #=> "30071"
 *     12345.to_s(10)   #=> "12345"
 *     12345.to_s(16)   #=> "3039"
 *     12345.to_s(36)   #=> "9ix"
 *
 */
static mrb_value
fix_to_s(mrb_state *mrb, mrb_value self)
{
  mrb_int base = 10;

  mrb_get_args(mrb, "|i", &base);
  return mrb_fix2str(mrb, self, base);
}

/* 15.2.9.3.6  */
/*
 * call-seq:
 *     self.f <=> other.f    => -1, 0, +1
 *             <  => -1
 *             =  =>  0
 *             >  => +1
 *  Comparison---Returns -1, 0, or +1 depending on whether <i>fix</i> is
 *  less than, equal to, or greater than <i>numeric</i>. This is the
 *  basis for the tests in <code>Comparable</code>.
 */
static mrb_value
num_cmp(mrb_state *mrb, mrb_value self)
{
  mrb_value other;
  mrb_float x, y;

  mrb_get_args(mrb, "o", &other);

  x = mrb_to_flo(mrb, self);
  switch (mrb_type(other)) {
  case MRB_TT_FIXNUM:
    y = (mrb_float)mrb_fixnum(other);
    break;
  case MRB_TT_FLOAT:
    y = mrb_float(other);
    break;
  default:
    return mrb_nil_value();
  }
  if (x > y)
    return mrb_fixnum_value(1);
  else {
    if (x < y)
      return mrb_fixnum_value(-1);
    return mrb_fixnum_value(0);
  }
}

/* 15.2.9.3.1  */
/*
 * call-seq:
 *   float + other  ->  float
 *
 * Returns a new float which is the sum of <code>float</code>
 * and <code>other</code>.
 */
static mrb_value
flo_plus(mrb_state *mrb, mrb_value self)
{
  mrb_float x,  y;

  x = mrb_float(self);
  mrb_get_args(mrb, "f", &y);

  return mrb_float_value(x + y);
}
/* ------------------------------------------------------------------------*/
void
mrb_init_numeric(mrb_state *mrb)
{
  struct RClass *numeric, *integer, *fixnum, *fl;
  /* Numeric Class */
  numeric = mrb_define_class(mrb, "Numeric",  mrb->object_class);
  mrb_include_module(mrb, numeric, mrb_class_get(mrb, "Comparable"));

  mrb_define_method(mrb, numeric, "+@",       num_uplus,      ARGS_REQ(1));  /* 15.2.7.4.1  */
  mrb_define_method(mrb, numeric, "-@",       num_uminus,     ARGS_REQ(1));  /* 15.2.7.4.2  */
  mrb_define_method(mrb, numeric, "**",       num_pow,        ARGS_REQ(1));
  mrb_define_method(mrb, numeric, "/",        num_div,        ARGS_REQ(1));  /* 15.2.8.3.4  */
  mrb_define_method(mrb, numeric, "quo",      num_div,        ARGS_REQ(1));  /* 15.2.7.4.5 (x) */
  mrb_define_method(mrb, numeric, "abs",      num_abs,        ARGS_NONE());  /* 15.2.7.4.3  */
  mrb_define_method(mrb, numeric, "<=>",      num_cmp,        ARGS_REQ(1));  /* 15.2.9.3.6  */

  /* Integer Class */
  integer = mrb_define_class(mrb, "Integer",  numeric);
  fixnum = mrb->fixnum_class = mrb_define_class(mrb, "Fixnum", integer);

  mrb_undef_class_method(mrb,  fixnum, "new");
  mrb_define_method(mrb, fixnum,  "+",        fix_plus,          ARGS_REQ(1)); /* 15.2.8.3.1  */
  mrb_define_method(mrb, fixnum,  "-",        fix_minus,         ARGS_REQ(1)); /* 15.2.8.3.2  */
  mrb_define_method(mrb, fixnum,  "-@",       fix_uminus,        ARGS_REQ(1)); /* 15.2.7.4.2  */
  mrb_define_method(mrb, fixnum,  "*",        fix_mul,           ARGS_REQ(1)); /* 15.2.8.3.3  */
  mrb_define_method(mrb, fixnum,  "%",        fix_mod,           ARGS_REQ(1)); /* 15.2.8.3.5  */
  mrb_define_method(mrb, fixnum,  "==",       fix_equal,         ARGS_REQ(1)); /* 15.2.8.3.7  */
  mrb_define_method(mrb, fixnum,  "~",        fix_rev,           ARGS_NONE()); /* 15.2.8.3.8  */
  mrb_define_method(mrb, fixnum,  "&",        fix_and,           ARGS_REQ(1)); /* 15.2.8.3.9  */
  mrb_define_method(mrb, fixnum,  "|",        fix_or,            ARGS_REQ(1)); /* 15.2.8.3.10 */
  mrb_define_method(mrb, fixnum,  "^",        fix_xor,           ARGS_REQ(1)); /* 15.2.8.3.11 */
  mrb_define_method(mrb, fixnum,  "<<",       fix_lshift,        ARGS_REQ(1)); /* 15.2.8.3.12 */
  mrb_define_method(mrb, fixnum,  ">>",       fix_rshift,        ARGS_REQ(1)); /* 15.2.8.3.13 */
  mrb_define_method(mrb, fixnum,  "ceil",     int_to_i,          ARGS_NONE()); /* 15.2.8.3.14 */
  mrb_define_method(mrb, fixnum,  "eql?",     num_eql,           ARGS_REQ(1)); /* 15.2.8.3.16 */
  mrb_define_method(mrb, fixnum,  "floor",    num_floor,         ARGS_NONE()); /* 15.2.8.3.17 */
  mrb_define_method(mrb, fixnum,  "hash",     flo_hash,          ARGS_NONE()); /* 15.2.8.3.18 */
  mrb_define_method(mrb, fixnum,  "next",     int_succ,          ARGS_NONE()); /* 15.2.8.3.19 */
  mrb_define_method(mrb, fixnum,  "round",    num_round,         ARGS_ANY());  /* 15.2.8.3.20 */
  mrb_define_method(mrb, fixnum,  "succ",     fix_succ,          ARGS_NONE()); /* 15.2.8.3.21 */
  mrb_define_method(mrb, fixnum,  "to_f",     fix_to_f,          ARGS_NONE()); /* 15.2.8.3.23 */
  mrb_define_method(mrb, fixnum,  "to_i",     int_to_i,          ARGS_NONE()); /* 15.2.8.3.24 */
  mrb_define_method(mrb, fixnum,  "to_s",     fix_to_s,          ARGS_NONE()); /* 15.2.8.3.25 */
  mrb_define_method(mrb, fixnum,  "inspect",  fix_to_s,          ARGS_NONE());
  mrb_define_method(mrb, fixnum,  "truncate", int_to_i,          ARGS_NONE()); /* 15.2.8.3.26 */
  mrb_define_method(mrb, fixnum,  "divmod",   fix_divmod,        ARGS_REQ(1)); /* 15.2.8.3.30 (x) */

  /* Float Class */
  fl = mrb->float_class = mrb_define_class(mrb, "Float", numeric);
  mrb_undef_class_method(mrb,  fl, "new");
  mrb_define_method(mrb, fl,      "+",         flo_plus,         ARGS_REQ(1)); /* 15.2.9.3.1  */
  mrb_define_method(mrb, fl,      "-",         flo_minus,        ARGS_REQ(1)); /* 15.2.9.3.2  */
  mrb_define_method(mrb, fl,      "*",         flo_mul,          ARGS_REQ(1)); /* 15.2.9.3.3  */
  mrb_define_method(mrb, fl,      "%",         flo_mod,          ARGS_REQ(1)); /* 15.2.9.3.5  */
  mrb_define_method(mrb, fl,      "==",        flo_eq,           ARGS_REQ(1)); /* 15.2.9.3.7  */
  mrb_define_method(mrb, fl,      "ceil",      flo_ceil,         ARGS_NONE()); /* 15.2.9.3.8  */
  mrb_define_method(mrb, fl,      "finite?",   flo_finite_p,     ARGS_NONE()); /* 15.2.9.3.9  */
  mrb_define_method(mrb, fl,      "floor",     flo_floor,        ARGS_NONE()); /* 15.2.9.3.10 */
  mrb_define_method(mrb, fl,      "infinite?", flo_infinite_p,   ARGS_NONE()); /* 15.2.9.3.11 */
  mrb_define_method(mrb, fl,      "round",     flo_round,        ARGS_ANY());  /* 15.2.9.3.12 */
  mrb_define_method(mrb, fl,      "to_f",      flo_to_f,         ARGS_NONE()); /* 15.2.9.3.13 */
  mrb_define_method(mrb, fl,      "to_i",      flo_truncate,     ARGS_NONE()); /* 15.2.9.3.14 */
  mrb_define_method(mrb, fl,      "truncate",  flo_truncate,     ARGS_NONE()); /* 15.2.9.3.15 */

  mrb_define_method(mrb, fl,      "to_s",      flo_to_s,         ARGS_NONE()); /* 15.2.9.3.16(x) */
  mrb_define_method(mrb, fl,      "inspect",   flo_to_s,         ARGS_NONE());
}
