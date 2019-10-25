set testmodule [file normalize tests/modules/hooks.so]

tags "modules" {
    start_server {} {
        r module load $testmodule
        test {Test clients connection / disconnection hooks} {
            for {set j 0} {$j < 2} {incr j} {
                set rd1 [redis_deferring_client]
                $rd1 close
            }
            assert {[r llen connected] > 1}
            assert {[r llen disconnected] > 1}
        }

        test {Test flushdb hooks} {
            r flushall ;# Note: only the "end" RPUSH will survive
            r select 1
            r flushdb
            r select 2
            r flushdb
            r select 9
            assert {[r llen flush-start] == 2}
            assert {[r llen flush-end] == 3}
            assert {[r lrange flush-start 0 -1] eq {1 2}}
            assert {[r lrange flush-end 0 -1] eq {-1 1 2}}
        }
    }
}
