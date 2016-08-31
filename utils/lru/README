The test-lru.rb program can be used in order to check the behavior of the
Redis approximated LRU algorithm against the theoretical output of true
LRU algorithm.

In order to use the program you need to recompile Redis setting the define
REDIS_LRU_CLOCK_RESOLUTION to 1, by editing the file server.h.
This allows to execute the program in a fast way since the 1 ms resolution
is enough for all the objects to have a different enough time stamp during
the test.

The program is executed like this:

    ruby test-lru.rb /tmp/lru.html

You can optionally specify a number of times to run, so that the program
will output averages of different runs, by adding an additional argument.
For instance in order to run the test 10 times use:

    ruby test-lru.rb /tmp/lru.html 10
