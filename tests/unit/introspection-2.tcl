proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

start_server {tags {"introspection"}} {
    test {TTL, TYPE and EXISTS do not alter the last access time of a key} {
        r set foo bar
        after 3000
        r ttl foo
        r type foo
        r exists foo
        assert {[r object idletime foo] >= 2}
    }

    test {TOUCH alters the last access time of a key} {
        r set foo bar
        after 3000
        r touch foo
        assert {[r object idletime foo] < 2}
    }

    test {TOUCH returns the number of existing keys specified} {
        r flushdb
        r set key1{t} 1
        r set key2{t} 2
        r touch key0{t} key1{t} key2{t} key3{t}
    } 2

    test {command stats for GEOADD} {
        r config resetstat
        r GEOADD foo 0 0 bar
        assert_match {*calls=1,*} [cmdstat geoadd]
        assert_match {} [cmdstat zadd]
    } {} {needs:config-resetstat}

    test {command stats for EXPIRE} {
        r config resetstat
        r SET foo bar
        r EXPIRE foo 0
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {} [cmdstat del]
    } {} {needs:config-resetstat}

    test {command stats for BRPOP} {
        r config resetstat
        r LPUSH list foo
        r BRPOP list 0
        assert_match {*calls=1,*} [cmdstat brpop]
        assert_match {} [cmdstat rpop]
    } {} {needs:config-resetstat}

    test {command stats for MULTI} {
        r config resetstat
        r MULTI
        r set foo{t} bar
        r GEOADD foo2{t} 0 0 bar
        r EXPIRE foo2{t} 0
        r EXEC
        assert_match {*calls=1,*} [cmdstat multi]
        assert_match {*calls=1,*} [cmdstat exec]
        assert_match {*calls=1,*} [cmdstat set]
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {*calls=1,*} [cmdstat geoadd]
    } {} {needs:config-resetstat}

    test {command stats for scripts} {
        r config resetstat
        r set mykey myval
        r eval {
            redis.call('set', KEYS[1], 0)
            redis.call('expire', KEYS[1], 0)
            redis.call('geoadd', KEYS[1], 0, 0, "bar")
        } 1 mykey
        assert_match {*calls=1,*} [cmdstat eval]
        assert_match {*calls=2,*} [cmdstat set]
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {*calls=1,*} [cmdstat geoadd]
    } {} {needs:config-resetstat}
}
