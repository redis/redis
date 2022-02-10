set testmodule [file normalize tests/modules/test_lazyfree.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "modules allocated memory can be reclaimed in the background" {
        set orig_mem [s used_memory]
        set rd [redis_deferring_client]

        # LAZYFREE_THRESHOLD is 64
        for {set i 0} {$i < 10000} {incr i} {
            $rd lazyfreelink.insert lazykey $i
        }

        for {set j 0} {$j < 10000} {incr j} {
            $rd read 
        }

        assert {[r lazyfreelink.len lazykey] == 10000}

        set peak_mem [s used_memory]
        assert {[r unlink lazykey] == 1}
        assert {$peak_mem > $orig_mem+10000}
        wait_for_condition 50 100 {
            [s used_memory] < $peak_mem &&
            [s used_memory] < $orig_mem*2 &&
            [string match {*lazyfreed_objects:1*} [r info Memory]]
        } else {
            fail "Module memory is not reclaimed by UNLINK"
        }
    }
}
