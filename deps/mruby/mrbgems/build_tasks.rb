MRBGEMS_PATH = File.dirname(__FILE__)

GEM_INIT = "#{MRBGEMS_PATH}/gem_init"
GEM_MAKEFILE = "#{MRBGEMS_PATH}/g/Makefile"
GEM_MAKEFILE_LIST = "#{MRBGEMS_PATH}/g/MakefileGemList"
MAKEFILE_4_GEM = "#{MRUBY_ROOT}/mrbgems/Makefile4gem"

def gem_make_flags
  if ENV['OS'] == 'Windows_NT'
    "#{MAKE_FLAGS rescue ''} MAKEFILE_4_GEM=\"#{MAKEFILE_4_GEM}\""
  else
    "#{MAKE_FLAGS rescue ''} MAKEFILE_4_GEM='#{MAKEFILE_4_GEM}'"
  end
end

task :mrbgems_all => ["#{GEM_INIT}.a", :mrbgems_generate_gem_makefile_list] do
  for_each_gem do |path, gemname|
    sh "#{MAKE} -C #{path} #{gem_make_flags}"
  end
end

task :load_mrbgems_flags do
  for_each_gem do |path, gemname|
    sh "#{MAKE} gem-flags -C #{path} #{gem_make_flags}"
    CFLAGS << File.read("#{path}/gem-cflags.tmp").chomp
    LDFLAGS << File.read("#{path}/gem-ldflags.tmp").chomp
    LIBS << File.read("#{path}/gem-libs.tmp").chomp
  end
end

task :mrbgems_clean do
  sh "cd #{MRUBY_ROOT}/mrbgems && #{RM_F} *.c *.d *.a *.o"
  sh "cd #{MRUBY_ROOT}/mrbgems/g && #{RM_F} *.c *.d *.rbtmp *.ctmp *.o mrbtest"
  for_each_gem do |path, gemname|
    sh "#{MAKE} gem-clean -C #{path} #{gem_make_flags}"
  end
end

task :mrbgems_prepare_test do
  for_each_gem do |path, gemname, escaped_gemname|
    sh "#{MAKE} gem-test -C #{path} #{gem_make_flags}"
  end
  open("#{MRUBY_ROOT}/mrbgems/g/mrbgemtest.ctmp", 'w') do |f|
    for_each_gem do |path, gemname, escaped_gemname|
      f.puts "void mrb_#{escaped_gemname}_gem_test_init(mrb_state *mrb);"
      f.puts "extern const char gem_mrblib_irep_#{escaped_gemname}_test[];"
    end
    f.puts "void mrbgemtest_init(mrb_state* mrb) {"
    for_each_gem do |path, gemname, escaped_gemname|
      f.puts "mrb_#{escaped_gemname}_gem_test_init(mrb);"
    end
    for_each_gem do |path, gemname, escaped_gemname|
      f.puts "mrb_load_irep(mrb, gem_mrblib_irep_#{escaped_gemname}_test);"
    end
    f.puts "}"

  end
  sh "#{CAT} #{for_each_gem{|path, gemname| "#{path}/gem_test.ctmp "}} >> #{MRUBY_ROOT}/mrbgems/g/mrbgemtest.ctmp"
end

file "#{GEM_INIT}.a" => ["#{GEM_INIT}.c", "#{GEM_INIT}.o"] do |t|
  sh "#{AR} rs #{t.name} #{GEM_INIT}.o"
end

rule ".o" => [".c"] do |t|
  puts "Build the driver which initializes all gems"
  sh "#{CC} #{CFLAGS.join(' ')} -I#{MRUBY_ROOT}/include -MMD -c #{t.source} -o #{t.name}"
end

file "#{GEM_INIT}.c" do |t|
  puts "Generate Gem driver: #{t.name}"
  open(t.name, 'w') do |f|
    f.puts <<__EOF__
/*
 * This file contains a list of all
 * initializing methods which are
 * necessary to bootstrap all gems.
 *
 * IMPORTANT:
 *   This file was generated!
 *   All manual changes will get lost.
 */

#include "mruby.h"

#{for_each_gem{|path, gemname, escaped_gemname| "void GENERATED_TMP_mrb_%s_gem_init(mrb_state*);" % [escaped_gemname]}}

void
mrb_init_mrbgems(mrb_state *mrb) {
#{for_each_gem{|path, gemname, escaped_gemname| "  GENERATED_TMP_mrb_%s_gem_init(mrb);" % [escaped_gemname]}}
}
__EOF__
  end
end

def for_each_gem(&block)
  IO.readlines(ACTIVE_GEMS).map { |line|
    path = line.chomp
    if not File.exist?(path)
      path2 = File.join MRUBY_ROOT, 'mrbgems', 'g', path
      path = path2 if File.exist? path2
    end
    gemname = File.basename(path)
    escaped_gemname = gemname.gsub(/-/, '_')
    block.call(path, gemname, escaped_gemname)
  }.join('')
end

task :mrbgems_generate_gem_makefile_list do
  open(GEM_MAKEFILE_LIST, 'w') do |f|
    f.puts <<__EOF__
GEM_LIST := #{for_each_gem{|path, gemname| "#{path}/mrb-#{gemname}-gem.a "}}

GEM_ARCHIVE_FILES := #{MRUBY_ROOT}/mrbgems/gem_init.a
GEM_ARCHIVE_FILES += $(GEM_LIST)

GEM_CFLAGS_LIST := #{for_each_gem{|path, gemname| "#{File.read("#{path}/gem-cflags.tmp").chomp} "}}
GEM_LDFLAGS_LIST := #{for_each_gem{|path, gemname| "#{File.read("#{path}/gem-ldflags.tmp").chomp} "}}
__EOF__
  end
end
