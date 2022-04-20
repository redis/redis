
STD=-std=c99
FINAL_CFLAGS=$(STD) $(CFLAGS) -I./include -I./


CTRIP_CC=$(CC) $(FINAL_CFLAGS)
CTRIP_GTID_LIB=lib/libctrip_gtid.a
CTRIP_GTID_OBJ=ctrip_gtid.o zmalloc.o ctrip_util.o
AR=ar
ARFLAGS=rcu
DEBUG=-g -ggdb


PREFIX?=.
INSTALL_DIR?=$(PREFIX)/bin
INSTALL=cp -rf

.make-prerequisites:
	@touch $@

%.o: %.c .make-prerequisites
	echo $(CTRIP_CC)
	$(CTRIP_CC) $(DEBUG) -MMD -o $@ -c $< -DCTRIP_GTID_TEST


$(CTRIP_GTID_LIB): $(CTRIP_GTID_OBJ)
	@mkdir -p lib
	$(AR) $(ARFLAGS) $(CTRIP_GTID_LIB) $(CTRIP_GTID_OBJ)


all: $(CTRIP_GTID_LIB)

clean:
	rm -rf $(CTRIP_GTID_LIB) $(CTRIP_GTID_OBJ) ./ctrip_gtid_test.o
	rm -rf ctrip_gtid_test debug

test:  $(CTRIP_GTID_LIB) ./ctrip_gtid_test.o	
	$(CTRIP_CC)  -g -ggdb  -o  ctrip_gtid_test  ctrip_gtid_test.o ./lib/libctrip_gtid.a -lm -ldl -DCTRIP_GTID_TEST
	./ctrip_gtid_test

install: all
	@mkdir -p $(INSTALL_DIR)
	$(INSTALL) include $(INSTALL_DIR)
	$(INSTALL) lib $(INSTALL_DIR)


