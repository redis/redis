# ################################################################
# xxHash Makefile
# Copyright (C) 2012-2021 Yann Collet
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
# You can contact the author at:
#   - xxHash homepage: https://www.xxhash.com
#   - xxHash source repository: https://github.com/Cyan4973/xxHash
# ################################################################

MAKEFLAGS += --no-print-directory
CFLAGS += -Wall -Wextra -Wundef -g

CP = cp
NM = nm
GREP = grep

# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif

ifneq (,$(filter %UTF-8,$(LANG)))
ENABLE_UNICODE ?= 1
else
ENABLE_UNICODE ?= 0
endif

.PHONY: default
default: all

C_SRCDIRS = . .. ../cli
include ../build/make/multiconf.make

.PHONY: all
all: test test_alias

.PHONY: test
test: test_multiInclude test_unicode test_sanity

.PHONY: test_multiInclude
test_multiInclude:
	# compile without xxhash.o, ensure symbols exist within target
	# Note: built using only default rules
	$(MAKE) multiInclude
	$(RM) multiInclude
	# compile with xxhash.o, to detect duplicated symbols
	$(MAKE) multiInclude_withxxhash
	$(RM) multiInclude_withxxhash
	# compile with XXH_NAMESPACE before XXH_INLINE_ALL
	CPPFLAGS=-DXXH_NAMESPACE=TESTN_ $(MAKE) multiInclude
	# no symbol prefixed TESTN_ should exist
	! $(NM) multiInclude | $(GREP) TESTN_
	$(RM) multiInclude
	# compile xxhash.o with XXH_NAMESPACE
	CPPFLAGS=-DXXH_NAMESPACE=TESTN_ $(MAKE) multiInclude_withxxhash
	# symbols prefixed TESTN_ should exist in xxhash.o (though not be invoked)
	$(NM) multiInclude_withxxhash | $(GREP) TESTN_
	$(RM) multiInclude_withxxhash

.PHONY: test_ppc_redefine
test_ppc_redefine: ppc_define.o

XXHSUM_OBJS = xxhash.o $(notdir $(patsubst %.c,%.o,$(wildcard ../cli/*.c)))
xxhsum:
$(eval $(call c_program,xxhsum,$(XXHSUM_OBJS)))

generate_unicode_test:
$(eval $(call c_program,generate_unicode_test,generate_unicode_test.o))

# Make sure that Unicode filenames work.
# https://github.com/Cyan4973/xxHash/issues/293
.PHONY: test_unicode
ifeq (0,$(ENABLE_UNICODE))
test_unicode:
	@echo "Skipping Unicode test, your terminal doesn't appear to support UTF-8."
	@echo "Try with ENABLE_UNICODE=1"
else
test_unicode: generate_unicode_test xxhsum
	# Generate a Unicode filename test dynamically
	# to keep UTF-8 out of the source tree.
	./generate_unicode_test$(EXT)
	$(SHELL) ./unicode_test.sh
endif

.PHONY: test_filename_escape
test_filename_escape: xxhsum
	./filename-escape.sh

.PHONY: test_cli_comment_line
test_cli_comment_line: xxhsum
	$(SHELL) ./cli-comment-line.sh

.PHONY: test_cli_ignore_missing
test_cli_ignore_missing: xxhsum
	$(SHELL) ./cli-ignore-missing.sh

.PHONY: test_sanity
test_sanity: sanity_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) sanity_test.c -o sanity_test$(EXT)
	$(RUN_ENV) ./sanity_test$(EXT)

sanity_test_vectors_generator:
$(eval $(call c_program,sanity_test_vectors_generator,sanity_test_vectors_generator.o))

.PHONY: sanity_test_vectors.h
sanity_test_vectors.h: sanity_test_vectors_generator
	./sanity_test_vectors_generator$(EXT)

multiInclude_withxxhash:
$(eval $(call c_program,multiInclude_withxxhash,multiInclude.o xxhash.o))

test_alias: CPPFLAGS += -I..
test_alias: CFLAGS += -O3 -Werror
$(eval $(call c_program,test_alias,test_alias.o))

.PHONY: clean
clean:
	@$(RM) *.o
	@$(RM) multiInclude
	@$(RM) *.unicode unicode_test.*
	@$(RM) sanity_test$(EXT) sanity_test_vectors_generator$(EXT)
