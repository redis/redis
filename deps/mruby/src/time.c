/*
** time.c - Time class
**
** See Copyright Notice in mruby.h
*/


#include "mruby.h"
#ifdef ENABLE_TIME
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "mruby/class.h"
#include "mruby/data.h"

/** Time class configuration */

/* gettimeofday(2) */
/* C99 does not have gettimeofday that is required to retrieve microseconds */
/* uncomment following macro on platforms without gettimeofday(2) */
/* #define NO_GETTIMEOFDAY */

/* gmtime(3) */
/* C99 does not have reentrant gmtime_r() so it might cause troubles under */
/* multi-threading environment.  undef following macro on platforms that */
/* does not have gmtime_r() and localtime_r(). */
/* #define NO_GMTIME_R */

#ifdef _WIN32
#if _MSC_VER
/* Win32 platform do not provide gmtime_r/localtime_r; emulate them using gmtime_s/localtime_s */
#define gmtime_r(tp, tm)    ((gmtime_s((tm), (tp)) == 0) ? (tm) : NULL)
#define localtime_r(tp, tm)    ((localtime_s((tm), (tp)) == 0) ? (tm) : NULL)
#else
#define NO_GMTIME_R
#endif
#endif

/* timegm(3) */
/* mktime() creates tm structure for localtime; timegm() is for UTF time */
/* define following macro to use probably faster timegm() on the platform */
/* #define USE_SYSTEM_TIMEGM */

/** end of Time class configuration */

#ifndef NO_GETTIMEOFDAY
#include <sys/time.h>
#endif
#ifdef NO_GMTIME_R
#define gmtime_r(t,r) gmtime(t)
#define localtime_r(t,r) (tzset(),localtime(t))
#endif

#ifndef USE_SYSTEM_TIMEGM
#define timegm my_timgm

static unsigned int
is_leapyear(unsigned int y)
{
  return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

static time_t
timegm(struct tm *tm)
{
  static const unsigned int ndays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
  };
  time_t r = 0;
  int i;
  unsigned int *nday = (unsigned int*) ndays[is_leapyear(tm->tm_year+1900)];

  for (i = 70; i < tm->tm_year; ++i)
    r += is_leapyear(i+1900) ? 366*24*60*60 : 365*24*60*60;
  for (i = 0; i < tm->tm_mon; ++i)
    r += nday[i] * 24 * 60 * 60;
  r += (tm->tm_mday - 1) * 24 * 60 * 60;
  r += tm->tm_hour * 60 * 60;
  r += tm->tm_min * 60;
  r += tm->tm_sec;
  return r;
}
#endif

/* Since we are limited to using ISO C89, this implementation is based
* on time_t. That means the resolution of time is only precise to the
* second level. Also, there are only 2 timezones, namely UTC and LOCAL.
*/

#ifndef mrb_bool_value
#define mrb_bool_value(val) ((val) ? mrb_true_value() : mrb_false_value())
#endif


enum mrb_timezone {
  MRB_TIMEZONE_NONE   = 0,
  MRB_TIMEZONE_UTC    = 1,
  MRB_TIMEZONE_LOCAL  = 2,
  MRB_TIMEZONE_LAST   = 3
};

static const char *timezone_names[] = {
  "none",
  "UTC",
  "LOCAL",
   NULL
};

static const char *mon_names[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static const char *wday_names[] = {
  "Sun", "Mon", "Tus", "Wed", "Thu", "Fri", "Sat",
};

struct mrb_time {
  time_t              sec;
  time_t              usec;
  enum mrb_timezone   timezone;
  struct tm           datetime;
};

static void
mrb_time_free(mrb_state *mrb, void *ptr)
{
  mrb_free(mrb, ptr);
}

static struct mrb_data_type mrb_time_type = { "Time", mrb_time_free };

/** Updates the datetime of a mrb_time based on it's timezone and
seconds setting. Returns self on cussess, NULL of failure. */
struct mrb_time*
mrb_time_update_datetime(struct mrb_time *self)
{
  struct tm *aid;

  if (self->timezone == MRB_TIMEZONE_UTC) {
    aid = gmtime_r(&self->sec, &self->datetime);
  }
  else {
    aid = localtime_r(&self->sec, &self->datetime);
  }
  if (!aid) return NULL;
#ifdef NO_GMTIME_R
  self->datetime = *aid; // copy data
#endif

  return self;
}

static mrb_value
mrb_time_wrap(mrb_state *mrb, struct RClass *tc, struct mrb_time *tm)
{
  return mrb_obj_value(Data_Wrap_Struct(mrb, tc, &mrb_time_type, tm));
}


/* Allocates a mrb_time object and initializes it. */
static struct mrb_time*
mrb_time_alloc(mrb_state *mrb, double sec, double usec, enum mrb_timezone timezone)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_malloc(mrb, sizeof(struct mrb_time));
  tm->sec  = (time_t)sec;
  tm->usec = (sec - tm->sec) * 1.0e6 + usec;
  while (tm->usec < 0) {
    tm->sec--;
    tm->usec += 1.0e6;
  }
  while (tm->usec > 1.0e6) {
    tm->sec++;
    tm->usec -= 1.0e6;
  }
  tm->timezone = timezone;
  mrb_time_update_datetime(tm);

  return tm;
}

static mrb_value
mrb_time_make(mrb_state *mrb, struct RClass *c, double sec, double usec, enum mrb_timezone timezone)
{
  return mrb_time_wrap(mrb, c, mrb_time_alloc(mrb, sec, usec, timezone));
}

static struct mrb_time*
current_mrb_time(mrb_state *mrb)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_malloc(mrb, sizeof(*tm));
#ifdef NO_GETTIMEOFDAY
  {
    static time_t last_sec = 0, last_usec = 0;

    tm->sec  = time(NULL);
    if (tm->sec != last_sec) {
      last_sec = tm->sec;
      last_usec = 0;
    }
    else {
      /* add 1 usec to differentiate two times */
      last_usec += 1;
    }
    tm->usec = last_usec;
  }
#else
  {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tm->sec = tv.tv_sec;
    tm->usec = tv.tv_usec;
  }
#endif
  tm->timezone = MRB_TIMEZONE_LOCAL;
  mrb_time_update_datetime(tm);

  return tm;
}

/* Allocates a new Time object with given millis value. */
static mrb_value
mrb_time_now(mrb_state *mrb, mrb_value self)
{
  return mrb_time_wrap(mrb, mrb_class_ptr(self), current_mrb_time(mrb));
}

/* 15.2.19.6.1 */
/* Creates an instance of time at the given time in seconds, etc. */
static mrb_value
mrb_time_at(mrb_state *mrb, mrb_value self)
{
  mrb_float f, f2 = 0;

  mrb_get_args(mrb, "f|f", &f, &f2);
  return mrb_time_make(mrb, mrb_class_ptr(self), f, f2, MRB_TIMEZONE_LOCAL);
}

static struct mrb_time*
time_mktime(mrb_state *mrb, mrb_int ayear, mrb_int amonth, mrb_int aday,
	    mrb_int ahour, mrb_int amin, mrb_int asec, mrb_int ausec,
	    enum mrb_timezone timezone)
{
  time_t nowsecs;
  struct tm nowtime = { 0 };

  nowtime.tm_year  = (int)ayear  - 1900;
  nowtime.tm_mon   = (int)amonth - 1;
  nowtime.tm_mday  = (int)aday;
  nowtime.tm_hour  = (int)ahour;
  nowtime.tm_min   = (int)amin;
  nowtime.tm_sec   = (int)asec;
  nowtime.tm_isdst = -1;
  if (timezone == MRB_TIMEZONE_UTC) {
    nowsecs = timegm(&nowtime);
  }
  else {
    nowsecs = mktime(&nowtime);
  }
  if (nowsecs < 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Not a valid time.");
  }

  return mrb_time_alloc(mrb, nowsecs, ausec, timezone);
}

/* 15.2.19.6.2 */
/* Creates an instance of time at the given time in UTC. */
static mrb_value
mrb_time_gm(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0, amin = 0, asec = 0, ausec = 0;

  mrb_get_args(mrb, "i|iiiiii",
                &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  return mrb_time_wrap(mrb, mrb_class_ptr(self),
		       time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_UTC));
}


/* 15.2.19.6.3 */
/* Creates an instance of time at the given time in local time zone. */
static mrb_value
mrb_time_local(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0, amin = 0, asec = 0, ausec = 0;

  mrb_get_args(mrb, "i|iiiiii",
                &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  return mrb_time_wrap(mrb, mrb_class_ptr(self),
		       time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_LOCAL));
}


static mrb_value
mrb_time_eq(mrb_state *mrb, mrb_value self)
{
  mrb_value other;
  struct mrb_time *tm1, *tm2;

  mrb_get_args(mrb, "o", &other);
  tm1 = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  tm2 = (struct mrb_time *)mrb_get_datatype(mrb, other, &mrb_time_type);
  if (!tm1 || !tm2) return mrb_false_value();
  if (tm1->sec == tm2->sec && tm1->usec == tm2->usec) {
    return mrb_true_value();
  }
  return mrb_false_value();
}

static mrb_value
mrb_time_cmp(mrb_state *mrb, mrb_value self)
{
  mrb_value other;
  struct mrb_time *tm1, *tm2;

  mrb_get_args(mrb, "o", &other);
  tm1 = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  tm2 = (struct mrb_time *)mrb_get_datatype(mrb, other, &mrb_time_type);
  if (!tm1 || !tm2) return mrb_nil_value();
  if (tm1->sec > tm2->sec) {
    return mrb_fixnum_value(1);
  }
  else if (tm1->sec < tm2->sec) {
    return mrb_fixnum_value(-1);
  }
  /* tm1->sec == tm2->sec */
  if (tm1->usec > tm2->usec) {
    return mrb_fixnum_value(1);
  }
  else if (tm1->usec < tm2->usec) {
    return mrb_fixnum_value(-1);
  }
  return mrb_fixnum_value(0);
}

static mrb_value
mrb_time_plus(mrb_state *mrb, mrb_value self)
{
  mrb_float f;
  struct mrb_time *tm;

  mrb_get_args(mrb, "f", &f);
  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_time_make(mrb, mrb_obj_class(mrb, self), (double)tm->sec+f, tm->usec, tm->timezone);
}

static mrb_value
mrb_time_minus(mrb_state *mrb, mrb_value self)
{
  mrb_float f;
  mrb_value other;
  struct mrb_time *tm, *tm2;

  mrb_get_args(mrb, "o", &other);
  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();

  tm2 = (struct mrb_time *)mrb_get_datatype(mrb, other, &mrb_time_type);
  if (tm2) {
    f = (mrb_float)(tm->sec - tm2->sec)
      + (mrb_float)(tm->usec - tm2->usec) / 1.0e6;
    return mrb_float_value(f);
  }
  else {
    mrb_get_args(mrb, "f", &f);
    return mrb_time_make(mrb, mrb_obj_class(mrb, self), (double)tm->sec-f, tm->usec, tm->timezone);
  }
}

/* 15.2.19.7.30 */
/* Returns week day number of time. */
static mrb_value
mrb_time_wday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_wday);
}

/* 15.2.19.7.31 */
/* Returns year day number of time. */
static mrb_value
mrb_time_yday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_check_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_yday + 1);
}

/* 15.2.19.7.32 */
/* Returns year of time. */
static mrb_value
mrb_time_year(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_year + 1900);
}

/* 15.2.19.7.33 */
/* Returns name of time's timezone. */
static mrb_value
mrb_time_zone(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  if (tm->timezone <= MRB_TIMEZONE_NONE) return mrb_nil_value();
  if (tm->timezone >= MRB_TIMEZONE_LAST) return mrb_nil_value();
  return mrb_str_new_cstr(mrb, timezone_names[tm->timezone]);
}

/* 15.2.19.7.4 */
/* Returns a string that describes the time. */
static mrb_value
mrb_time_asctime(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;
  struct tm *d;
  char buf[256];
  int len;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  d = &tm->datetime;
  len = snprintf(buf, sizeof(buf), "%s %s %02d %02d:%02d:%02d %s%d",
		 wday_names[d->tm_wday], mon_names[d->tm_mon], d->tm_mday,
		 d->tm_hour, d->tm_min, d->tm_sec,
		 tm->timezone == MRB_TIMEZONE_UTC ? "UTC " : "",
		 d->tm_year + 1900);
  return mrb_str_new(mrb, buf, len);
}

/* 15.2.19.7.6 */
/* Returns the day in the month of the time. */
static mrb_value
mrb_time_day(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_mday);
}


/* 15.2.19.7.7 */
/* Returns true if daylight saving was applied for this time. */
static mrb_value
mrb_time_dstp(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_bool_value(tm->datetime.tm_isdst);
}

/* 15.2.19.7.8 */
/* 15.2.19.7.10 */
/* Returns the Time object of the UTC(GMT) timezone. */
static mrb_value
mrb_time_getutc(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm, *tm2;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return self;
  tm2 = (struct mrb_time *)mrb_malloc(mrb, sizeof(*tm));
  *tm2 = *tm;
  tm2->timezone = MRB_TIMEZONE_UTC;
  mrb_time_update_datetime(tm2);
  return mrb_time_wrap(mrb, mrb_obj_class(mrb, self), tm2);
}

/* 15.2.19.7.9 */
/* Returns the Time object of the LOCAL timezone. */
static mrb_value
mrb_time_getlocal(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm, *tm2;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return self;
  tm2 = (struct mrb_time *)mrb_malloc(mrb, sizeof(*tm));
  *tm2 = *tm;
  tm2->timezone = MRB_TIMEZONE_LOCAL;
  mrb_time_update_datetime(tm2);
  return mrb_time_wrap(mrb, mrb_obj_class(mrb, self), tm2);
}

/* 15.2.19.7.15 */
/* Returns hour of time. */
static mrb_value
mrb_time_hour(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_hour);
}

/* 15.2.19.7.16 */
/* Initializes a time by setting the amount of milliseconds since the epoch.*/
static mrb_value
mrb_time_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_int ayear = 0, amonth = 1, aday = 1, ahour = 0,
  amin = 0, asec = 0, ausec = 0;
  int n;
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (tm) {
    mrb_time_free(mrb, tm);
  }

  n = mrb_get_args(mrb, "|iiiiiii",
		   &ayear, &amonth, &aday, &ahour, &amin, &asec, &ausec);
  if (n == 0) {
    tm = current_mrb_time(mrb);
  }
  else {
    tm = time_mktime(mrb, ayear, amonth, aday, ahour, amin, asec, ausec, MRB_TIMEZONE_LOCAL);
  }
  DATA_PTR(self) = tm;
  DATA_TYPE(self) = &mrb_time_type;
  return self;
}

/* 15.2.19.7.17(x) */
/* Initializes a copy of this time object. */
static mrb_value
mrb_time_initialize_copy(mrb_state *mrb, mrb_value copy)
{
  mrb_value src;

  mrb_get_args(mrb, "o", &src);
  if (mrb_obj_equal(mrb, copy, src)) return copy;
  if (!mrb_obj_is_instance_of(mrb, src, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }
  if (!DATA_PTR(copy)) {
    DATA_PTR(copy) = mrb_malloc(mrb, sizeof(struct mrb_time));
    DATA_TYPE(copy) = &mrb_time_type;
  }
  *(struct mrb_time *)DATA_PTR(copy) = *(struct mrb_time *)DATA_PTR(src);
  return copy;
}

/* 15.2.19.7.18 */
/* Sets the timezone attribute of the Time object to LOCAL. */
static mrb_value
mrb_time_localtime(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return self;
  tm->timezone = MRB_TIMEZONE_LOCAL;
  mrb_time_update_datetime(tm);
  return self;
}

/* 15.2.19.7.19 */
/* Returns day of month of time. */
static mrb_value
mrb_time_mday(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_mday);
}

/* 15.2.19.7.20 */
/* Returns minutes of time. */
static mrb_value
mrb_time_min(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_min);
}

/* 15.2.19.7.21 and 15.2.19.7.22 */
/* Returns month of time. */
static mrb_value
mrb_time_mon(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_mon + 1);
}

/* 15.2.19.7.23 */
/* Returns seconds in minute of time. */
static mrb_value
mrb_time_sec(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->datetime.tm_sec);
}


/* 15.2.19.7.24 */
/* Returns a Float with the time since the epoch in seconds. */
static mrb_value
mrb_time_to_f(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_float_value((mrb_float)tm->sec + (mrb_float)tm->usec/1.0e6);
}

/* 15.2.19.7.25 */
/* Returns a Fixnum with the time since the epoch in seconds. */
static mrb_value
mrb_time_to_i(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->sec);
}

/* 15.2.19.7.26 */
/* Returns a Float with the time since the epoch in microseconds. */
static mrb_value
mrb_time_usec(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_fixnum_value(tm->usec);
}

/* 15.2.19.7.27 */
/* Sets the timzeone attribute of the Time object to UTC. */
static mrb_value
mrb_time_utc(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;

  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (tm) {
    tm->timezone = MRB_TIMEZONE_UTC;
    mrb_time_update_datetime(tm);
  }
  return self;
}

/* 15.2.19.7.28 */
/* Returns true if this time is in the UTC timze zone false if not. */
static mrb_value
mrb_time_utcp(mrb_state *mrb, mrb_value self)
{
  struct mrb_time *tm;
  tm = (struct mrb_time *)mrb_get_datatype(mrb, self, &mrb_time_type);
  if (!tm) return mrb_nil_value();
  return mrb_bool_value(tm->timezone == MRB_TIMEZONE_UTC);
}



void
mrb_init_time(mrb_state *mrb)
{
  struct RClass *tc;
  /* ISO 15.2.19.2 */
  tc = mrb_define_class(mrb, "Time", mrb->object_class);
  MRB_SET_INSTANCE_TT(tc, MRB_TT_DATA);
  mrb_include_module(mrb, tc, mrb_class_get(mrb, "Comparable"));
  mrb_define_class_method(mrb, tc, "at", mrb_time_at, ARGS_ANY());                    /* 15.2.19.6.1 */
  mrb_define_class_method(mrb, tc, "gm", mrb_time_gm, ARGS_REQ(1)|ARGS_OPT(6));       /* 15.2.19.6.2 */
  mrb_define_class_method(mrb, tc, "local", mrb_time_local, ARGS_REQ(1)|ARGS_OPT(6)); /* 15.2.19.6.3 */
  mrb_define_class_method(mrb, tc, "mktime", mrb_time_local, ARGS_REQ(1)|ARGS_OPT(6));/* 15.2.19.6.4 */
  mrb_define_class_method(mrb, tc, "now", mrb_time_now, ARGS_NONE());                 /* 15.2.19.6.5 */
  mrb_define_class_method(mrb, tc, "utc", mrb_time_gm, ARGS_REQ(1)|ARGS_OPT(6));      /* 15.2.19.6.6 */

  mrb_define_method(mrb, tc, "=="     , mrb_time_eq     , ARGS_REQ(1));
  mrb_define_method(mrb, tc, "<=>"    , mrb_time_cmp    , ARGS_REQ(1)); /* 15.2.19.7.1 */
  mrb_define_method(mrb, tc, "+"      , mrb_time_plus   , ARGS_REQ(1)); /* 15.2.19.7.2 */
  mrb_define_method(mrb, tc, "-"      , mrb_time_minus  , ARGS_REQ(1)); /* 15.2.19.7.3 */
  mrb_define_method(mrb, tc, "to_s"   , mrb_time_asctime, ARGS_NONE());
  mrb_define_method(mrb, tc, "inspect", mrb_time_asctime, ARGS_NONE());
  mrb_define_method(mrb, tc, "asctime", mrb_time_asctime, ARGS_NONE()); /* 15.2.19.7.4 */
  mrb_define_method(mrb, tc, "ctime"  , mrb_time_asctime, ARGS_NONE()); /* 15.2.19.7.5 */
  mrb_define_method(mrb, tc, "day"    , mrb_time_day    , ARGS_NONE()); /* 15.2.19.7.6 */
  mrb_define_method(mrb, tc, "dst?"   , mrb_time_dstp   , ARGS_NONE()); /* 15.2.19.7.7 */
  mrb_define_method(mrb, tc, "getgm"  , mrb_time_getutc , ARGS_NONE()); /* 15.2.19.7.8 */
  mrb_define_method(mrb, tc, "getlocal",mrb_time_getlocal,ARGS_NONE()); /* 15.2.19.7.9 */
  mrb_define_method(mrb, tc, "getutc" , mrb_time_getutc , ARGS_NONE()); /* 15.2.19.7.10 */
  mrb_define_method(mrb, tc, "gmt?"   , mrb_time_utcp   , ARGS_NONE()); /* 15.2.19.7.11 */
  mrb_define_method(mrb, tc, "gmtime" , mrb_time_utc    , ARGS_NONE()); /* 15.2.19.7.13 */
  mrb_define_method(mrb, tc, "hour"   , mrb_time_hour, ARGS_NONE());    /* 15.2.19.7.15 */
  mrb_define_method(mrb, tc, "localtime", mrb_time_localtime, ARGS_NONE()); /* 15.2.19.7.18 */
  mrb_define_method(mrb, tc, "mday"   , mrb_time_mday, ARGS_NONE());    /* 15.2.19.7.19 */
  mrb_define_method(mrb, tc, "min"    , mrb_time_min, ARGS_NONE());     /* 15.2.19.7.20 */

  mrb_define_method(mrb, tc, "mon"  , mrb_time_mon, ARGS_NONE());       /* 15.2.19.7.21 */
  mrb_define_method(mrb, tc, "month", mrb_time_mon, ARGS_NONE());       /* 15.2.19.7.22 */

  mrb_define_method(mrb, tc, "sec" , mrb_time_sec, ARGS_NONE());        /* 15.2.19.7.23 */
  mrb_define_method(mrb, tc, "to_i", mrb_time_to_i, ARGS_NONE());       /* 15.2.19.7.25 */
  mrb_define_method(mrb, tc, "to_f", mrb_time_to_f, ARGS_NONE());       /* 15.2.19.7.24 */
  mrb_define_method(mrb, tc, "usec", mrb_time_usec, ARGS_NONE());       /* 15.2.19.7.26 */
  mrb_define_method(mrb, tc, "utc" , mrb_time_utc, ARGS_NONE());        /* 15.2.19.7.27 */
  mrb_define_method(mrb, tc, "utc?", mrb_time_utcp, ARGS_NONE());       /* 15.2.19.7.28 */
  mrb_define_method(mrb, tc, "wday", mrb_time_wday, ARGS_NONE());       /* 15.2.19.7.30 */
  mrb_define_method(mrb, tc, "yday", mrb_time_yday, ARGS_NONE());       /* 15.2.19.7.31 */
  mrb_define_method(mrb, tc, "year", mrb_time_year, ARGS_NONE());       /* 15.2.19.7.32 */
  mrb_define_method(mrb, tc, "zone", mrb_time_zone, ARGS_NONE());       /* 15.2.19.7.33 */

  mrb_define_method(mrb, tc, "initialize", mrb_time_initialize, ARGS_REQ(1)); /* 15.2.19.7.16 */
  mrb_define_method(mrb, tc, "initialize_copy", mrb_time_initialize_copy, ARGS_REQ(1)); /* 15.2.19.7.17 */

  /*
    methods not available:
      gmt_offset(15.2.19.7.12)
      gmtoff(15.2.19.7.14)
      utc_offset(15.2.19.7.29)
  */
}
#endif
