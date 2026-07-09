ifeq ($(origin CC),default)
CC := clang
endif

ifneq ($(MAKECMDGOALS),clean)
ifeq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null | head -1)))
$(error Redis fuzz targets require clang. Re-run with CC=clang)
endif
endif

FUZZ_CFLAGS ?= -DREDIS_FUZZ -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined
FUZZ_LDFLAGS ?= -fsanitize=fuzzer,address,undefined

.PHONY: all clean

all:
	$(MAKE) -C ../src fuzz CC="$(CC)" MALLOC=libc OPTIMIZATION=-O1 REDIS_CFLAGS="$(FUZZ_CFLAGS)" REDIS_LDFLAGS="$(FUZZ_LDFLAGS)"

clean:
	rm -f fuzz_string_commands fuzz_bitmap_commands *.o *.d support/*.o support/*.d
