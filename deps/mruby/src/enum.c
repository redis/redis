/*
** enum.c - Enumerable module
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"

void
mrb_init_enumerable(mrb_state *mrb)
{
  mrb_define_module(mrb, "Enumerable");
}

