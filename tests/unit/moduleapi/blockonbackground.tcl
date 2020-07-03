set testmodule [file normalize tests/modules/blockonbackground.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {SLOWLOG - check that logs commands taking more time than specified including the background time} {
        r config set slowlog-log-slower-than 100000
        r ping
        assert_equal [r slowlog len] 0
        r block.debug 0 10
        assert_equal [r slowlog len] 0
        r block.debug 2 10
        assert_equal [r slowlog len] 1
    }
}
