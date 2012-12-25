# Makefile description.
# basic build file for mirb executable

# project-specific macros
# extension of the executable-file is modifiable(.exe .out ...)
MRUBY_ROOT := ../..
BASEDIR = ../../src
TARGET := ../../bin/mirb
LIBR := ../../lib/libmruby.a

ifeq ($(strip $(ENABLE_GEMS)),)
  # by default GEMs are deactivated
  ENABLE_GEMS = false
endif

ifeq ($(ENABLE_GEMS),false)
  GEM_ARCHIVE_FILES =
else
  MAKEFILE_GEM_LIST := $(MRUBY_ROOT)/mrbgems/g/MakefileGemList
  ifeq ($(wildcard $(MAKEFILE_GEM_LIST)),)
    GEM_ARCHIVE_FILES =
  else
    include $(MAKEFILE_GEM_LIST)
  endif
endif

ifeq ($(OS),Windows_NT)
  EXE := $(TARGET).exe
else
  EXE := $(TARGET)
endif
OBJ0 := $(patsubst %.c,%.o,$(wildcard $(BASEDIR)/../tools/mirb/*.c))
OBJS := $(OBJ0)

# ext libraries
EXTS := $(EXT1)

# libraries, includes
LIBS = -lm
INCLUDES = -I$(BASEDIR) -I$(BASEDIR)/../include

ifeq ($(strip $(COMPILE_MODE)),)
  # default compile option
  COMPILE_MODE = debug
endif

ifeq ($(COMPILE_MODE),debug)
  CFLAGS = -g -O3
else ifeq ($(COMPILE_MODE),release)
  CFLAGS = -O3
else ifeq ($(COMPILE_MODE),small)
  CFLAGS = -Os
endif

ALL_CFLAGS = -Wall -Werror-implicit-function-declaration $(CFLAGS)
ifeq ($(OS),Windows_NT)
  MAKE_FLAGS = CC=$(CC) LL=$(LL) ALL_CFLAGS='$(ALL_CFLAGS)' LDFLAGS='$(LDFLAGS)' 
else
  MAKE_FLAGS = CC='$(CC)' LL='$(LL)' ALL_CFLAGS='$(ALL_CFLAGS)' LDFLAGS='$(LDFLAGS)' 
endif

##############################
# generic build targets, rules

.PHONY : all
all : $(LIBR) $(EXE)

# executable constructed using linker from object files
$(EXE) : $(LIBR) $(OBJS) $(GEM_ARCHIVE_FILES) $(EXTS)
	$(LL) -o $@ $(LDFLAGS) $(OBJS) $(LIBR) $(GEM_ARCHIVE_FILES) $(EXTS) $(LIBS)

-include $(OBJS:.o=.d)

# objects compiled from source
$(OBJS) : %.o : %.c
	$(CC) $(ALL_CFLAGS) -MMD $(INCLUDES) -c $< -o $@

# C library compile
$(LIBR) :
	@$(MAKE) -C $(BASEDIR) $(MAKE_FLAGS)

# mruby library compile
# extend libraries complile
$(EXTS) : %.o : %.c
	$(CC) $(ALL_CFLAGS) -MMD $(INCLUDES) -c $< -o $@

# clean up
.PHONY : clean #cleandep
clean :
	$(MAKE) clean -C ../../mrblib $(MAKE_FLAGS)
	$(MAKE) clean -C ../mrbc $(MAKE_FLAGS)
	@echo "make: removing targets, objects and depend files of `pwd`"
	-$(RM_F) $(EXE) $(OBJS)
	-$(RM_F) $(OBJS:.o=.d)
