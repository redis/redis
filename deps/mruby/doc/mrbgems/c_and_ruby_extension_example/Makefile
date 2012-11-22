GEM := c_and_ruby_extension_example

include $(MAKEFILE_4_GEM)

GEM_C_FILES := $(wildcard $(SRC_DIR)/*.c)
GEM_OBJECTS := $(patsubst %.c, %.o, $(GEM_C_FILES))

GEM_RB_FILES := $(wildcard $(MRB_DIR)/*.rb)

MRUBY_CFLAGS := 
MRUBY_LDFLAGS := 
MRUBY_LIBS := 

gem-all : $(GEM_OBJECTS) gem-c-and-rb-files

gem-clean : gem-clean-c-and-rb-files

gem-test : gem-test-c-and-rb-files
