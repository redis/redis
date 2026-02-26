# ##########################################################################
# multiconf.make
# Copyright (C) Yann Collet
#
# GPL v2 License
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# ##########################################################################


# Provides c_program(_shared_o) and cxx_program(_shared_o) target generation macros
# Provides static_library and c_dynamic_library target generation macros
# Support recompilation of only impacted units when an associated *.h is updated.
# Provides V=1 / VERBOSE=1 support. V=2 is used for debugging purposes.
# Complement target clean: delete objects and binaries created by this script

# Requires:
# - C_SRCDIRS, CXX_SRCDIRS, ASM_SRCDIRS defined
#   OR
#   C_SRCS, CXX_SRCS and ASM_SRCS variables defined
#   *and* vpath set to find all source files
#   OR
#   C_OBJS, CXX_OBJS and ASM_OBJS variables defined
#   *and* vpath set to find all source files
# - directory `cachedObjs/` available to cache object files.
#   alternatively: set CACHE_ROOT to some different value.
# Optional:
# - HASH can be set to a different custom hash program.

# *_program*: generates a recipe for a target that will be built in a cache directory.
# The cache directory is automatically derived from CACHE_ROOT and list of flags and compilers.
# *_shared_o* variants are optional optimization variants, that share the same objects across multiple targets.
# However, as a consequence, all these objects must have exactly the same list of flags,
# which in practice means that there must be no target-level modification (like: target: CFLAGS += someFlag).
# If unsure, only use the standard variants, c_program and cxx_program.

# All *_program* macro functions take up to 4 argument:
# - The name of the target
# - The list of object files to build in the cache directory
# - An optional list of dependencies for linking, that will not be built
# - An optional complementary recipe code, that will run after compilation and link


# Silent mode is default; use V = 1 or VERBOSE = 1 to see compilation lines
VERBOSE ?= $(V)
$(VERBOSE).SILENT:

# Directory where object files will be built
CACHE_ROOT ?= cachedObjs

# --------------------------------------------------------------------------------------------

# Dependency management
DEPFLAGS = -MT $@ -MMD -MP -MF

# Include dependency files
include $(wildcard $(CACHE_ROOT)/**/*.d)
include $(wildcard $(CACHE_ROOT)/generic/*/*.d)

# --------------------------------------------------------------------------------------------

# Automatic determination of build artifacts cache directory, keyed on build
# flags, so that we can do incremental, parallel builds of different binaries
# with different build flags without collisions.

UNAME ?= $(shell uname)
ifeq ($(UNAME), Darwin)
  HASH ?= md5
else ifeq ($(UNAME), FreeBSD)
  HASH ?= gmd5sum
else ifeq ($(UNAME), OpenBSD)
  HASH ?= md5
endif
HASH ?= md5sum

HAVE_HASH := $(shell echo 1 | $(HASH) > /dev/null && echo 1 || echo 0)
ifeq ($(HAVE_HASH),0)
  $(info warning : could not find HASH ($(HASH)), required to differentiate builds using different flags)
  HASH_FUNC = generic/$(1)
else
  HASH_FUNC = $(firstword $(shell echo $(2) | $(HASH) ))
endif


MKDIR ?= mkdir
LN ?= ln

# --------------------------------------------------------------------------------------------
# The following macros are used to create object files in the cache directory.
# The object files are named after the source file, but with a different path.

# Create build directories on-demand.
#
# For some reason, make treats the directory as an intermediate file and tries
# to delete it. So we work around that by marking it "precious". Solution found
# here:
# http://ismail.badawi.io/blog/2017/03/28/automatic-directory-creation-in-make/
.PRECIOUS: $(CACHE_ROOT)/%/.
$(CACHE_ROOT)/%/. :
	$(MKDIR) -p $@


define addTargetAsmObject  # targetName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2))))

.PRECIOUS: $$(CACHE_ROOT)/%/$(1)
$$(CACHE_ROOT)/%/$(1) : $(1:.o=.S) $(2) | $$(CACHE_ROOT)/%/.
	@echo AS $$@
	$$(CC) $$(CPPFLAGS) $$(CXXFLAGS) $$(DEPFLAGS) $$(CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetAsmObject

define addTargetCObject  # targetName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2)))) #debug print

.PRECIOUS: $$(CACHE_ROOT)/%/$(1)
$$(CACHE_ROOT)/%/$(1) : $(1:.o=.c) $(2) | $$(CACHE_ROOT)/%/.
	@echo CC $$@
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(DEPFLAGS) $$(CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetCObject

define addTargetCxxObject  # targetName, suffix, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3))))

.PRECIOUS: $$(CACHE_ROOT)/%/$(1)
$$(CACHE_ROOT)/%/$(1) : $(1:.o=.$(2)) $(3) | $$(CACHE_ROOT)/%/.
	@echo CXX $$@
	$$(CXX) $$(CPPFLAGS) $$(CXXFLAGS) $$(DEPFLAGS) $$(CACHE_ROOT)/$$*/$(1:.o=.d) -c $$< -o $$@

endef # addTargetCxxObject

# Create targets for individual object files
C_SRCDIRS += .
vpath %.c $(C_SRCDIRS)
CXX_SRCDIRS += .
vpath %.cpp $(CXX_SRCDIRS)
vpath %.cc $(CXX_SRCDIRS)
ASM_SRCDIRS += .
vpath %.S $(ASM_SRCDIRS)

# If C_SRCDIRS, CXX_SRCDIRS and ASM_SRCDIRS are not defined, use C_SRCS, CXX_SRCS and ASM_SRCS
C_SRCS   ?= $(notdir $(foreach dir,$(C_SRCDIRS),$(wildcard $(dir)/*.c)))
CPP_SRCS ?= $(notdir $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cpp)))
CC_SRCS  ?= $(notdir $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cc)))
CXX_SRCS ?= $(CPP_SRCS) $(CC_SRCS)
ASM_SRCS ?= $(notdir $(foreach dir,$(ASM_SRCDIRS),$(wildcard $(dir)/*.S)))

# If C_SRCS, CXX_SRCS and ASM_SRCS are not defined, use C_OBJS, CXX_OBJS and ASM_OBJS
C_OBJS   ?= $(patsubst %.c,%.o,$(C_SRCS))
CPP_OBJS ?= $(patsubst %.cpp,%.o,$(CPP_SRCS))
CC_OBJS  ?= $(patsubst %.cc,%.o,$(CC_SRCS))
CXX_OBJS ?= $(CPP_OBJS) $(CC_OBJS) # Note: not used
ASM_OBJS ?= $(patsubst %.S,%.o,$(ASM_SRCS))

$(foreach OBJ,$(C_OBJS),$(eval $(call addTargetCObject,$(OBJ))))
$(foreach OBJ,$(CPP_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cpp)))
$(foreach OBJ,$(CC_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cc)))
$(foreach OBJ,$(ASM_OBJS),$(eval $(call addTargetAsmObject,$(OBJ))))

# --------------------------------------------------------------------------------------------
# The following macros are used to create targets in the user Makefile.
# Binaries are built in the cache directory, and then symlinked to the current directory.
# The cache directory is automatically derived from CACHE_ROOT and list of flags and compilers.


# static_library - Create build rules for a static library with caching
# Parameters:
#   1. libName       - Library name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps     - Additional dependencies (no cache path prefix)
#   4. postBuildCmds - Extra commands to run after AR
#   5. extraHash     - Additional key to compute the unique cache path
# Example:
#   $(call static_library,libmath.a,vector.o matrix.o,$(CONFIG_H),strip $@,$(VERSION))
define static_library  # libName, objectDeps, extraDeps, postBuildCmds, extraHash

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5))))
MCM_ALL_BINS += $(1)

$$(CACHE_ROOT)/%/$(1) : $$(addprefix $$(CACHE_ROOT)/%/,$(2)) $(3)
	@echo AR $$@
	$$(AR) $$(ARFLAGS) $$@ $$^
	$(4)

.PHONY: $(1)
$(1) : ARFLAGS = rcs
$(1) : $$(CACHE_ROOT)/$$(call HASH_FUNC,$(1),$(2) $$(CPPFLAGS) $$(CC) $$(CFLAGS) $$(CXX) $$(CXXFLAGS) $$(AR) $$(ARFLAGS) $(5))/$(1)
	$$(LN) -sf $$< $$@

endef # static_library


# c_dynamic_library - Create build rules for a C dynamic/shared library with caching
# Parameters:
#   1. libName      - Library name (becomes output file and phony target)
#   2. objectDeps   - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps    - Additional dependencies (no cache path prefix)
#   4. postLinkCmds - Extra commands to run after linking
#   5. extraHash    - Additional key to compute the unique cache path
# Example:
#   $(call c_dynamic_library,libmath.so,vector.o matrix.o,$(CONFIG_H),strip $@,$(VERSION))
define c_dynamic_library  # libName, objectDeps, extraDeps, postLinkCmds, extraHash

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5))))
MCM_ALL_BINS += $(1)

$$(CACHE_ROOT)/%/$(1) : $$(addprefix $$(CACHE_ROOT)/%/,$(2)) $(3)
	@echo LD $$@
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(LDFLAGS) -shared -o $$@ $$^ $$(LDLIBS)
	$(4)

.PHONY: $(1)
$(1) : CFLAGS += -fPIC
$(1) : $$(CACHE_ROOT)/$$(call HASH_FUNC,$(1),$(2) $$(CPPFLAGS) $$(CC) $$(CFLAGS) $$(LDFLAGS) $$(LDLIBS) $(5))/$(1)
	$$(LN) -sf $$< $$@

endef # c_dynamic_library


# program_base - Create build rules for an executable program with caching
# Parameters:
#   1. progName      - Executable name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be prefixed with cache path)
# Parameters 3 to 5 are optional:
#   3. extraDeps     - Additional dependencies (without cache path prefix)
#   4. postLinkCmds  - Extra commands to run after linking
#   5. extraHash     - Additional data to include in cache path hash
# Parameters 6 & 7 are compulsory:
#   6. compiler      - Variable name of compiler to use (CC or CXX)
#   7. compilerFlags - Variable name of compiler flags to use (CFLAGS or CXXFLAGS)
# Example:
#   $(call program_base,myapp,main.o utils.o,$(CONFIG_H),strip $@,$(VERSION),CC,CFLAGS)
#   $(call program_base,mycppapp,main.o utils.o,$(CONFIG_H),strip $@,$(VERSION),CXX,CXXFLAGS)
define program_base  # progName, objectDeps, extraDeps, postLinkCmds, extraHash, compiler, compilerFlags

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5),$(6),$(7))))
MCM_ALL_BINS += $(1)

$$(CACHE_ROOT)/%/$(1) : $$(addprefix $$(CACHE_ROOT)/%/,$(2)) $(3)
	@echo LD $$@
	$$($(6)) $$(CPPFLAGS) $$($(7)) $$^ -o $$@ $$(LDFLAGS) $$(LDLIBS)
	$(4)

.PHONY: $(1)
$(1) : $$(CACHE_ROOT)/$$(call HASH_FUNC,$(1),$$($(6)) $$(CPPFLAGS) $$($(7)) $$(LDFLAGS) $$(LDLIBS) $(5))/$(1)
	$$(LN) -sf $$< $$@$(EXT)

endef # program_base
# Note: $(EXT) must be set to .exe for Windows

define c_program  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),$(1)$(2),CC,CFLAGS))
endef # c_program

define c_program_shared_o  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CC,CFLAGS))
endef # c_program_shared_o

define cxx_program  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),$(1)$(2),CXX,CXXFLAGS))
endef # cxx_program

define cxx_program_shared_o  # progName, objectDeps, extraDeps, postLinkCmds
$$(eval $$(call program_base,$(1),$(2),$(3),$(4),,CXX,CXXFLAGS))
endef # cxx_program_shared_o

# --------------------------------------------------------------------------------------------

# Cleaning: delete all objects and binaries created by this script
.PHONY: clean_cache
clean_cache:
	$(RM) -rf $(CACHE_ROOT)
	$(RM) $(MCM_ALL_BINS)

# automatically attach to standard clean target
.PHONY: clean
clean: clean_cache
