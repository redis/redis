# Makefile description.
# basic build file for mruby library (Ruby part)

# project-specific macros
# extension of the executable-file is modifiable(.exe .out ...)
BASEDIR = .
TARGET := mrblib
MLIB := $(TARGET).o
CLIB := $(TARGET).c
DLIB := $(TARGET).ctmp
RLIB := $(TARGET).rbtmp
DEPLIB := $(TARGET).d
MRB1 := $(BASEDIR)/*.rb
MRBS := $(MRB1)
LIBR0 := ../lib/libmruby_core.a
LIBR := ../lib/libmruby.a

# libraries, includes
INCLUDES = -I../src -I../include

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

# mruby compiler
ifeq ($(OS),Windows_NT)
MRBC = ../bin/mrbc.exe
else
MRBC = ../bin/mrbc
endif


##############################
# generic build targets, rules

.PHONY : all
all : $(LIBR)

# update libmruby.a
$(LIBR) : $(MLIB) $(LIBR0)
	$(CP) $(LIBR0) $(LIBR)
	$(AR) r $(LIBR) $(MLIB)

# Compile mrblib source
$(MLIB) : $(CLIB)
	$(CC) $(ALL_CFLAGS) -MMD $(INCLUDES) -c $(CLIB) -o $(MLIB)

# Compile C source from merged mruby source
$(CLIB) : $(RLIB) $(MRBC)
	$(MRBC) -Bmrblib_irep -o$(DLIB) $(RLIB); $(CAT) init_$(TARGET).c $(DLIB) > $@

$(MRBC) : $(LIBR0)
	$(MAKE) -C ../tools/mrbc $(MAKE_FLAGS)

# merge mruby sources
$(RLIB) : $(MRBS)
	$(CAT) $(MRBS) > $@

# clean up
.PHONY : clean
clean :
	@echo "make: removing targets, objects and depend files of `pwd`"
	-$(RM_F) $(MRBC) $(MLIB) $(CLIB) $(RLIB) $(DLIB) $(DEPLIB) $(LIBR)
