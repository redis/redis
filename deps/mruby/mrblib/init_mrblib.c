#include "mruby.h"
#include "mruby/irep.h"
#include "mruby/dump.h"
#include "mruby/string.h"
#include "mruby/proc.h"

extern const char mrblib_irep[];

void
mrb_init_mrblib(mrb_state *mrb)
{
  mrb_load_irep(mrb, mrblib_irep);
}

