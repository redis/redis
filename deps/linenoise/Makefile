STD=
WARN= -Wall
OPT= -Os

R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
R_LDFLAGS= $(LDFLAGS)
DEBUG= -g

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

linenoise.o: linenoise.h linenoise.c

linenoise_example: linenoise.o example.o
	$(R_LD) -o $@ $^

.c.o:
	$(R_CC) -o $@ -c $<

clean:
	rm -f linenoise_example *.o $(OBJDIR)/*.o
