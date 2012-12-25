/*
** error.h - Exception class
**
** See Copyright Notice in mruby.h
*/

#ifndef MRUBY_ERROR_H
#define MRUBY_ERROR_H

struct RException {
  MRB_OBJECT_HEADER;
};

void mrb_sys_fail(mrb_state *mrb, const char *mesg);
void mrb_bug_errno(const char*, int);
int sysexit_status(mrb_state *mrb, mrb_value err);
mrb_value mrb_exc_new3(mrb_state *mrb, struct RClass* c, mrb_value str);
mrb_value make_exception(mrb_state *mrb, int argc, mrb_value *argv, int isstr);
mrb_value mrb_make_exception(mrb_state *mrb, int argc, mrb_value *argv);
mrb_value mrb_sprintf(mrb_state *mrb, const char *fmt, ...);
void mrb_name_error(mrb_state *mrb, mrb_sym id, const char *fmt, ...);
void mrb_exc_print(mrb_state *mrb, struct RObject *exc);

#endif  /* MRUBY_ERROR_H */
