GEM := c_extension_example

include $(MAKEFILE_4_GEM)

GEM_C_FILES := $(wildcard $(SRC_DIR)/*.c)
GEM_OBJECTS := $(patsubst %.c, %.o, $(GEM_C_FILES))

MRUBY_CFLAGS := 
MRUBY_LDFLAGS := 
MRUBY_LIBS := 

gem-all : $(GEM_OBJECTS) gem-c-files

gem-clean : gem-clean-c-files

gem-test : gem-test-c-files
