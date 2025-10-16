#  Brute force collision tester for 64-bit hashes
#  Part of xxHash project
#  Copyright (C) 2019-2021 Yann Collet
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
#  You can contact the author at:
#  - xxHash homepage: https://www.xxhash.com
#  - xxHash source repository: https://github.com/Cyan4973/xxHash
#

HEADER_DIRS = ./ ../../ allcodecs/
CPPFLAGS += $(addprefix -I ,$(HEADER_DIRS))
CFLAGS   += -Wall -Wextra -Wconversion \
            -std=c11
CXXFLAGS += -Wall -Wextra -Wconversion \
            -std=c++11
LDFLAGS  += -pthread
TESTHASHES = 3200000

HASH_SRC := $(wildcard allcodecs/*.c) $(wildcard allcodecs/*.cc) $(wildcard allcodecs/*.cpp)
HASH_OBJ := $(addsuffix .o,$(basename $(HASH_SRC)))

.PHONY: default
default: collisionsTest

C_SRCDIRS = $(shell find allcodecs -type d)
CXX_SRCDIRS = $(shell find allcodecs -type d)
include ../../build/make/multiconf.make

.PHONY: all
all: collisionsTest

collisionsTest: CXXFLAGS := -O3 $(CXXFLAGS)
collisionsTest: CFLAGS := -O3 $(CFLAGS)
$(eval $(call cxx_program,collisionsTest,main.o pool.o threading.o sort.o $(HASH_OBJ)))

.PHONY: debug
debug: CPPFLAGS += -DDEBUG -DXXH_NO_INLINE_HINTS
debug:
	CFLAGS='$(CFLAGS) -g3 -Og' CXXFLAGS='$(CXXFLAGS) -g3 -Og' CPPFLAGS='$(CPPFLAGS)' $(MAKE) collisionsTest

.PHONY: check
check: test

.PHONY: test
test: debug
	@echo ""
	@echo "## $(TESTHASHES) hashes"
	@time ./collisionsTest --nbh=$(TESTHASHES)
	@echo ""
	@echo "## $(TESTHASHES) hashes with filter"
	@time ./collisionsTest --nbh=$(TESTHASHES) --filter
	@echo ""
	@echo "## $(TESTHASHES) hashes with 2 threads"
	@time ./collisionsTest --nbh=$(TESTHASHES) --threadlog=1
	@echo ""

.PHONY: clean
clean:
	$(RM) *.o allcodecs/*.o
