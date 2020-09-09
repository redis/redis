# source tests/support/util.tcl

set testmodule [file normalize tests/modules/blockedclient.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Locked GIL acquisition} {
        assert_match "OK" [r acquire_gil]
    }
}
