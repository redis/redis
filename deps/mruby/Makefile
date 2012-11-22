# mruby is using Rake (http://rake.rubyforge.org) as a build tool.
# We provide a minimalistic version called minirake inside of our 
# codebase.

RAKE = ruby ./minirake

.PHONY : all
all :
	$(RAKE)

.PHONY : test
test :
	$(RAKE) test

.PHONY : clean
clean :
	$(RAKE) clean

.PHONY : showconfig
showconfig :
	$(RAKE) showconfig
