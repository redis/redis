#!/usr/bin/env ruby

gemname = ARGV.shift.gsub('-', '_')

puts <<__EOF__
void
GENERATED_TMP_mrb_#{gemname}_gem_init(mrb_state *mrb)
{
  mrb_load_irep(mrb, gem_mrblib_irep_#{gemname});
  if (mrb->exc) {
    mrb_p(mrb, mrb_obj_value(mrb->exc));
    exit(0);
  }
}
__EOF__
