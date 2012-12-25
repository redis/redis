# Makefile description.
# basic build file for mruby-compiler

# project-specific macros
# extension of the executable-file is modifiable(.exe .out ...)
BASEDIR := ../../src
TARGET := ../../bin/mrbc
LIBR := ../../lib/libmruby_core.a
ifeq ($(OS),Windows_NT)
EXE := $(TARGET).exe
else
EXE := $(TARGET)
endif
EXCEPT1 := 
OBJ0 := $(patsubst %.c,%.o,$(wildcard $(BASEDIR)/../tools/mrbc/*.c))
#OBJ1 := $(patsubst %.c,%.o,$(filter-out $(EXCEPT1),$(wildcard $(BASEDIR)/*.c)))
#OBJ2 := $(patsubst %.c,%.o,$(wildcard $(BASEDIR)/ext/regex/*.c))
#OBJ3 := $(patsubst %.c,%.o,$(wildcard $(BASEDIR)/ext/enc/*.c))
OBJS := $(OBJ0)

# libraries, includes
LIBS = -lm
INCLUDES = -I$(BASEDIR) -I$(BASEDIR)/../include

# compiler, linker (gcc)
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
  MAKE_FLAGS = CC=$(CC) LL=$(LL) ALL_CFLAGS="$(ALL_CFLAGS)"
else
  MAKE_FLAGS = CC='$(CC)' LL='$(LL)' ALL_CFLAGS='$(ALL_CFLAGS)'
endif


##############################
# generic build targets, rules

.PHONY : all
all : $(EXE)

# executable constructed using linker from object files
$(EXE) : $(OBJS) $(LIBR)
	$(LL) -o $@ $(LDFLAGS) $(OBJS) $(LIBR) $(LIBS)

-include $(OBJS:.o=.d)

# objects compiled from source
$(OBJS) : %.o : %.c
	$(CC) $(ALL_CFLAGS) -MMD $(INCLUDES) -c $< -o $@

# C library compile
$(LIBR) :
	@$(MAKE) -C $(BASEDIR) $(MAKE_FLAGS)

# clean up
.PHONY : clean
clean :
	@echo "make: removing targets, objects and depend files of `pwd`"
	-$(RM_F) $(EXE) $(OBJS)
	-$(RM_F) $(OBJS:.o=.d)
