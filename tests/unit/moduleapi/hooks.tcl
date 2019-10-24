set testmodule [file normalize tests/modules/hooks.so]

tags "modules" {
    start_server {} {
        r module load $testmodule
        test {Test clients connection / disconnection hooks} {
            for {set j 0} {$j < 2} {incr j} {
                set rd1 [redis_deferring_client]
                $rd1 close
            }

            r select 0
            puts "Keys: [r keys *]"
            assert {[r llen connected] > 1}
            assert {[r llen disconnected] > 1}
        }
    }
}
