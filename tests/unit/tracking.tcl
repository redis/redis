start_server {tags {"tracking"}} {
    # Create a deferred client we'll use to redirect invalidation
    # messages to.
    set rd1 [redis_deferring_client]
    $rd1 client id
    set redir [$rd1 read]
    $rd1 subscribe __redis__:invalidate
    $rd1 read ; # Consume the SUBSCRIBE reply.

    # Create another client as well in order to test NOLOOP
    set rd2 [redis_deferring_client]

    test {Clients are able to enable tracking and redirect it} {
        r CLIENT TRACKING on REDIRECT $redir
    } {*OK}

    test {The other connection is able to get invalidations} {
        r SET a 1
        r SET b 1
        r GET a
        r INCR b ; # This key should not be notified, since it wasn't fetched. 
        r INCR a 
        set keys [lindex [$rd1 read] 2]
        assert {[llength $keys] == 1}
        assert {[lindex $keys 0] eq {a}}
    }

    test {The client is now able to disable tracking} {
        # Make sure to add a few more keys in the tracking list
        # so that we can check for leaks, as a side effect.
        r MGET a b c d e f g
        r CLIENT TRACKING off
    }

    test {Clients can enable the BCAST mode with the empty prefix} {
        r CLIENT TRACKING on BCAST REDIRECT $redir
    } {*OK*}

    test {The connection gets invalidation messages about all the keys} {
        r MSET a 1 b 2 c 3
        set keys [lsort [lindex [$rd1 read] 2]]
        assert {$keys eq {a b c}}
    }

    test {Clients can enable the BCAST mode with prefixes} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir PREFIX a: PREFIX b:
        r MULTI
        r INCR a:1
        r INCR a:2
        r INCR b:1
        r INCR b:2
        r EXEC
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different prefixes.
        set keys1 [lsort [lindex [$rd1 read] 2]]
        set keys2 [lsort [lindex [$rd1 read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {a:1 a:2 b:1 b:2}}
    }
    
    test {Adding prefixes to BCAST mode works} {
        r CLIENT TRACKING on BCAST REDIRECT $redir PREFIX c:
        r INCR c:1234
        set keys [lsort [lindex [$rd1 read] 2]]
        assert {$keys eq {c:1234}}
    }

    test {Tracking NOLOOP mode in standard mode works} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir NOLOOP
        r MGET otherkey1 loopkey otherkey2
        $rd2 SET otherkey1 1; # We should get this
        r SET loopkey 1 ; # We should not get this
        $rd2 SET otherkey2 1; # We should get this
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different prefixes.
        set keys1 [lsort [lindex [$rd1 read] 2]]
        set keys2 [lsort [lindex [$rd1 read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {otherkey1 otherkey2}}
    }

    test {Tracking NOLOOP mode in BCAST mode works} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir NOLOOP
        $rd2 SET otherkey1 1; # We should get this
        r SET loopkey 1 ; # We should not get this
        $rd2 SET otherkey2 1; # We should get this
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different prefixes.
        set keys1 [lsort [lindex [$rd1 read] 2]]
        set keys2 [lsort [lindex [$rd1 read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {otherkey1 otherkey2}}
    }

    test {Tracking gets notification of expired keys} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir NOLOOP
        r SET mykey myval px 1
        r SET mykeyotherkey myval ; # We should not get it
        after 1000
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different prefixes.
        set keys1 [lsort [lindex [$rd1 read] 2]]
        set keys [lsort [list {*}$keys1]]
        assert {$keys eq {mykey}}
    }

    test {Tracking gets notification of lazy expired keys} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir NOLOOP
        # Use multi-exec to expose a race where the key gets an two invalidations
        # in the same event loop, once by the client so filtered by NOLOOP, and
        # the second one by the lazy expire
        r MULTI
        r SET mykey{t} myval px 1
        r SET mykeyotherkey{t} myval ; # We should not get it
        r DEBUG SLEEP 0.1
        r GET mykey{t}
        r EXEC
        set keys [lsort [lindex [$rd1 read] 2]]
        assert {$keys eq {mykey{t}}}
    } {}

    test {Tracking gets notification on tracking table key eviction} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir NOLOOP
        r MSET key1 1 key2 2
        # Let the server track the two keys for us
        r MGET key1 key2
        # Force the eviction of all the keys but one:
        r config set tracking-table-max-keys 1
        # Note that we may have other keys in the table for this client,
        # since we disabled/enabled tracking multiple time with the same
        # ID, and tracking does not do ID cleanups for performance reasons.
        # So we check that eventually we'll receive one or the other key,
        # otherwise the test will die for timeout.
        while 1 {
            set keys [lindex [$rd1 read] 2]
            if {$keys eq {key1} || $keys eq {key2}} break
        }
        # We should receive an expire notification for one of
        # the two keys (only one must remain)
        assert {$keys eq {key1} || $keys eq {key2}}
    }
}

start_server {tags {"tracking network"}} {
    # Create a deferred client we'll use to redirect invalidation
    # messages to.
    set rd_redirection [redis_deferring_client]
    $rd_redirection client id
    set redir_id [$rd_redirection read]
    $rd_redirection subscribe __redis__:invalidate
    $rd_redirection read ; # Consume the SUBSCRIBE reply.

    # Create another client that's not used as a redirection client
    # We should always keep this client's buffer clean
    set rd [redis_deferring_client]

    # Client to be used for SET and GET commands
    # We don't read this client's buffer
    set rd_sg [redis_client] 

    proc clean_all {} {
        uplevel {
            # We should make r TRACKING off first. If r is in RESP3,
            # r FLUSH ALL will send us tracking-redir-broken or other
            # info which will not be consumed.
            r CLIENT TRACKING off
            $rd QUIT
            $rd_redirection QUIT
            set rd [redis_deferring_client]
            set rd_redirection [redis_deferring_client]
            $rd_redirection client id
            set redir_id [$rd_redirection read]
            $rd_redirection subscribe __redis__:invalidate
            $rd_redirection read ; # Consume the SUBSCRIBE reply.
            r FLUSHALL
            r HELLO 2
            r config set tracking-table-max-keys 1000000
        }
    }

    foreach resp {3 2} {
        test "RESP$resp based basic invalidation with client reply off" {
            # This entire test is mostly irrelevant for RESP2, but we run it anyway just for some extra coverage.
            clean_all

            $rd hello $resp
            $rd read
            $rd client tracking on
            $rd read

            $rd_sg set foo bar
            $rd get foo
            $rd read

            $rd client reply off

            $rd_sg set foo bar2

            if {$resp == 3} {
                assert_equal {invalidate foo} [$rd read]
            } elseif {$resp == 2} { } ;# Just coverage

            # Verify things didn't get messed up and no unexpected reply was pushed to the client.
            $rd client reply on
            assert_equal {OK} [$rd read]
            $rd ping
            assert_equal {PONG} [$rd read]
        }
    }

    test {RESP3 based basic redirect invalidation with client reply off} {
        clean_all

        set rd_redir [redis_deferring_client]
        $rd_redir hello 3
        $rd_redir read

        $rd_redir client id
        set rd_redir_id [$rd_redir read]

        $rd client tracking on redirect $rd_redir_id
        $rd read

        $rd_sg set foo bar
        $rd get foo
        $rd read

        $rd_redir client reply off

        $rd_sg set foo bar2
        assert_equal {invalidate foo} [$rd_redir read]

        # Verify things didn't get messed up and no unexpected reply was pushed to the client.
        $rd_redir client reply on
        assert_equal {OK} [$rd_redir read]
        $rd_redir ping
        assert_equal {PONG} [$rd_redir read]

        $rd_redir close
    }

    test {RESP3 based basic tracking-redir-broken with client reply off} {
        clean_all

        $rd hello 3
        $rd read
        $rd client tracking on redirect $redir_id
        $rd read

        $rd_sg set foo bar
        $rd get foo
        $rd read

        $rd client reply off

        $rd_redirection quit
        $rd_redirection read

        $rd_sg set foo bar2

        set res [lsearch -exact [$rd read] "tracking-redir-broken"]
        assert_morethan_equal $res 0

        # Verify things didn't get messed up and no unexpected reply was pushed to the client.
        $rd client reply on
        assert_equal {OK} [$rd read]
        $rd ping
        assert_equal {PONG} [$rd read]
    }

    $rd_redirection close
    $rd_sg close
    $rd close
}
