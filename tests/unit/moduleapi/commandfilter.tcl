set testmodule [file normalize tests/modules/commandfilter.so]

start_server {tags {"modules"}} {
    r module load $testmodule log-key 0

    test {Retain a command filter argument} {
        # Retain an argument now. Later we'll try to re-read it and make sure
        # it is not corrupt and that valgrind does not complain.
        r rpush some-list @retain my-retained-string
        r commandfilter.retained
    } {my-retained-string}

    test {Command Filter handles redirected commands} {
        r set mykey @log
        r lrange log-key 0 -1
    } "{set mykey @log}"

    test {Command Filter can call RedisModule_CommandFilterArgDelete} {
        r rpush mylist elem1 @delme elem2
        r lrange mylist 0 -1
    } {elem1 elem2}

    test {Command Filter can call RedisModule_CommandFilterArgInsert} {
        r del mylist
        r rpush mylist elem1 @insertbefore elem2 @insertafter elem3
        r lrange mylist 0 -1
    } {elem1 --inserted-before-- @insertbefore elem2 @insertafter --inserted-after-- elem3}

    test {Command Filter can call RedisModule_CommandFilterArgReplace} {
        r del mylist
        r rpush mylist elem1 @replaceme elem2
        r lrange mylist 0 -1
    } {elem1 --replaced-- elem2}

    test {Command Filter applies on RM_Call() commands} {
        r del log-key
        r commandfilter.ping
        r lrange log-key 0 -1
    } "{ping @log}"

    test {Command Filter applies on Lua redis.call()} {
        r del log-key
        r eval "redis.call('ping', '@log')" 0
        r lrange log-key 0 -1
    } "{ping @log}"

    test {Command Filter applies on Lua redis.call() that calls a module} {
        r del log-key
        r eval "redis.call('commandfilter.ping')" 0
        r lrange log-key 0 -1
    } "{ping @log}"

    test {Command Filter strings can be retained} {
        r commandfilter.retained
    } {my-retained-string}

    test {Command Filter is unregistered implicitly on module unload} {
        r del log-key
        r module unload commandfilter
        r set mykey @log
        r lrange log-key 0 -1
    } {}

    r module load $testmodule log-key 0

    test {Command Filter unregister works as expected} {
        # Validate reloading succeeded
        r del log-key
        r set mykey @log
        assert_equal "{set mykey @log}" [r lrange log-key 0 -1]

        # Unregister
        r commandfilter.unregister
        r del log-key

        r set mykey @log
        r lrange log-key 0 -1
    } {}

    r module unload commandfilter
    r module load $testmodule log-key 1

    test {Command Filter REDISMODULE_CMDFILTER_NOSELF works as expected} {
        r set mykey @log
        assert_equal "{set mykey @log}" [r lrange log-key 0 -1]

        r del log-key
        r commandfilter.ping
        assert_equal {} [r lrange log-key 0 -1]

        r eval "redis.call('commandfilter.ping')" 0
        assert_equal {} [r lrange log-key 0 -1]
    }

    test "Unload the module - commandfilter" {
        assert_equal {OK} [r module unload commandfilter]
    }
} 

test {RM_CommandFilterArgInsert and script argv caching} {
    # coverage for scripts calling commands that expand the argv array
    # an attempt to add coverage for a possible bug in luaArgsToRedisArgv
    # this test needs a fresh server so that lua_argv_size is 0.
    # glibc realloc can return the same pointer even when the size changes
    # still this test isn't able to trigger the issue, but we keep it anyway.
    start_server {tags {"modules"}} {
        r module load $testmodule log-key 0
        r del mylist
        # command with 6 args
        r eval {redis.call('rpush', KEYS[1], 'elem1', 'elem2', 'elem3', 'elem4')} 1 mylist
        # command with 3 args that is changed to 4
        r eval {redis.call('rpush', KEYS[1], '@insertafter')} 1 mylist
        # command with 6 args again
        r eval {redis.call('rpush', KEYS[1], 'elem1', 'elem2', 'elem3', 'elem4')} 1 mylist
        assert_equal [r lrange mylist 0 -1] {elem1 elem2 elem3 elem4 @insertafter --inserted-after-- elem1 elem2 elem3 elem4}
    }
}

