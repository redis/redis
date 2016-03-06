SHOBJ_CFLAGS ?= -dynamic -fno-common -g -ggdb
SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup

.SUFFIXES: .c .so .xo .o

all: helloworld.so

.c.xo:
	$(CC) -I. $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

helloworld.xo: ../redismodule.h

helloworld.so: helloworld.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

clean:
	rm -rf *.xo *.so
