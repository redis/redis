start_server {tags {"swap" "perflog"}} {
    test {perflog basics} {
        assert_equal [r swap.perflog len] 0

        # swap perflog disabled by default
        r debug set-swap-debug-rio-delay 10; # 10ms
        r get not-existing-key
        assert_equal [r swap.perflog len] 0

        # manually set swap perflog on/off
        r swap.perflog on
        r get not-existing-key
        assert_equal [r swap.perflog len] 1

        r get loooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooongkey
        assert_equal [r swap.perflog len] 2
        r swap.perflog reset
        assert_equal [r swap.perflog len] 0
        r swap.perflog off

        r config set swap-perflog-sample-ratio 100
        r get not-existing-key
        assert_equal [r swap.perflog len] 1
        assert_match {*NOP*get*not-existing-key*} [lindex [r swap.perflog get] 0] 

        r config set swap-perflog-max-len 5
        for {set i 0} {$i < 10} {incr i} {
            r get not-existing-key
        }
        assert_equal [r swap.perflog len] 5
        assert_equal [llength [r swap.perflog get 2]] 2

        r swap.perflog reset
        assert_equal [r swap.perflog len] 0
        assert_equal [r swap.perflog get] {}

        r config set swap-perflog-sample-ratio 50
        r config set swap-perflog-log-slower-than-us 1000
        r config set swap-perflog-max-len 10
        r debug set-swap-debug-rio-delay 1; #1ms

        for {set i 0} {$i < 10} {incr i} {
            r get not-existing-key
        }
        assert {[llength [r swap.perflog get]] < 10 && [r swap.perflog len] > 0}
    }
}

start_server {tags {"swap" "perflog"}} {
    test {save & load can safely generate perflog} {
        r config set swap-perflog-sample-ratio 100
        r config set swap-perflog-log-slower-than-us 1000
        r debug set-active-expire 0

        for {set i 0} {$i < 100} {incr i} {
            r set foo-$i bar-$i
        }
        r swap.perflog reset

        r debug set-swap-debug-rio-delay 1; #1ms
        r debug reload

        assert {[r swap.perflog len] > 0}
    }
}
