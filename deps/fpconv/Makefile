STD=
WARN= -Wall
# -O3, not -Os: at -Os the compiler declines to strength-reduce the constant
# divisors in generate_digits() (a multiply-shift costs more bytes than a divq)
# and does not unroll the digit loop, so the hot double->string path executes
# real 64-bit divisions. See the comment on tens[] in fpconv_dtoa.c.
OPT= -O3

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

AR= ar
ARFLAGS= rcs

libfpconv.a: fpconv_dtoa.o
	$(AR) $(ARFLAGS) $@ $+

fpconv_dtoa.o: fpconv_dtoa.h fpconv_dtoa.c

.c.o:
	$(R_CC) -c  $< 

clean:
	rm -f *.o
	rm -f *.a


