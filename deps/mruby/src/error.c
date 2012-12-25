/*
** error.c - Exception class
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include "error.h"
#include "mruby/variable.h"
#include "mruby/string.h"
#include "mruby/class.h"
#include "mruby/proc.h"
#include "mruby/irep.h"

#define warn_printf printf

mrb_value
mrb_exc_new(mrb_state *mrb, struct RClass *c, const char *ptr, long len)
{
  return mrb_funcall(mrb, mrb_obj_value(c), "new", 1, mrb_str_new(mrb, ptr, len));
}

mrb_value
mrb_exc_new3(mrb_state *mrb, struct RClass* c, mrb_value str)
{
  //StringValue(str);
  mrb_string_value(mrb, &str);
  return mrb_funcall(mrb, mrb_obj_value(c), "new", 1, str);
}

/*
 * call-seq:
 *    Exception.new(msg = nil)   ->  exception
 *
 *  Construct a new Exception object, optionally passing in
 *  a message.
 */

static mrb_value
exc_initialize(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg;

  if (mrb_get_args(mrb, "|o", &mesg) == 1) {
    mrb_iv_set(mrb, exc, mrb_intern(mrb, "mesg"), mesg);
  }
  return exc;
}

/*
 *  Document-method: exception
 *
 *  call-seq:
 *     exc.exception(string)  ->  an_exception or exc
 *
 *  With no argument, or if the argument is the same as the receiver,
 *  return the receiver. Otherwise, create a new
 *  exception object of the same class as the receiver, but with a
 *  message equal to <code>string.to_str</code>.
 *
 */

static mrb_value
exc_exception(mrb_state *mrb, mrb_value self)
{
  mrb_value exc;
  mrb_value a;
  int argc;

  argc = mrb_get_args(mrb, "|o", &a);
  if (argc == 0) return self;
  if (mrb_obj_equal(mrb, self, a)) return self;
  exc = mrb_obj_clone(mrb, self);
  mrb_iv_set(mrb, exc, mrb_intern(mrb, "mesg"), a);

  return exc;
}

/*
 * call-seq:
 *   exception.to_s   ->  string
 *
 * Returns exception's message (or the name of the exception if
 * no message is set).
 */

static mrb_value
exc_to_s(mrb_state *mrb, mrb_value exc)
{
  mrb_value mesg = mrb_attr_get(mrb, exc, mrb_intern(mrb, "mesg"));

  if (mrb_nil_p(mesg)) return mrb_str_new2(mrb, mrb_obj_classname(mrb, exc));
  return mesg;
}

/*
 * call-seq:
 *   exception.message   ->  string
 *
 * Returns the result of invoking <code>exception.to_s</code>.
 * Normally this returns the exception's message or name. By
 * supplying a to_str method, exceptions are agreeing to
 * be used where Strings are expected.
 */

static mrb_value
exc_message(mrb_state *mrb, mrb_value exc)
{
  return mrb_funcall(mrb, exc, "to_s", 0);
}

/*
 * call-seq:
 *   exception.inspect   -> string
 *
 * Return this exception's class name an message
 */

static mrb_value
exc_inspect(mrb_state *mrb, mrb_value exc)
{
  mrb_value str, mesg, file, line;

  mesg = mrb_attr_get(mrb, exc, mrb_intern(mrb, "mesg"));
  file = mrb_attr_get(mrb, exc, mrb_intern(mrb, "file"));
  line = mrb_attr_get(mrb, exc, mrb_intern(mrb, "line"));
  
  if (!mrb_nil_p(file) && !mrb_nil_p(line)) {
    str = file;
    mrb_str_cat2(mrb, str, ":");
    mrb_str_append(mrb, str, line);
    mrb_str_cat2(mrb, str, ": ");
    if (!mrb_nil_p(mesg) && RSTRING_LEN(mesg) > 0) {
      mrb_str_append(mrb, str, mesg);
      mrb_str_cat2(mrb, str, " (");
    }
    mrb_str_cat2(mrb, str, mrb_obj_classname(mrb, exc));
    if (!mrb_nil_p(mesg) && RSTRING_LEN(mesg) > 0) {
      mrb_str_cat2(mrb, str, ")");
    }
  }
  else {
    str = mrb_str_new2(mrb, mrb_obj_classname(mrb, exc));
    if (!mrb_nil_p(mesg) && RSTRING_LEN(mesg) > 0) {
      mrb_str_cat2(mrb, str, ": ");
      mrb_str_append(mrb, str, mesg);
    } else {
      mrb_str_cat2(mrb, str, ": ");
      mrb_str_cat2(mrb, str, mrb_obj_classname(mrb, exc));
    }
  }
  return str;
}


static mrb_value
exc_equal(mrb_state *mrb, mrb_value exc)
{
  mrb_value obj;
  mrb_value mesg;
  mrb_sym id_mesg = mrb_intern(mrb, "mesg");

  mrb_get_args(mrb, "o", &obj);
  if (mrb_obj_equal(mrb, exc, obj)) return mrb_true_value();

  if (mrb_obj_class(mrb, exc) != mrb_obj_class(mrb, obj)) {
    if (mrb_respond_to(mrb, obj, mrb_intern(mrb, "message"))) {
      mesg = mrb_funcall(mrb, obj, "message", 0);
    }
    else
      return mrb_false_value();
  }
  else {
    mesg = mrb_attr_get(mrb, obj, id_mesg);
  }

  if (!mrb_equal(mrb, mrb_attr_get(mrb, exc, id_mesg), mesg))
    return mrb_false_value();
  return mrb_true_value();
}

static void
exc_debug_info(mrb_state *mrb, struct RObject *exc)
{
  mrb_callinfo *ci = mrb->ci;
  mrb_code *pc = ci->pc;

  mrb_obj_iv_set(mrb, exc, mrb_intern(mrb, "ciidx"), mrb_fixnum_value(ci - mrb->cibase));
  ci--;
  while (ci >= mrb->cibase) {
    if (ci->proc && !MRB_PROC_CFUNC_P(ci->proc)) {
      mrb_irep *irep = ci->proc->body.irep;      

      if (irep->filename && irep->lines && irep->iseq <= pc && pc < irep->iseq + irep->ilen) {
	mrb_obj_iv_set(mrb, exc, mrb_intern(mrb, "file"), mrb_str_new_cstr(mrb, irep->filename));
	mrb_obj_iv_set(mrb, exc, mrb_intern(mrb, "line"), mrb_fixnum_value(irep->lines[pc - irep->iseq - 1]));
	return;
      }
    }
    pc = ci->pc;
    ci--;
  }
}

void
mrb_exc_raise(mrb_state *mrb, mrb_value exc)
{
  mrb->exc = (struct RObject*)mrb_object(exc);
  exc_debug_info(mrb, mrb->exc);
  if (!mrb->jmp) {
    abort();
  }
  longjmp(*(jmp_buf*)mrb->jmp, 1);
}

void
mrb_raise(mrb_state *mrb, struct RClass *c, const char *msg)
{
  mrb_value mesg;
  mesg = mrb_str_new2(mrb, msg);
  mrb_exc_raise(mrb, mrb_exc_new3(mrb, c, mesg));
}

void
mrb_raisef(mrb_state *mrb, struct RClass *c, const char *fmt, ...)
{
  va_list args;
  char buf[256];
  int n;

  va_start(args, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) {
    n = 0;
  }
  mrb_exc_raise(mrb, mrb_exc_new(mrb, c, buf, n));
}

void
mrb_name_error(mrb_state *mrb, mrb_sym id, const char *fmt, ...)
{
  mrb_value exc, argv[2];
  va_list args;
  char buf[256];
  int n;

  va_start(args, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) {
    n = 0;
  }
  argv[0] = mrb_str_new(mrb, buf, n);
  argv[1] = mrb_symbol_value(id); /* ignore now */
  exc = mrb_class_new_instance(mrb, 1, argv, E_NAME_ERROR);
  mrb_exc_raise(mrb, exc);
}

mrb_value
mrb_sprintf(mrb_state *mrb, const char *fmt, ...)
{
  va_list args;
  char buf[256];
  int n;

  va_start(args, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) {
    n = 0;
  }
  return mrb_str_new(mrb, buf, n);
}

void
mrb_warn(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  printf("warning: ");
  vprintf(fmt, args);
  va_end(args);
}

void
mrb_bug(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  printf("bug: ");
  vprintf(fmt, args);
  va_end(args);
  exit(EXIT_FAILURE);
}

static const char *
mrb_strerrno(int err)
{
#define defined_error(name, num) if (err == num) return name;
#define undefined_error(name)
//#include "known_errors.inc"
#undef defined_error
#undef undefined_error
    return NULL;
}

void
mrb_bug_errno(const char *mesg, int errno_arg)
{
  if (errno_arg == 0)
    mrb_bug("%s: errno == 0 (NOERROR)", mesg);
  else {
    const char *errno_str = mrb_strerrno(errno_arg);
    if (errno_str)
      mrb_bug("%s: %s (%s)", mesg, strerror(errno_arg), errno_str);
    else
      mrb_bug("%s: %s (%d)", mesg, strerror(errno_arg), errno_arg);
  }
}

int
sysexit_status(mrb_state *mrb, mrb_value err)
{
  mrb_value st = mrb_iv_get(mrb, err, mrb_intern(mrb, "status"));
  return mrb_fixnum(st);
}

static void
set_backtrace(mrb_state *mrb, mrb_value info, mrb_value bt)
{
  mrb_funcall(mrb, info, "set_backtrace", 1, bt);
}

mrb_value
make_exception(mrb_state *mrb, int argc, mrb_value *argv, int isstr)
{
  mrb_value mesg;
  int n;

  mesg = mrb_nil_value();
  switch (argc) {
    case 0:
    break;
    case 1:
      if (mrb_nil_p(argv[0]))
        break;
      if (isstr) {
        mesg = mrb_check_string_type(mrb, argv[0]);
        if (!mrb_nil_p(mesg)) {
          mesg = mrb_exc_new3(mrb, E_RUNTIME_ERROR, mesg);
          break;
        }
      }
      n = 0;
      goto exception_call;

    case 2:
    case 3:
      n = 1;
exception_call:
      {
	mrb_sym exc = mrb_intern(mrb, "exception");
	if (mrb_respond_to(mrb, argv[0], exc)) {
	  mesg = mrb_funcall_argv(mrb, argv[0], exc, n, argv+1);
	}
	else {
	  /* undef */
	  mrb_raise(mrb, E_TYPE_ERROR, "exception class/object expected");
	}
      }

      break;
    default:
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong number of arguments (%d for 0..3)", argc);
      break;
  }
  if (argc > 0) {
    if (!mrb_obj_is_kind_of(mrb, mesg, mrb->eException_class))
      mrb_raise(mrb, E_TYPE_ERROR, "exception object expected");
    if (argc > 2)
        set_backtrace(mrb, mesg, argv[2]);
  }

  return mesg;
}

mrb_value
mrb_make_exception(mrb_state *mrb, int argc, mrb_value *argv)
{
  return make_exception(mrb, argc, argv, TRUE);
}

void
mrb_sys_fail(mrb_state *mrb, const char *mesg)
{
  mrb_raise(mrb, E_RUNTIME_ERROR, mesg);
}

void
mrb_init_exception(mrb_state *mrb)
{
  struct RClass *e;

  mrb->eException_class = e = mrb_define_class(mrb, "Exception",           mrb->object_class);         /* 15.2.22 */
  mrb_define_class_method(mrb, e, "exception", mrb_instance_new, ARGS_ANY());
  mrb_define_method(mrb, e, "exception", exc_exception, ARGS_ANY());
  mrb_define_method(mrb, e, "initialize", exc_initialize, ARGS_ANY());
  mrb_define_method(mrb, e, "==", exc_equal, ARGS_REQ(1));
  mrb_define_method(mrb, e, "to_s", exc_to_s, ARGS_NONE());
  mrb_define_method(mrb, e, "message", exc_message, ARGS_NONE());
  mrb_define_method(mrb, e, "inspect", exc_inspect, ARGS_NONE());

  mrb->eStandardError_class     = mrb_define_class(mrb, "StandardError",       mrb->eException_class); /* 15.2.23 */
  mrb_define_class(mrb, "RuntimeError", mrb->eStandardError_class);                                    /* 15.2.28 */

  mrb_define_class(mrb, "RuntimeError", mrb->eStandardError_class);                                    /* 15.2.28 */
  e = mrb_define_class(mrb, "ScriptError",  mrb->eException_class);                                    /* 15.2.37 */
  mrb_define_class(mrb, "SyntaxError",  e);                                                            /* 15.2.38 */
}
