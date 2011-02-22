Where to find complete Redis documentation?
===========================================

This `README` is just a fast "quick start" document. You can find more detailed
documentation here:

1. http://code.google.com/p/redis
2. Check the `doc` directory. `doc/README.html` is a good starting point :)

Building Redis
--------------

It is as simple as:

    % make

Redis is just a single binary, but if you want to install it you can use
the `make install` target that will copy the binary in `/usr/local/bin`
for default. You can also use `make PREFIX=/some/other/directory install`
if you wish to use a different destination.

You can run a 32 bit Redis binary using:

    % make 32bit

After you build Redis is a good idea to test it, using:

    % make test

Buliding using tcmalloc
-----------------------

`tcmalloc` is a fast and space efficient implementation (for little objects)
of `malloc()`. Compiling Redis with it can improve performances and memeory
usage. You can read more about it here:

http://goog-perftools.sourceforge.net/doc/tcmalloc.html

In order to compile Redis with tcmalloc support install tcmalloc on your system
and then use:

    % make USE_TCMALLOC=yes

Note that you can pass any other target to make, as long as you append
`USE_TCMALLOC=yes` at the end.

Running Redis
-------------

To run Redis with the default configuration just type:

    % cd src
    % ./redis-server

If you want to provide your redis.conf, you have to run it using an additional
parameter (the path of the configuration file):

    % cd src
    % ./redis-server /path/to/redis.conf

Playing with Redis
------------------

You can use `redis-cli` to play with Redis. Start a `redis-server` instance,
then in another terminal try the following:

    % cd src
    % ./redis-cli
    redis> ping
    PONG
    redis> set foo bar
    OK
    redis> get foo
    "bar"
    redis> incr mycounter
    (integer) 1
    redis> incr mycounter
    (integer) 2
    redis>

You can find the list of all the available commands here:

    http://code.google.com/p/redis/wiki/CommandReference

Enjoy!
