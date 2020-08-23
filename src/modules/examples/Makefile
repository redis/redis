
include ../module.mk

all: helloworld.so hellotype.so helloblock.so testmodule.so hellocluster.so hellotimer.so hellodict.so hellohook.so helloacl.so

helloworld.xo: $(REDISMODULE_H)

helloworld.so: helloworld.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotype.xo: $(REDISMODULE_H)

hellotype.so: hellotype.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloblock.xo: $(REDISMODULE_H)

helloblock.so: helloblock.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lpthread -lc

hellocluster.xo: $(REDISMODULE_H)

hellocluster.so: hellocluster.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellotimer.xo: $(REDISMODULE_H)

hellotimer.so: hellotimer.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellodict.xo: $(REDISMODULE_H)

hellodict.so: hellodict.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

hellohook.xo: $(REDISMODULE_H)

hellohook.so: hellohook.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

helloacl.xo: $(REDISMODULE_H)

helloacl.so: helloacl.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

testmodule.xo: $(REDISMODULE_H)

testmodule.so: testmodule.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc
