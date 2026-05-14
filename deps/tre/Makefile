STD= -std=c99
WARN= -Wall
OPT= -Os

ifeq ($(SANITIZER),address)
	CFLAGS+=-fsanitize=address -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=address
else
ifeq ($(SANITIZER),undefined)
	CFLAGS+=-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=undefined
else
ifeq ($(SANITIZER),thread)
	CFLAGS+=-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=thread
else
ifeq ($(SANITIZER),memory)
	CFLAGS+=-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-sanitize-recover=all -fno-omit-frame-pointer
	LDFLAGS+=-fsanitize=memory
endif
endif
endif
endif

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS) -DTRE_REGEX_T_FIELD=value -Ilocal_includes -Ilib
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

TRE_OBJ=lib/regcomp.o lib/regerror.o lib/regexec.o lib/tre-ast.o lib/tre-compile.o \
	lib/tre-filter.o lib/tre-match-backtrack.o lib/tre-match-parallel.o \
	lib/tre-mem.o lib/tre-parse.o lib/tre-stack.o lib/xmalloc.o
TRE_TESTS=tests/retest tests/test-str-source tests/test-literal-opt tests/test-malformed-regn

libtre.a: $(TRE_OBJ)
	$(AR) $(ARFLAGS) $@ $+

check: $(TRE_TESTS)
	@set -e; \
	for test in $(TRE_TESTS); do \
		echo "TEST $$test"; \
		./$$test; \
	done

tests/retest: tests/retest.c libtre.a
	$(R_LD) $(R_CFLAGS) -DHAVE_REGNEXEC -DHAVE_REGNCOMP -o $@ $< libtre.a

tests/test-str-source: tests/test-str-source.c libtre.a
	$(R_LD) $(R_CFLAGS) -o $@ $< libtre.a

tests/test-literal-opt: tests/test-literal-opt.c libtre.a
	$(R_LD) $(R_CFLAGS) -o $@ $< libtre.a

tests/test-malformed-regn: tests/test-malformed-regn.c libtre.a
	$(R_LD) $(R_CFLAGS) -o $@ $< libtre.a

lib/regcomp.o: lib/regcomp.c local_includes/tre.h local_includes/tre-config.h lib/tre-internal.h lib/xmalloc.h
lib/regerror.o: lib/regerror.c local_includes/tre.h
lib/regexec.o: lib/regexec.c local_includes/tre.h lib/tre-internal.h lib/xmalloc.h
lib/tre-ast.o: lib/tre-ast.c lib/tre-ast.h lib/tre-internal.h
lib/tre-compile.o: lib/tre-compile.c lib/tre-compile.h lib/tre-internal.h lib/tre-mem.h lib/tre-parse.h lib/tre-stack.h lib/xmalloc.h
lib/tre-filter.o: lib/tre-filter.c lib/tre-filter.h lib/tre-internal.h
lib/tre-match-backtrack.o: lib/tre-match-backtrack.c lib/tre-internal.h lib/tre-match-utils.h lib/tre-mem.h lib/tre-stack.h
lib/tre-match-parallel.o: lib/tre-match-parallel.c lib/tre-internal.h lib/tre-match-utils.h lib/tre-mem.h
lib/tre-mem.o: lib/tre-mem.c lib/tre-mem.h
lib/tre-parse.o: lib/tre-parse.c lib/tre-ast.h lib/tre-compile.h lib/tre-filter.h lib/tre-internal.h lib/tre-mem.h lib/tre-parse.h lib/tre-stack.h lib/xmalloc.h
lib/tre-stack.o: lib/tre-stack.c lib/tre-internal.h lib/tre-stack.h
lib/xmalloc.o: lib/xmalloc.c lib/xmalloc.h

.c.o:
	$(R_CC) -c -o $@ $<

clean:
	rm -f $(TRE_OBJ) libtre.a $(TRE_TESTS)
