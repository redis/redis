#!/usr/bin/env ruby

gemname = ARGV.shift.gsub('-', '_')

puts <<__EOF__
/*
 * This file is loading the irep
 * Ruby GEM code.
 *
 * IMPORTANT:
 *   This file was generated!
 *   All manual changes will get lost.
 */
#include "mruby.h"

void mrb_#{gemname}_gem_init(mrb_state*);

void
GENERATED_TMP_mrb_#{gemname}_gem_init(mrb_state *mrb)
{
  mrb_#{gemname}_gem_init(mrb);
}
__EOF__