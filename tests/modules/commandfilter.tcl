set testmodule [file normalize src/modules/hellofilter.so]

start_server {tags {"modules"}} {
    r module load $testmodule log-key

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
        r hellofilter.ping
        r lrange log-key 0 -1
    } "{ping @log}"

    test {Command Filter applies on Lua redis.call()} {
        r del log-key
        r eval "redis.call('ping', '@log')" 0
        r lrange log-key 0 -1
    } "{ping @log}"

    test {Command Filter applies on Lua redis.call() that calls a module} {
        r del log-key
        r eval "redis.call('hellofilter.ping')" 0
        r lrange log-key 0 -1
    } "{ping @log}"
} 
