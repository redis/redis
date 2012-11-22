# Makefile description.
# basic build file for the mruby testing environment mrbtest

# project-specific macros
# extension of the executable-file is modifiable(.exe .out ...)
MRUBY_ROOT := ..
BASEDIR = .
TARGET := mrbtest
LIBR := ../lib/libmruby.a

ifeq ($(strip $(ENABLE_GEMS)),)
  # by default GEMs are deactivated
  ENABLE_GEMS = false
endif

ifeq ($(ENABLE_GEMS),false)
  GEM_ARCHIVE_FILES =
else
  GEMDIR := ../mrbgems
  GEMDLIB := $(GEMDIR)/g/mrbgemtest.ctmp
  MAKEFILE_GEM_LIST := $(MRUBY_ROOT)/mrbgems/g/MakefileGemList
  ifeq ($(wildcard $(MAKEFILE_GEM_LIST)),)
    GEM_ARCHIVE_FILES =
  else
    include $(MAKEFILE_GEM_LIST)
  endif
endif

MLIB := $(TARGET).o
CLIB := $(TARGET).c
INIT := init_$(TARGET).c
DLIB := $(TARGET).ctmp
RLIB := $(TARGET).rbtmp
DEPLIB := $(TARGET).d driver.d
ASSLIB := $(BASEDIR)/assert.rb
MRBS := $(BASEDIR)/t/*.rb
OBJS := driver.o $(MLIB)

# for mruby/mrbc test
REPLIB  := $(BASEDIR)/report.rb
TESTRB  := mrubytest.rb
TESTMRB := mrubytest.mrb
TESTRB_REP  := mrubytest.rb.report
TESTMRB_REP := mrubytest.mrb.report

# libraries, includes
LIBS = -lm
INCLUDES = -I$(BASEDIR)/../src -I$(BASEDIR)/../include

ifeq ($(strip $(COMPILE_MODE)),)
  # default compile option
  COMPILE_MODE = debug
endif

ALL_CFLAGS = -Wall -Werror-implicit-function-declaration $(CFLAGS)
ifeq ($(OS),Windows_NT)
  MAKE_FLAGS = CC=$(CC) LL=$(LL) ALL_CFLAGS='$(ALL_CFLAGS)' LDFLAGS='$(LDFLAGS)' 
else
  MAKE_FLAGS = CC='$(CC)' LL='$(LL)' ALL_CFLAGS='$(ALL_CFLAGS)' LDFLAGS='$(LDFLAGS)' 
endif

# mruby compiler and test driver
ifeq ($(OS),Windows_NT)
MRBC = ../bin/mrbc.exe
MRUBY= ../bin/mruby.exe
EXE := $(TARGET).exe
else
MRBC = ../bin/mrbc
MRUBY= ../bin/mruby
EXE := $(TARGET)
endif

RAKE = ../minirake


##############################
# generic build targets, rules

.PHONY : test
all : $(EXE) $(MRUBY) $(TESTRB) $(TESTMRB) 
	@echo "# exec mrbtest"
	./$(EXE)
	@echo
	@echo "# exec mruby test with ruby script"
	@($(MRUBY) $(TESTRB) > $(TESTRB_REP) && echo "mrubytest.rb success.") || $(CAT) $(TESTRB_REP)
	@echo
	@echo "# exec mruby test with mrb file"
	@($(MRUBY) -b $(TESTMRB) > $(TESTMRB_REP) && echo "mrubytest.mrb success.") || $(CAT) $(TESTMRB_REP)
	@echo

# executable constructed using linker from object files
$(EXE) : generate-mrbgems-test $(OBJS) $(LIBR) $(GEM_ARCHIVE_FILES)
	$(LL) -o $@ $(LDFLAGS) $(OBJS) $(LIBR) $(GEM_ARCHIVE_FILES) $(LIBS)

generate-mrbgems-test :
ifeq ($(ENABLE_GEMS),true)
	@$(RAKE) mrbgems_prepare_test
endif

-include $(OBJS:.o=.d)

# objects compiled from source
$(OBJS) : %.o : %.c
	$(CC) $(ALL_CFLAGS) -MMD $(INCLUDES) -c $< -o $@

# Compile C source from merged mruby source
$(CLIB) : $(DLIB) $(INIT) $(GEMDLIB)
	$(CAT) $(INIT) $(DLIB) $(GEMDLIB) > $@

$(DLIB) : $(RLIB) $(MRBC)
	$(MRBC) -Bmrbtest_irep -o$@ $(RLIB)

# merge mruby sources
$(RLIB) : $(ASSLIB) $(MRBS)
	$(CAT) $(ASSLIB) $(MRBS) > $@

# Compile mrb file from mruby source
$(TESTMRB) : $(MRBC) $(TESTRB)
	$(MRBC) $(TESTRB)

$(TESTRB) : $(ASSLIB) $(MRBS) $(REPLIB)
	$(CAT) $(ASSLIB) $(MRBS) $(REPLIB) > $@

# clean up
.PHONY : clean
clean :
	@echo "make: removing targets, objects and depend files of `pwd`"
	-$(RM_F) $(MLIB) $(CLIB) $(RLIB) $(DLIB) $(DEPLIB) $(OBJS) $(EXE)
	-$(RM_F) $(TESTRB) $(TESTMRB) $(TESTRB_REP) $(TESTMRB_REP)
