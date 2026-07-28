# Redis CRoaring dependency Makefile

STD = -std=gnu11
WARN = -Wall
OPT = -O3

AR ?= ar
ARFLAGS = rcs

CROARING_CFLAGS = $(STD) $(WARN) $(OPT) $(ENABLE_LTO) $(CFLAGS) -fPIC \
	-fvisibility=hidden -DROARING_DISABLE_X64=1 -DDISABLENEON=1 \
	-Iinclude -Isrc
CROARING_BUILD_SETTINGS = .make-build-settings
REFCOUNT_NONE_TEST = .make-refcount-none-test

ROARING_SRC = \
	src/isadetection.c \
	src/array_util.c \
	src/bitset_util.c \
	src/art/art.c \
	src/bitset.c \
	src/containers/array.c \
	src/containers/bitset.c \
	src/containers/containers.c \
	src/containers/convert.c \
	src/containers/mixed_intersection.c \
	src/containers/mixed_union.c \
	src/containers/mixed_equal.c \
	src/containers/mixed_subset.c \
	src/containers/mixed_negation.c \
	src/containers/mixed_xor.c \
	src/containers/mixed_andnot.c \
	src/containers/run.c \
	src/memory.c \
	src/roaring.c \
	src/roaring64.c \
	src/roaring_priority_queue.c \
	src/roaring_array.c

ROARING_OBJ = $(ROARING_SRC:.c=.o)

libroaring.a: $(ROARING_OBJ)
	$(AR) $(ARFLAGS) $@ $+

$(CROARING_BUILD_SETTINGS): FORCE
	@printf '%s\n' 'CROARING_CFLAGS=$(CROARING_CFLAGS)' > $@.tmp
	@if test -r $@ && cmp -s $@ $@.tmp; then \
		rm $@.tmp; \
	else \
		mv $@.tmp $@; \
		rm -f $(ROARING_OBJ) libroaring.a; \
	fi

%.o: %.c $(CROARING_BUILD_SETTINGS)
	$(CC) $(CROARING_CFLAGS) -c -o $@ $<

$(REFCOUNT_NONE_TEST): tests/refcount_none.c include/roaring/portability.h
	$(CC) $(CROARING_CFLAGS) $(LDFLAGS) \
		-DCROARING_ATOMIC_IMPL=CROARING_ATOMIC_IMPL_NONE -o $@ $<

test: $(REFCOUNT_NONE_TEST)
	./$(REFCOUNT_NONE_TEST)

clean:
	rm -f $(ROARING_OBJ)
	rm -f libroaring.a
	rm -f $(REFCOUNT_NONE_TEST)
	rm -f $(CROARING_BUILD_SETTINGS) $(CROARING_BUILD_SETTINGS).tmp

FORCE:

.PHONY: clean test FORCE
