start_server {tags {"introspection"}} {
    test {CLIENT LIST} {
        r client list
    } {*addr=*:* fd=* age=* idle=* flags=N db=9 sub=0 psub=0 multi=-1 qbuf=0 qbuf-free=* obl=0 oll=0 omem=0 events=r cmd=client*}

    test {MONITOR can log executed commands} {
        set rd [redis_deferring_client]
        $rd monitor
        r set foo bar
        r get foo
        list [$rd read] [$rd read] [$rd read]
    } {*OK*"set" "foo"*"get" "foo"*}

    test {MONITOR can log commands issued by the scripting engine} {
        set rd [redis_deferring_client]
        $rd monitor
        r eval {redis.call('set',KEYS[1],ARGV[1])} 1 foo bar
        $rd read ;# Discard the OK
        assert_match {*eval*} [$rd read]
        assert_match {*lua*"set"*"foo"*"bar"*} [$rd read]
    }
}
