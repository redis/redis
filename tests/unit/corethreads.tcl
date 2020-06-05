start_server {tags {"corethreads"}} {
    test {Write operations are blocked on locked keys} {
        set rd [redis_deferring_client]
        set now [clock milliseconds]
        $rd DEBUG LOCK 1000 read mykey
        r incr mykey
        set elapsed [expr {[clock milliseconds]-$now}]
        assert {$elapsed > 500}
        $rd read
        $rd close
    } 0

    test {Disconnecting a client with an active thread does not crash Redis} {
        set rd [redis_deferring_client]
        $rd CLIENT ID
        set id [$rd read]
        $rd DEBUG LOCK 5000 read mykey
        assert {[r client kill id $id] == 1}
        $rd close
    }

    test {Disconnecting a client blocked on a key does not crash Redis} {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        $rd1 CLIENT ID
        set id [$rd1 read]
        $rd2 DEBUG LOCK 5000 read mykey
        $rd1 INCR mykey ; # Now this $rd1 is blocked on mykey
        assert {[r client kill id $id] == 1}
        $rd1 close
        $rd2 close
    }

    test {Client is resumed only after last key gets unlocked} {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set now [clock milliseconds]
        $rd1 DEBUG LOCK 500 read mykey1
        $rd2 DEBUG LOCK 2000 read mykey2
        r mset mykey1 a mykey2 b
        set elapsed [expr {[clock milliseconds]-$now}]
        assert {$elapsed > 1500}
        $rd1 close
        $rd2 close
    } 0

    test {Build two large sets for the next tests} {
        for {set j 0} {$j < 10000} {incr j} {
            lappend set1 $j
            lappend set2 [expr {$j*2}]
        }
        r sadd set1 {*}$set1
        r sadd set2 {*}$set2
        assert {[r scard set1] == $j}
        assert {[r scard set2] == $j}
    }

    test {Lua can execute threaded commands synchronously} {
        set items [r EVAL {return redis.call('smembers','set1')} 0]
        llength $items
    } 10000

    test {Lua report an error if we try to access a locked key} {
        set rd [redis_deferring_client]
        $rd DEBUG LOCK 5000 read set1
        catch {
            r EVAL {return redis.call('sadd','set1','foo')} 0
        } err
        $rd close
        set err
    } {*LOCKED*}

    test {MULTI can execute threaded commands synchronously} {
        r MULTI
        r SMEMBERS set1
        set items [r EXEC]
        set items [lindex $items 0]
        llength $items
    } 10000

    test {MULTI can block on locked keys} {
        set rd [redis_deferring_client]
        set now [clock milliseconds]
        $rd DEBUG LOCK 1000 read mykey3
        r multi
        r incr mykey3
        r exec
        set elapsed [expr {[clock milliseconds]-$now}]
        assert {$elapsed > 500}
        $rd read
        $rd close
    }
}
