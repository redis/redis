set testmodule [file normalize tests/modules/eventloop.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module eventloop sendbytes" {
        assert_match "OK" [r test.sendbytes 0 10000000]
        assert_match "OK" [r test.sendbytes 0 2000000]
        assert_match "OK" [r test.sendbytes 0 800000000]
        assert_match "OK" [r test.sendbytes 1 10000000]
        assert_match "OK" [r test.sendbytes 1 2000000]
    }

    test "Module eventloop iteration" {
        set iteration [r test.iteration]
        set next_iteration [r test.iteration]
        assert {$next_iteration > $iteration}
    }
}
