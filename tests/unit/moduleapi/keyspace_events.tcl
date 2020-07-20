set testmodule [file normalize tests/modules/keyspace_events.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test loaded key space event} {
            r set x 1
            r hset y f v
            r lpush z 1 2 3
            r sadd p 1 2 3
            r zadd t 1 f1 2 f2
            r xadd s * f v
            r debug reload
            assert_equal 1 [r keyspace.is_key_loaded x]
            assert_equal 1 [r keyspace.is_key_loaded y]
            assert_equal 1 [r keyspace.is_key_loaded z]
            assert_equal 1 [r keyspace.is_key_loaded p]
            assert_equal 1 [r keyspace.is_key_loaded t]
            assert_equal 1 [r keyspace.is_key_loaded s]
        }
	}
}