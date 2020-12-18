start_server {tags {"pause"}} {
    proc verify_command_blocked {rd id} {   
        r unblock client $id
        assert_match "-UNBLOCKED client unblocked via CLIENT UNBLOCK" [$rd read]
    }

    test "Test various write commands are blocked by client pause" {
        r client PAUSE 100000000 READONLY
        set rd [redis_deferring_client]
        $rd client id
        set id [$rd read]
        [$rd id]

        # Test basic write commands
        $rd SET FOO BAR
        verify_command_blocked $rd $id
    }
}