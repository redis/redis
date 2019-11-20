
# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

ifeq ($(DEBUG),1)
SHOBJ_MORE_CFLAGS += -O0
SHOBJ_MORE_LDFLAGS = -g
else
SHOBJ_MORE_CFLAGS += -O2
endif

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb $(SHOBJ_MORE_CFLAGS)
	SHOBJ_LDFLAGS ?= -shared $(SHOBJ_MORE_LDFLAGS)
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb $(SHOBJ_MORE_CFLAGS)
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup $(SHOBJ_MORE_LDFLAGS)
endif

TEST_MODULES = \
    commandfilter.so \
    testrdb.so \
    fork.so \
    infotest.so \
    propagate.so \
    misc.so \
    hooks.so \
    blockonkeys.so \
    scan.so \
    datatype.so

.PHONY: all

all: $(TEST_MODULES)

%.xo: %.c ../../src/redismodule.h
	$(CC) -I../../src $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

$(filter-out misc.so,$(TEST_MODULES)): %.so: %.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

misc.so: %.so: %.xo misc1.xo
	$(LD) -o $@ $^ $(SHOBJ_LDFLAGS) $(LIBS) -lc

.PHONY: clean

clean:
	rm -f $(TEST_MODULES) $(TEST_MODULES:.so=.xo) misc1.xo
