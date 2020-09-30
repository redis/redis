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

    $rd1 close
}
