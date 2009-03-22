LIBDIR=`erl -eval 'io:format("~s~n", [code:lib_dir()])' -s init stop -noshell`

all:
	mkdir -p ebin/
	(cd src;$(MAKE))
	(cd test;$(MAKE))

clean: clean_tests
	(cd src;$(MAKE) clean)
	rm -rf erl_crash.dump *.beam

clean_tests:
	(cd test;$(MAKE) clean)
	rm -rf erl_crash.dump *.beam	

test: clean
	mkdir -p ebin/
	(cd src;$(MAKE))
	(cd test;$(MAKE))
	(cd test;$(MAKE) test)

testrun: all
	mkdir -p ebin/
	(cd test;$(MAKE) test)

install: all
	mkdir -p ${LIBDIR}/erldis-0.0.1/{ebin,include}
	for i in ebin/*.beam; do install $$i $(LIBDIR)/erldis-0.0.1/$$i ; done
	for i in include/*.hrl; do install $$i $(LIBDIR)/erldis-0.0.1/$$i ; done
