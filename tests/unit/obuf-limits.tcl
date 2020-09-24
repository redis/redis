start_server {tags {"obuf-limits"}} {
    test {Client output buffer hard limit is enforced} {
        r config set client-output-buffer-limit {pubsub 100000 0 0}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set omem 0
        while 1 {
            r publish foo bar
            set clients [split [r client list] "\r\n"]
            set c [split [lindex $clients 1] " "]
            if {![regexp {omem=([0-9]+)} $c - omem]} break
            if {$omem > 200000} break
        }
        assert {$omem >= 70000 && $omem < 200000}
        $rd1 close
    }

    test {Client output buffer soft limit is not enforced if time is not overreached} {
        r config set client-output-buffer-limit {pubsub 0 100000 10}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set omem 0
        set start_time 0
        set time_elapsed 0
        while 1 {
            r publish foo bar
            set clients [split [r client list] "\r\n"]
            set c [split [lindex $clients 1] " "]
            if {![regexp {omem=([0-9]+)} $c - omem]} break
            if {$omem > 100000} {
                if {$start_time == 0} {set start_time [clock seconds]}
                set time_elapsed [expr {[clock seconds]-$start_time}]
                if {$time_elapsed >= 5} break
            }
        }
        assert {$omem >= 100000 && $time_elapsed >= 5 && $time_elapsed <= 10}
        $rd1 close
    }

    test {Client output buffer soft limit is enforced if time is overreached} {
        r config set client-output-buffer-limit {pubsub 0 100000 3}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set omem 0
        set start_time 0
        set time_elapsed 0
        while 1 {
            r publish foo bar
            set clients [split [r client list] "\r\n"]
            set c [split [lindex $clients 1] " "]
            if {![regexp {omem=([0-9]+)} $c - omem]} break
            if {$omem > 100000} {
                if {$start_time == 0} {set start_time [clock seconds]}
                set time_elapsed [expr {[clock seconds]-$start_time}]
                if {$time_elapsed >= 10} break
            }
        }
        assert {$omem >= 100000 && $time_elapsed < 6}
        $rd1 close
    }

    test {No response for single command if client output buffer hard limit is enforced} {
        r config set client-output-buffer-limit {normal 100000 0 0}
        # Total size of all items must be more than 100k
        set item [string repeat "x" 1000]
        for {set i 0} {$i < 150} {incr i} {
            r lpush mylist $item
        }
        set orig_mem [s used_memory]
        # Set client name and get all items
        set rd [redis_deferring_client]
        $rd client setname mybiglist
        assert {[$rd read] eq "OK"}
        $rd lrange mylist 0 -1
        $rd flush
        after 100

        # Before we read reply, redis will close this client.
        set clients [r client list]
        assert_no_match "*name=mybiglist*" $clients
        set cur_mem [s used_memory]
        # 10k just is a deviation threshold
        assert {$cur_mem < 10000 + $orig_mem}

        # Read nothing
        set fd [$rd channel]
        assert_equal {} [read $fd]
    }

    test {No response for multi commands in pipeline if client output buffer limit is enforced} {
        r config set client-output-buffer-limit {normal 100000 0 0}
        set value [string repeat "x" 10000]
        r set bigkey $value
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        # Let redis sleep 2s firstly
        $rd1 debug sleep 2
        $rd1 flush
        after 100

        # Total size should be less than OS socket buffer, redis can
        # execute all commands in this pipeline when it wakes up.
        for {set i 0} {$i < 15} {incr i} {
            $rd2 set $i $i
            $rd2 get $i
            $rd2 del $i
            # One bigkey is 10k, total response size must be more than 100k
            $rd2 get bigkey
        }
        $rd2 flush
        after 100

        # Reds must wake up if it can send reply
        assert_equal "PONG" [r ping]
        set fd [$rd2 channel]
        assert_equal {} [read $fd]
    }

    test {Execute transactions completely even if client output buffer limit is enforced} {
        r config set client-output-buffer-limit {normal 100000 0 0}
        # Total size of all items must be more than 100k
        set item [string repeat "x" 1000]
        for {set i 0} {$i < 150} {incr i} {
            r lpush mylist2 $item
        }

        # Output buffer limit is enforced during executing transaction
        r client setname transactionclient
        r set k1 v1
        r multi
        r set k2 v2
        r get k2
        r lrange mylist2 0 -1
        r set k3 v3
        r del k1
        catch {[r exec]} e
        assert_match "*I/O error*" $e
        set clients [r client list]
        assert_no_match "*name=transactionclient*" $clients
        reconnect

        # Transactions should be executed completely
        assert_equal {} [r get k1]
        assert_equal "v2" [r get k2]
        assert_equal "v3" [r get k3]
    }
}
