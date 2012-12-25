/*
** math.c - Math module
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/array.h"

#ifdef ENABLE_MATH
#include <math.h>

#define domain_error(msg) \
    mrb_raise(mrb, E_RANGE_ERROR, "Numerical argument is out of domain - " #msg);

/* math functions not provided under Microsoft Visual C++ */
#ifdef _MSC_VER

#define MATH_TOLERANCE 1E-12

#define asinh(x) log(x + sqrt(pow(x,2.0) + 1))
#define acosh(x) log(x + sqrt(pow(x,2.0) - 1))
#define atanh(x) (log(1+x) - log(1-x))/2.0
#define cbrt(x)  pow(x,1.0/3.0)

/* Declaration of complementary Error function */
double
erfc(double x);

/* 
** Implementations of error functions
** credits to http://www.digitalmars.com/archives/cplusplus/3634.html
*/

/* Implementation of Error function */
double 
erf(double x)
{
  static const double two_sqrtpi =  1.128379167095512574;
  double sum  = x;
  double term = x;
  double xsqr = x*x;
  int j= 1;
  if (fabs(x) > 2.2) {
    return 1.0 - erfc(x);
  }
  do {
    term *= xsqr/j;
    sum  -= term/(2*j+1);
    ++j;
    term *= xsqr/j;
    sum  += term/(2*j+1);
    ++j;
  } while (fabs(term/sum) > MATH_TOLERANCE);
  return two_sqrtpi*sum;
}

/* Implementation of complementary Error function */
double 
erfc(double x)
{
  static const double one_sqrtpi=  0.564189583547756287;        
  double a = 1; 
  double b = x;               
  double c = x; 
  double d = x*x+0.5;         
  double q1;
  double q2 = b/d;
  double n = 1.0;
  double t;
  if (fabs(x) < 2.2) {
    return 1.0 - erf(x);       
  }                              
  if (x < 0.0) { /*signbit(x)*/              
    return 2.0 - erfc(-x);     
  }                              
  do {
    t  = a*n+b*x;
    a  = b;
    b  = t;
    t  = c*n+d*x;
    c  = d;
    d  = t;
    n += 0.5;
    q1 = q2;
    q2 = b/d;
  } while (fabs(q1-q2)/q2 > MATH_TOLERANCE);
  return one_sqrtpi*exp(-x*x)*q2;
}

#endif

/*
  TRIGONOMETRIC FUNCTIONS
*/

/*
 *  call-seq:
 *     Math.sin(x)    -> float
 *
 *  Computes the sine of <i>x</i> (expressed in radians). Returns
 *  -1..1.
 */
static mrb_value
math_sin(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = sin(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.cos(x)    -> float
 *
 *  Computes the cosine of <i>x</i> (expressed in radians). Returns
 *  -1..1.
 */
static mrb_value
math_cos(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = cos(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.tan(x)    -> float
 *
 *  Returns the tangent of <i>x</i> (expressed in radians).
 */
static mrb_value
math_tan(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = tan(x);

  return mrb_float_value(x);
}

/*
  INVERSE TRIGONOMETRIC FUNCTIONS
*/

/*
 *  call-seq:
 *     Math.asin(x)    -> float
 *
 *  Computes the arc sine of <i>x</i>. Returns -{PI/2} .. {PI/2}.
 */
static mrb_value
math_asin(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = asin(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.acos(x)    -> float
 *
 *  Computes the arc cosine of <i>x</i>. Returns 0..PI.
 */
static mrb_value
math_acos(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = acos(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.atan(x)    -> float
 *
 *  Computes the arc tangent of <i>x</i>. Returns -{PI/2} .. {PI/2}.
 */
static mrb_value
math_atan(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = atan(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.atan2(y, x)  -> float
 *
 *  Computes the arc tangent given <i>y</i> and <i>x</i>. Returns
 *  -PI..PI.
 *
 *    Math.atan2(-0.0, -1.0) #=> -3.141592653589793
 *    Math.atan2(-1.0, -1.0) #=> -2.356194490192345
 *    Math.atan2(-1.0, 0.0)  #=> -1.5707963267948966
 *    Math.atan2(-1.0, 1.0)  #=> -0.7853981633974483
 *    Math.atan2(-0.0, 1.0)  #=> -0.0
 *    Math.atan2(0.0, 1.0)   #=> 0.0
 *    Math.atan2(1.0, 1.0)   #=> 0.7853981633974483
 *    Math.atan2(1.0, 0.0)   #=> 1.5707963267948966
 *    Math.atan2(1.0, -1.0)  #=> 2.356194490192345
 *    Math.atan2(0.0, -1.0)  #=> 3.141592653589793
 *
 */
static mrb_value
math_atan2(mrb_state *mrb, mrb_value obj)
{
  mrb_float x, y;

  mrb_get_args(mrb, "ff", &x, &y);
  x = atan2(x, y);

  return mrb_float_value(x);
}



/*
  HYPERBOLIC TRIG FUNCTIONS
*/
/*
 *  call-seq:
 *     Math.sinh(x)    -> float
 *
 *  Computes the hyperbolic sine of <i>x</i> (expressed in
 *  radians).
 */
static mrb_value
math_sinh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = sinh(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.cosh(x)    -> float
 *
 *  Computes the hyperbolic cosine of <i>x</i> (expressed in radians).
 */
static mrb_value
math_cosh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = cosh(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.tanh()    -> float
 *
 *  Computes the hyperbolic tangent of <i>x</i> (expressed in
 *  radians).
 */
static mrb_value
math_tanh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = tanh(x);

  return mrb_float_value(x);
}


/*
  INVERSE HYPERBOLIC TRIG FUNCTIONS
*/

/*
 *  call-seq:
 *     Math.asinh(x)    -> float
 *
 *  Computes the inverse hyperbolic sine of <i>x</i>.
 */
static mrb_value
math_asinh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  
  x = asinh(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.acosh(x)    -> float
 *
 *  Computes the inverse hyperbolic cosine of <i>x</i>.
 */
static mrb_value
math_acosh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = acosh(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.atanh(x)    -> float
 *
 *  Computes the inverse hyperbolic tangent of <i>x</i>.
 */
static mrb_value
math_atanh(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = atanh(x);

  return mrb_float_value(x);
}

/*
  EXPONENTIALS AND LOGARITHMS
*/
#if defined __CYGWIN__
# include <cygwin/version.h>
# if CYGWIN_VERSION_DLL_MAJOR < 1005
#  define nan(x) nan()
# endif
# define log(x) ((x) < 0.0 ? nan("") : log(x))
# define log10(x) ((x) < 0.0 ? nan("") : log10(x))
#endif

#ifndef log2
#ifndef HAVE_LOG2
double
log2(double x)
{
    return log10(x)/log10(2.0);
}
#else
extern double log2(double);
#endif
#endif

/*
 *  call-seq:
 *     Math.exp(x)    -> float
 *
 *  Returns e**x.
 *
 *    Math.exp(0)       #=> 1.0
 *    Math.exp(1)       #=> 2.718281828459045
 *    Math.exp(1.5)     #=> 4.4816890703380645
 *
 */
static mrb_value
math_exp(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = exp(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.log(numeric)    -> float
 *     Math.log(num,base)   -> float
 *
 *  Returns the natural logarithm of <i>numeric</i>.
 *  If additional second argument is given, it will be the base
 *  of logarithm.
 *
 *    Math.log(1)          #=> 0.0
 *    Math.log(Math::E)    #=> 1.0
 *    Math.log(Math::E**3) #=> 3.0
 *    Math.log(12,3)       #=> 2.2618595071429146
 *
 */
static mrb_value
math_log(mrb_state *mrb, mrb_value obj)
{
  mrb_float x, base;
  int argc;

  argc = mrb_get_args(mrb, "f|f", &x, &base);
  x = log(x);
  if (argc == 2) {
    x /= log(base);
  }
  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.log2(numeric)    -> float
 *
 *  Returns the base 2 logarithm of <i>numeric</i>.
 *
 *    Math.log2(1)      #=> 0.0
 *    Math.log2(2)      #=> 1.0
 *    Math.log2(32768)  #=> 15.0
 *    Math.log2(65536)  #=> 16.0
 *
 */
static mrb_value
math_log2(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = log2(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.log10(numeric)    -> float
 *
 *  Returns the base 10 logarithm of <i>numeric</i>.
 *
 *    Math.log10(1)       #=> 0.0
 *    Math.log10(10)      #=> 1.0
 *    Math.log10(10**100) #=> 100.0
 *
 */
static mrb_value
math_log10(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = log10(x);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.sqrt(numeric)    -> float
 *
 *  Returns the square root of <i>numeric</i>.
 *
 */
static mrb_value
math_sqrt(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;
  
  mrb_get_args(mrb, "f", &x);
  x = sqrt(x);
  
  return mrb_float_value(x);
}


/*
 *  call-seq:
 *     Math.cbrt(numeric)    -> float
 *
 *  Returns the cube root of <i>numeric</i>.
 *
 *    -9.upto(9) {|x|
 *      p [x, Math.cbrt(x), Math.cbrt(x)**3]
 *    }
 *    #=>
 *    [-9, -2.0800838230519, -9.0]
 *    [-8, -2.0, -8.0]
 *    [-7, -1.91293118277239, -7.0]
 *    [-6, -1.81712059283214, -6.0]
 *    [-5, -1.7099759466767, -5.0]
 *    [-4, -1.5874010519682, -4.0]
 *    [-3, -1.44224957030741, -3.0]
 *    [-2, -1.25992104989487, -2.0]
 *    [-1, -1.0, -1.0]
 *    [0, 0.0, 0.0]
 *    [1, 1.0, 1.0]
 *    [2, 1.25992104989487, 2.0]
 *    [3, 1.44224957030741, 3.0]
 *    [4, 1.5874010519682, 4.0]
 *    [5, 1.7099759466767, 5.0]
 *    [6, 1.81712059283214, 6.0]
 *    [7, 1.91293118277239, 7.0]
 *    [8, 2.0, 8.0]
 *    [9, 2.0800838230519, 9.0]
 *
 */
static mrb_value
math_cbrt(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = cbrt(x);

  return mrb_float_value(x);
}


/*
 *  call-seq:
 *     Math.frexp(numeric)    -> [ fraction, exponent ]
 *
 *  Returns a two-element array containing the normalized fraction (a
 *  <code>Float</code>) and exponent (a <code>Fixnum</code>) of
 *  <i>numeric</i>.
 *
 *     fraction, exponent = Math.frexp(1234)   #=> [0.6025390625, 11]
 *     fraction * 2**exponent                  #=> 1234.0
 */
static mrb_value
math_frexp(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;
  int exp;
  
  mrb_get_args(mrb, "f", &x);
  x = frexp(x, &exp);

  return mrb_assoc_new(mrb, mrb_float_value(x), mrb_fixnum_value(exp));
}

/*
 *  call-seq:
 *     Math.ldexp(flt, int) -> float
 *
 *  Returns the value of <i>flt</i>*(2**<i>int</i>).
 *
 *     fraction, exponent = Math.frexp(1234)
 *     Math.ldexp(fraction, exponent)   #=> 1234.0
 */
static mrb_value
math_ldexp(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;
  mrb_int   i;

  mrb_get_args(mrb, "fi", &x, &i);
  x = ldexp(x, i);

  return mrb_float_value(x);
}

/*
 *  call-seq:
 *     Math.hypot(x, y)    -> float
 *
 *  Returns sqrt(x**2 + y**2), the hypotenuse of a right-angled triangle
 *  with sides <i>x</i> and <i>y</i>.
 *
 *     Math.hypot(3, 4)   #=> 5.0
 */
static mrb_value
math_hypot(mrb_state *mrb, mrb_value obj)
{
  mrb_float x, y;

  mrb_get_args(mrb, "ff", &x, &y);
  x = hypot(x, y);

  return mrb_float_value(x);
}

/*
 * call-seq:
 *    Math.erf(x)  -> float
 *
 *  Calculates the error function of x.
 */
static mrb_value
math_erf(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = erf(x);

  return mrb_float_value(x);
}


/*
 * call-seq:
 *    Math.erfc(x)  -> float
 *
 *  Calculates the complementary error function of x.
 */
static mrb_value
math_erfc(mrb_state *mrb, mrb_value obj)
{
  mrb_float x;

  mrb_get_args(mrb, "f", &x);
  x = erfc(x);

  return mrb_float_value(x);
}

/* ------------------------------------------------------------------------*/
void
mrb_init_math(mrb_state *mrb)
{
  struct RClass *mrb_math;
  mrb_math = mrb_define_module(mrb, "Math");
  
#ifdef M_PI
  mrb_define_const(mrb, mrb_math, "PI", mrb_float_value(M_PI));
#else
  mrb_define_const(mrb, mrb_math, "PI", mrb_float_value(atan(1.0)*4.0));
#endif
  
#ifdef M_E
  mrb_define_const(mrb, mrb_math, "E", mrb_float_value(M_E));
#else
  mrb_define_const(mrb, mrb_math, "E", mrb_float_value(exp(1.0)));
#endif

#ifdef MRB_USE_FLOAT
  mrb_define_const(mrb, mrb_math, "TOLERANCE", mrb_float_value(1e-5));
#else
  mrb_define_const(mrb, mrb_math, "TOLERANCE", mrb_float_value(1e-12));
#endif

  mrb_define_module_function(mrb, mrb_math, "sin", math_sin, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "cos", math_cos, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "tan", math_tan, ARGS_REQ(1));

  mrb_define_module_function(mrb, mrb_math, "asin", math_asin, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "acos", math_acos, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "atan", math_atan, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "atan2", math_atan2, ARGS_REQ(2));
  
  mrb_define_module_function(mrb, mrb_math, "sinh", math_sinh, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "cosh", math_cosh, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "tanh", math_tanh, ARGS_REQ(1));

  mrb_define_module_function(mrb, mrb_math, "asinh", math_asinh, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "acosh", math_acosh, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "atanh", math_atanh, ARGS_REQ(1));

  mrb_define_module_function(mrb, mrb_math, "exp", math_exp, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "log", math_log, ARGS_REQ(1)|ARGS_OPT(1));
  mrb_define_module_function(mrb, mrb_math, "log2", math_log2, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "log10", math_log10, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "sqrt", math_sqrt, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "cbrt", math_cbrt, ARGS_REQ(1));

  mrb_define_module_function(mrb, mrb_math, "frexp", math_frexp, ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "ldexp", math_ldexp, ARGS_REQ(2));

  mrb_define_module_function(mrb, mrb_math, "hypot", math_hypot, ARGS_REQ(2));

  mrb_define_module_function(mrb, mrb_math, "erf",  math_erf,  ARGS_REQ(1));
  mrb_define_module_function(mrb, mrb_math, "erfc", math_erfc, ARGS_REQ(1));
}
#endif	/* ENABLE_MATH */
