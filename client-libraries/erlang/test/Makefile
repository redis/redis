include ../support/include.mk

all: $(EBIN_FILES)

clean:
	rm -rf $(EBIN_FILES) erl_crash.dump

test: $(MODULES)

./$(MODULES):
	@echo "Running tests for $@"
	erl -pa ../ebin -run $@ test -run init stop -noshell
