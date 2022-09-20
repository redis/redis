start_server {tags "multi"} {
    r config set debug-evict-keys 0; # evict manually

    test {transaction reserves cmd intention flag} {
        r set key val
        r evict key
        wait_key_cold r key
        assert {[rio_get_meta r key] != ""}
        r multi
        r del key
        r exec
        assert {[rio_get_meta r key] == ""}
    }

    test {scan in transaction} {
        r set key val
        r evict key
        wait_key_cold r key
        assert {[rio_get_meta r key] != ""}
        r multi
        r scan 1
        assert_equal [lindex [r exec] 1] {key}
    }
}

