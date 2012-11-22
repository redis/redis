GEM := ruby_extension_example

include $(MAKEFILE_4_GEM)

GEM_RB_FILES := $(wildcard $(MRB_DIR)/*.rb)

MRUBY_CFLAGS := 
MRUBY_LDFLAGS := 
MRUBY_LIBS := 

gem-all : gem-rb-files

gem-clean : gem-clean-rb-files

gem-test : gem-test-rb-files
