set testmodule [file normalize tests/modules/eventloop.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module eventloop sendbytes" {
        assert_match "OK" [r test.sendbytes 5000000]
        assert_match "OK" [r test.sendbytes 2000000]
    }

    test "Module eventloop iteration" {
        set iteration [r test.iteration]
        set next_iteration [r test.iteration]
        assert {$next_iteration > $iteration}
    }

    test "Module eventloop sanity" {
        r test.sanity
    }

    test "Module eventloop oneshot" {
        r test.oneshot
    }

    test "Unload the module - eventloop" {
        assert_equal {OK} [r module unload eventloop]
    }
}
