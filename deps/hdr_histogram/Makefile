STD=
WARN=
OPT=
CFLAGS=-Wc,dll,expo,langlvl\(extc99\),float\(ieee\)gonum,goff,hgpr,roconst,ASM,asmlib\('CEE.SCEEMAC','SYS1.MACLIB','SYS1.MODGEN'\)
R_CFLAGS= $(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
R_LDFLAGS= $(LDFLAGS)
DEBUG=

R_CC=$(CC) $(R_CFLAGS)
R_LD=$(CC) $(R_LDFLAGS)

hdr_histogram.o: hdr_histogram.h hdr_histogram.c 

.c.o:
	$(R_CC) -c  $< 

clean:
	rm -f *.o


