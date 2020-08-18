
include ../module.mk

all: tlsauth.so

tlsauth.xo: $(REDISMODULE_H)

tlsauth.so: tlsauth.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc
