start_server {tags {"swap" "perflog"}} {
    test {perflog basics} {
        assert_equal [r perflog len] 0

        # perflog disabled by default
        r debug set-debug-rio-latency 10; # 10ms
        r get not-existing-key
        assert_equal [r perflog len] 0

        r config set swap-perflog-sample-ratio 100
        r get not-existing-key
        assert_equal [r perflog len] 1
        assert_match {*NOP*get*not-existing-key*} [lindex [r perflog get] 0] 

        r config set swap-perflog-max-len 5
        for {set i 0} {$i < 10} {incr i} {
            r get not-existing-key
        }
        assert_equal [r perflog len] 5
        assert_equal [llength [r perflog get 2]] 2

        r perflog reset
        assert_equal [r perflog len] 0
        assert_equal [r perflog get] {}

        r config set swap-perflog-sample-ratio 50
        r config set swap-perflog-log-slower-than 1000
        r config set swap-perflog-max-len 10
        r debug set-debug-rio-latency 1; #1ms

        for {set i 0} {$i < 10} {incr i} {
            r get not-existing-key
        }
        assert {[llength [r perflog get]] < 10 && [r perflog len] > 0}
    }
}

start_server {tags {"swap" "perflog"}} {
    test {save & load generates can safely generate perflog} {
        r config set swap-perflog-sample-ratio 100
        r config set swap-perflog-log-slower-than 1000
        r debug set-active-expire 0

        for {set i 0} {$i < 100} {incr i} {
            r set foo-$i bar-$i
        }
        r perflog reset

        r debug set-debug-rio-latency 1; #1ms
        r debug reload

        assert {[r perflog len] > 0}
    }
}
