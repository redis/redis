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

    test {Clients are able to enable tracking and redirect it} {
        r CLIENT TRACKING on REDIRECT $redir_id
    } {*OK}

    test {The other connection is able to get invalidations} {
        r SET a{t} 1
        r SET b{t} 1
        r GET a{t}
        r INCR b{t} ; # This key should not be notified, since it wasn't fetched.
        r INCR a{t}
        set keys [lindex [$rd_redirection read] 2]
        assert {[llength $keys] == 1}
        assert {[lindex $keys 0] eq {a{t}}}
    }

    test {The client is now able to disable tracking} {
        # Make sure to add a few more keys in the tracking list
        # so that we can check for leaks, as a side effect.
        r MGET a{t} b{t} c{t} d{t} e{t} f{t} g{t}
        r CLIENT TRACKING off
    } {*OK}

    test {Clients can enable the BCAST mode with the empty prefix} {
        r CLIENT TRACKING on BCAST REDIRECT $redir_id
    } {*OK*}

    test {The connection gets invalidation messages about all the keys} {
        r MSET a{t} 1 b{t} 2 c{t} 3
        set keys [lsort [lindex [$rd_redirection read] 2]]
        assert {$keys eq {a{t} b{t} c{t}}}
    }

    test {Clients can enable the BCAST mode with prefixes} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir_id PREFIX a: PREFIX b:
        r MULTI
        r INCR a:1{t}
        r INCR a:2{t}
        r INCR b:1{t}
        r INCR b:2{t}
        # we should not get this key
        r INCR c:1{t}
        r EXEC
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different prefixes.
        set keys1 [lsort [lindex [$rd_redirection read] 2]]
        set keys2 [lsort [lindex [$rd_redirection read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {a:1{t} a:2{t} b:1{t} b:2{t}}}
    }

    test {Adding prefixes to BCAST mode works} {
        r CLIENT TRACKING on BCAST REDIRECT $redir_id PREFIX c:
        r INCR c:1234
        set keys [lsort [lindex [$rd_redirection read] 2]]
        assert {$keys eq {c:1234}}
    }

    test {Tracking NOLOOP mode in standard mode works} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir_id NOLOOP
        r MGET otherkey1{t} loopkey{t} otherkey2{t}
        $rd_sg SET otherkey1{t} 1; # We should get this
        r SET loopkey{t} 1 ; # We should not get this
        $rd_sg SET otherkey2{t} 1; # We should get this
        # Because of the internals, we know we are going to receive
        # two separated notifications for the two different keys.
        set keys1 [lsort [lindex [$rd_redirection read] 2]]
        set keys2 [lsort [lindex [$rd_redirection read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {otherkey1{t} otherkey2{t}}}
    }

    test {Tracking NOLOOP mode in BCAST mode works} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir_id NOLOOP
        $rd_sg SET otherkey1 1; # We should get this
        r SET loopkey 1 ; # We should not get this
        $rd_sg SET otherkey2 1; # We should get this
        # Because $rd_sg send command synchronously, we know we are
        # going to receive two separated notifications.
        set keys1 [lsort [lindex [$rd_redirection read] 2]]
        set keys2 [lsort [lindex [$rd_redirection read] 2]]
        set keys [lsort [list {*}$keys1 {*}$keys2]]
        assert {$keys eq {otherkey1 otherkey2}}
    }

    test {Tracking gets notification of expired keys} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir_id NOLOOP
        r SET mykey myval px 1
        r SET mykeyotherkey myval ; # We should not get it
        after 1000
        set keys [lsort [lindex [$rd_redirection read] 2]]
        assert {$keys eq {mykey}}
    }

    test {Tracking gets notification of lazy expired keys} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST REDIRECT $redir_id NOLOOP
        # Use multi-exec to expose a race where the key gets an two invalidations
        # in the same event loop, once by the client so filtered by NOLOOP, and
        # the second one by the lazy expire
        r MULTI
        r SET mykey{t} myval px 1
        r SET mykeyotherkey{t} myval ; # We should not get it
        r DEBUG SLEEP 0.1
        r GET mykey{t}
        r EXEC
        set keys [lsort [lindex [$rd_redirection read] 2]]
        assert {$keys eq {mykey{t}}}
    } {} {needs:debug}

    test {HELLO 3 reply is correct} {
        set reply [r HELLO 3]
        assert_equal [dict get $reply proto] 3
    }

    test {HELLO without protover} {
        set reply [r HELLO 3]
        assert_equal [dict get $reply proto] 3

        set reply [r HELLO]
        assert_equal [dict get $reply proto] 3

        set reply [r HELLO 2]
        assert_equal [dict get $reply proto] 2

        set reply [r HELLO]
        assert_equal [dict get $reply proto] 2

        # restore RESP3 for next test
        r HELLO 3
    }

    test {RESP3 based basic invalidation} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on
        $rd_sg SET key1 1
        r GET key1
        $rd_sg SET key1 2
        r read
    } {invalidate key1}

    test {RESP3 tracking redirection} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_sg SET key1 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key1}}
    }

    test {Invalidations of previous keys can be redirected after switching to RESP3} {
        r HELLO 2
        $rd_sg SET key1 1
        r GET key1
        r HELLO 3
        $rd_sg SET key1 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key1}}
    }

    test {Invalidations of new keys can be redirected after switching to RESP3} {
        r HELLO 3
        $rd_sg SET key1 1
        r GET key1
        $rd_sg SET key1 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key1}}
    }

    test {Tracking only occurs for scripts when a command calls a read-only command} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on
        $rd_sg MSET key2{t} 1 key2{t} 1

        # If a script doesn't call any read command, don't track any keys
        r EVAL "redis.call('set', 'key3{t}', 'bar')" 2 key1{t} key2{t} 
        $rd_sg MSET key2{t} 2 key1{t} 2

        # If a script calls a read command, track all declared keys
        r EVAL "redis.call('get', 'key3{t}')" 2 key1{t} key2{t} 
        $rd_sg MSET key2{t} 2 key1{t} 2
        assert_equal {invalidate key2{t}} [r read]
        assert_equal {invalidate key1{t}} [r read]

        # RO variants work like the normal variants
        r EVAL_RO "redis.call('ping')" 2 key1{t} key2{t} 
        $rd_sg MSET key2{t} 2 key1{t} 2

        r EVAL_RO "redis.call('get', 'key1{t}')" 2 key1{t} key2{t} 
        $rd_sg MSET key2{t} 3 key1{t} 3
        assert_equal {invalidate key2{t}} [r read]
        assert_equal {invalidate key1{t}} [r read]

        assert_equal "PONG" [r ping]
    }

    test {RESP3 Client gets tracking-redir-broken push message after cached key changed when rediretion client is terminated} {
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_redirection QUIT
        assert_equal OK [$rd_redirection read]
        $rd_sg SET key1 2
        set MAX_TRIES 100
        set res -1
        for {set i 0} {$i <= $MAX_TRIES && $res < 0} {incr i} {
            set res [lsearch -exact [r PING] "tracking-redir-broken"]
        }
        assert {$res >= 0}
        # Consume PING reply
        assert_equal PONG [r read]

        # Reinstantiating after QUIT
        set rd_redirection [redis_deferring_client]
        $rd_redirection CLIENT ID
        set redir_id [$rd_redirection read]
        $rd_redirection SUBSCRIBE __redis__:invalidate
        $rd_redirection read ; # Consume the SUBSCRIBE reply
    }

    test {Different clients can redirect to the same connection} {
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd CLIENT TRACKING on REDIRECT $redir_id 
        assert_equal OK [$rd read] ; # Consume the TRACKING reply
        $rd_sg MSET key1{t} 1 key2{t} 1
        r GET key1{t}
        $rd GET key2{t} 
        assert_equal 1 [$rd read] ; # Consume the GET reply
        $rd_sg INCR key1{t}
        $rd_sg INCR key2{t}
        set res1 [lindex [$rd_redirection read] 2]
        set res2 [lindex [$rd_redirection read] 2]
        assert {$res1 eq {key1{t}}}
        assert {$res2 eq {key2{t}}}
    }

    test {Different clients using different protocols can track the same key} {
        $rd HELLO 3 
        set reply [$rd read] ; # Consume the HELLO reply
        assert_equal 3 [dict get $reply proto]
        $rd CLIENT TRACKING on 
        assert_equal OK [$rd read] ; # Consume the TRACKING reply
        $rd_sg set key1 1
        r GET key1
        $rd GET key1 
        assert_equal 1 [$rd read] ; # Consume the GET reply
        $rd_sg INCR key1
        set res1 [lindex [$rd_redirection read] 2]
        $rd PING ; # Non redirecting client has to talk to the server in order to get invalidation message
        set res2 [lindex [split [$rd read] " "] 1] 
        assert_equal PONG [$rd read] ; # Consume the PING reply, which comes together with the invalidation message
        assert {$res1 eq {key1}}
        assert {$res2 eq {key1}}
    }

    test {No invalidation message when using OPTIN option} {
        r CLIENT TRACKING on OPTIN REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1 ; # This key should not be notified, since OPTIN is on and CLIENT CACHING yes wasn't called
        $rd_sg SET key1 2
        # Preparing some message to consume on $rd_redirection so we don't get blocked
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key2 1
        r GET key2 ; # This key should be notified
        $rd_sg SET key2 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key2}}
    }

    test {Invalidation message sent when using OPTIN option with CLIENT CACHING yes} {
        r CLIENT TRACKING on OPTIN REDIRECT $redir_id
        $rd_sg SET key1 3
        r CLIENT CACHING yes
        r GET key1
        $rd_sg SET key1 4
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key1}}
    }

    test {Invalidation message sent when using OPTOUT option} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on OPTOUT REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1 
        $rd_sg SET key1 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key1}}
    }

    test {No invalidation message when using OPTOUT option with CLIENT CACHING no} {
        $rd_sg SET key1 1
        r CLIENT CACHING no
        r GET key1 ; # This key should not be notified, since OPTOUT is on and CLIENT CACHING no was called
        $rd_sg SET key1 2
        # Preparing some message to consume on $rd_redirection so we don't get blocked
        $rd_sg SET key2 1
        r GET key2 ; # This key should be notified
        $rd_sg SET key2 2
        set res [lindex [$rd_redirection read] 2]
        assert {$res eq {key2}}
    }

    test {Able to redirect to a RESP3 client} {
        $rd_redirection UNSUBSCRIBE __redis__:invalidate ; # Need to unsub first before we can do HELLO 3
        set res [$rd_redirection read] ; # Consume the UNSUBSCRIBE reply
        assert_equal {__redis__:invalidate} [lindex $res 1]
        $rd_redirection HELLO 3
        set res [$rd_redirection read] ; # Consume the HELLO reply
        assert_equal [dict get $reply proto] 3
        $rd_redirection SUBSCRIBE __redis__:invalidate
        set res [$rd_redirection read] ; # Consume the SUBSCRIBE reply
        assert_equal {__redis__:invalidate} [lindex $res 1]
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_sg INCR key1
        set res [lindex [$rd_redirection read] 1]
        assert {$res eq {key1}}
        $rd_redirection HELLO 2
        set res [$rd_redirection read] ; # Consume the HELLO reply
        assert_equal [dict get $res proto] 2
    }

    test {After switching from normal tracking to BCAST mode, no invalidation message is produced for pre-BCAST keys} {
        r CLIENT TRACKING off
        r HELLO 3
        r CLIENT TRACKING on
        $rd_sg SET key1 1
        r GET key1
        r CLIENT TRACKING off 
        r CLIENT TRACKING on BCAST
        $rd_sg INCR key1
        set inv_msg [r PING]
        set ping_reply [r read]
        assert {$inv_msg eq {invalidate key1}}
        assert {$ping_reply eq {PONG}}
    }

    test {BCAST with prefix collisions throw errors} {
        set r [redis_client] 
        catch {$r CLIENT TRACKING ON BCAST PREFIX FOOBAR PREFIX FOO} output
        assert_match {ERR Prefix 'FOOBAR'*'FOO'*} $output

        catch {$r CLIENT TRACKING ON BCAST PREFIX FOO PREFIX FOOBAR} output
        assert_match {ERR Prefix 'FOO'*'FOOBAR'*} $output

        $r CLIENT TRACKING ON BCAST PREFIX FOO PREFIX BAR
        catch {$r CLIENT TRACKING ON BCAST PREFIX FO} output
        assert_match {ERR Prefix 'FO'*'FOO'*} $output

        catch {$r CLIENT TRACKING ON BCAST PREFIX BARB} output
        assert_match {ERR Prefix 'BARB'*'BAR'*} $output

        $r CLIENT TRACKING OFF
    }

    test {hdel deliver invalidate message after response in the same connection} {
        r CLIENT TRACKING off
        r HELLO 3
        r CLIENT TRACKING on
        r HSET myhash f 1
        r HGET myhash f
        set res [r HDEL myhash f]
        assert_equal $res 1
        set res [r read]
        assert_equal $res {invalidate myhash}
    }

    test {Tracking invalidation message is not interleaved with multiple keys response} {
        r CLIENT TRACKING off
        r HELLO 3
        r CLIENT TRACKING on
        # We need disable active expire, so we can trigger lazy expire
        r DEBUG SET-ACTIVE-EXPIRE 0
        r MULTI
        r MSET x{t} 1 y{t} 2
        r PEXPIRE y{t} 100
        r GET y{t}
        r EXEC
        after 110
        # Read expired key y{t}, generate invalidate message about this key
        set res [r MGET x{t} y{t}]
        assert_equal $res {1 {}}
        # Consume the invalidate message which is after command response
        set res [r read]
        assert_equal $res {invalidate y{t}}
        r DEBUG SET-ACTIVE-EXPIRE 1
    } {OK} {needs:debug}

    test {Tracking invalidation message is not interleaved with transaction response} {
        r CLIENT TRACKING off
        r HELLO 3
        r CLIENT TRACKING on
        r MSET a{t} 1 b{t} 2
        r GET a{t}
        # Start a transaction, make a{t} generate an invalidate message
        r MULTI
        r INCR a{t}
        r GET b{t}
        set res [r EXEC]
        assert_equal $res {2 2}
        set res [r read]
        # Consume the invalidate message which is after command response
        assert_equal $res {invalidate a{t}}
    }

    test {Tracking invalidation message of eviction keys should be before response} {
        # Get the current memory limit and calculate a new limit.
        r CLIENT TRACKING off
        r HELLO 3
        r CLIENT TRACKING on

        # make the previous test is really done before sampling used_memory
        wait_lazyfree_done r

        set used [expr {[s used_memory] - [s mem_not_counted_for_evict]}]
        set limit [expr {$used+100*1024}]
        set old_policy [lindex [r config get maxmemory-policy] 1]
        r config set maxmemory $limit
        # We set policy volatile-random, so only keys with ttl will be evicted
        r config set maxmemory-policy volatile-random
        # Add a volatile key and tracking it.
        r setex volatile-key 10000 x
        r get volatile-key
        # We use SETBIT here, so we can set a big key and get the used_memory
        # bigger than maxmemory. Next command will evict volatile keys. We
        # can't use SET, as SET uses big input buffer, so it will fail.
        r setbit big-key 1600000 0 ;# this will consume 200kb
        # volatile-key is evicted before response.
        set res [r getbit big-key 0]
        assert_equal $res {invalidate volatile-key}
        set res [r read]
        assert_equal $res 0
        r config set maxmemory-policy $old_policy
        r config set maxmemory 0
    }

    test {Unblocked BLMOVE gets notification after response} {
        r RPUSH list2{t} a
        $rd HELLO 3
        $rd read
        $rd CLIENT TRACKING on
        $rd read
        # Tracking key list2{t}
        $rd LRANGE list2{t} 0 -1
        $rd read
        # We block on list1{t}
        $rd BLMOVE list1{t} list2{t} left left 0
        wait_for_blocked_clients_count 1
        # unblock $rd, list2{t} gets element and generate invalidation message
        r rpush list1{t} foo
        assert_equal [$rd read] {foo}
        assert_equal [$rd read] {invalidate list2{t}}
    }

    test {Tracking gets notification on tracking table key eviction} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on REDIRECT $redir_id NOLOOP
        r MSET key1{t} 1 key2{t} 2
        # Let the server track the two keys for us
        r MGET key1{t} key2{t}
        # Force the eviction of all the keys but one:
        r config set tracking-table-max-keys 1
        # Note that we may have other keys in the table for this client,
        # since we disabled/enabled tracking multiple time with the same
        # ID, and tracking does not do ID cleanups for performance reasons.
        # So we check that eventually we'll receive one or the other key,
        # otherwise the test will die for timeout.
        while 1 {
            set keys [lindex [$rd_redirection read] 2]
            if {$keys eq {key1{t}} || $keys eq {key2{t}}} break
        }
        # We should receive an expire notification for one of
        # the two keys (only one must remain)
        assert {$keys eq {key1{t}} || $keys eq {key2{t}}}
    }

    test {Invalidation message received for flushall} {
        clean_all
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_sg FLUSHALL
        set msg [$rd_redirection read]
        assert {[lindex msg 2] eq {} }
    }

    test {Invalidation message received for flushdb} {
        clean_all
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_sg FLUSHDB
        set msg [$rd_redirection read]
        assert {[lindex msg 2] eq {} }
    }

    test {Test ASYNC flushall} {
        clean_all
        r CLIENT TRACKING on REDIRECT $redir_id
        r GET key1
        r GET key2
        assert_equal [s 0 tracking_total_keys] 2
        $rd_sg FLUSHALL ASYNC
        assert_equal [s 0 tracking_total_keys] 0
        assert_equal [lindex [$rd_redirection read] 2] {}
    }

    # Keys are defined to be evicted 100 at a time by default.
    # If after eviction the number of keys still surpasses the limit
    # defined in tracking-table-max-keys, we increases eviction 
    # effort to 200, and then 300, etc. 
    # This test tests this effort incrementation. 
    test {Server is able to evacuate enough keys when num of keys surpasses limit by more than defined initial effort} {
        clean_all
        set NUM_OF_KEYS_TO_TEST 250
        set TRACKING_TABLE_MAX_KEYS 1
        r CLIENT TRACKING on REDIRECT $redir_id
        for {set i 0} {$i < $NUM_OF_KEYS_TO_TEST} {incr i} {
            $rd_sg SET key$i $i
            r GET key$i
        }
        r config set tracking-table-max-keys $TRACKING_TABLE_MAX_KEYS
        # If not enough keys are evicted, we won't get enough invalidation
        # messages, and "$rd_redirection read" will block.
        # If too many keys are evicted, we will get too many invalidation
        # messages, and the assert will fail.
        for {set i 0} {$i < $NUM_OF_KEYS_TO_TEST - $TRACKING_TABLE_MAX_KEYS} {incr i} {
            $rd_redirection read
        }
        $rd_redirection PING
        assert {[$rd_redirection read] eq {pong {}}}
    }

    test {Tracking info is correct} {
        clean_all
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        $rd_sg SET key2 2
        r GET key1 
        r GET key2
        $rd CLIENT TRACKING on BCAST PREFIX prefix:
        assert [string match *OK* [$rd read]]
        $rd_sg SET prefix:key1 1 
        $rd_sg SET prefix:key2 2
        set info [r info]
        regexp "\r\ntracking_total_items:(.*?)\r\n" $info _ total_items
        regexp "\r\ntracking_total_keys:(.*?)\r\n" $info _ total_keys
        regexp "\r\ntracking_total_prefixes:(.*?)\r\n" $info _ total_prefixes
        regexp "\r\ntracking_clients:(.*?)\r\n" $info _ tracking_clients
        assert {$total_items == 2}
        assert {$total_keys == 2}
        assert {$total_prefixes == 1}
        assert {$tracking_clients == 2}
    }

    test {CLIENT GETREDIR provides correct client id} {
        set res [r CLIENT GETREDIR]
        assert_equal $redir_id $res
        r CLIENT TRACKING off
        set res [r CLIENT GETREDIR]
        assert_equal -1 $res
        r CLIENT TRACKING on
        set res [r CLIENT GETREDIR]
        assert_equal 0 $res
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking off} {
        r CLIENT TRACKING off
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {off} $flags
        set redirect [dict get $res redirect]
        assert_equal {-1} $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking on} {
        r CLIENT TRACKING on
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on} $flags
        set redirect [dict get $res redirect]
        assert_equal {0} $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking on with options} {
        r CLIENT TRACKING on REDIRECT $redir_id noloop
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on noloop} $flags
        set redirect [dict get $res redirect]
        assert_equal $redir_id $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking optin} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on optin
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on optin} $flags
        set redirect [dict get $res redirect]
        assert_equal {0} $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes

        r CLIENT CACHING yes
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on optin caching-yes} $flags
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking optout} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on optout
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on optout} $flags
        set redirect [dict get $res redirect]
        assert_equal {0} $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes

        r CLIENT CACHING no
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on optout caching-no} $flags
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking bcast mode} {
        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST PREFIX foo PREFIX bar
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on bcast} $flags
        set redirect [dict get $res redirect]
        assert_equal {0} $redirect
        set prefixes [lsort [dict get $res prefixes]]
        assert_equal {bar foo} $prefixes

        r CLIENT TRACKING off
        r CLIENT TRACKING on BCAST
        set res [r client trackinginfo]
        set prefixes [dict get $res prefixes]
        assert_equal {{}} $prefixes
    }

    test {CLIENT TRACKINGINFO provides reasonable results when tracking redir broken} {
        clean_all
        r HELLO 3
        r CLIENT TRACKING on REDIRECT $redir_id
        $rd_sg SET key1 1
        r GET key1
        $rd_redirection QUIT
        assert_equal OK [$rd_redirection read]
        $rd_sg SET key1 2
        set res [lsearch -exact [r read] "tracking-redir-broken"]
        assert {$res >= 0}
        set res [r client trackinginfo]
        set flags [dict get $res flags]
        assert_equal {on broken_redirect} $flags
        set redirect [dict get $res redirect]
        assert_equal $redir_id $redirect
        set prefixes [dict get $res prefixes]
        assert_equal {} $prefixes
    }

    $rd_redirection close
    $rd close
}
