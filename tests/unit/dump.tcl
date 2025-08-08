start_server {tags {"dump"}} {
    test {DUMP / RESTORE are able to serialize / unserialize a simple key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        list [r exists foo] [r restore foo 0 $encoded] [r ttl foo] [r get foo]
    } {0 OK -1 bar}

    test {RESTORE can set an arbitrary expire to the materialized key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 5000 $encoded
        set ttl [r pttl foo]
        assert_range $ttl 3000 5000
        r get foo
    } {bar}

    test {RESTORE can set an expire that overflows a 32 bit integer} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 2569591501 $encoded
        set ttl [r pttl foo]
        assert_range $ttl (2569591501-3000) 2569591501
        r get foo
    } {bar}
    
    test {RESTORE can set an absolute expire} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        set now [clock milliseconds]
        r restore foo [expr $now+3000] $encoded absttl
        set ttl [r pttl foo]
        assert_range $ttl 2000 3100
        r get foo
    } {bar}

    test {RESTORE with ABSTTL in the past} {
        r set foo bar
        set encoded [r dump foo]
        set now [clock milliseconds]
        r debug set-active-expire 0
        r restore foo [expr $now-3000] $encoded absttl REPLACE
        catch {r debug object foo} e
        r debug set-active-expire 1
        set e
    } {ERR no such key} {needs:debug}

    test {RESTORE can set LRU} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r config set maxmemory-policy allkeys-lru
        r restore foo 0 $encoded idletime 1000
        set idle [r object idletime foo]
        assert {$idle >= 1000 && $idle <= 1010}
        assert_equal [r get foo] {bar}
        r config set maxmemory-policy noeviction
    } {OK} {needs:config-maxmemory}
    
    test {RESTORE with TTL maintain valid object} {
        # RESTORE Creates a string with TTL in two steps. The second step potentially 
        # reallocates the object. Access the object and verify it is not corrupted
        r del foo
        r set foo bar
        set encoded [r dump foo]
        # Iterate several times and verify it is consistent
        for {set i 0} {$i < 100} {incr i} {
            r del foo
            r restore foo 1000 $encoded IDLETIME 500
            assert_equal [r get foo] {bar}
        }
    }

    test {RESTORE can set LFU} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r config set maxmemory-policy allkeys-lfu
        r restore foo 0 $encoded freq 100

        # We need to determine whether the `object` operation happens within the same minute or crosses into a new one
        # This will help us verify if the freq remains 100 or decays due to a minute transition
        set start [clock format [clock seconds] -format %M]
        set freq [r object freq foo]
        set end [clock format [clock seconds] -format %M]

        if { $start == $end } {
            # If the minutes haven't changed (i.e., the restore and object happened within the same minute),
            # the freq should remain 100 as no decay has occurred yet.
            assert {$freq == 100}
        } else {
            # If the object operation crosses into a new minute, freq may have already decayed by 1 (99),
            # or it may still be 100 if the minute update hasn't been applied yet when the operation is performed.
            # The decay might only take effect after the operation completes and the minute is updated.
            assert {($freq == 100) || ($freq == 99)}
        }

        r get foo
        assert_equal [r get foo] {bar}
        r config set maxmemory-policy noeviction
    } {OK} {needs:config-maxmemory}

    test {RESTORE returns an error of the key already exists} {
        r set foo bar
        set e {}
        catch {r restore foo 0 "..."} e
        set e
    } {*BUSYKEY*}

    test {RESTORE can overwrite an existing key with REPLACE} {
        r set foo bar1
        set encoded1 [r dump foo]
        r set foo bar2
        set encoded2 [r dump foo]
        r del foo
        r restore foo 0 $encoded1
        r restore foo 0 $encoded2 replace
        r get foo
    } {bar2}

    test {RESTORE can detect a syntax error for unrecognized options} {
        catch {r restore foo 0 "..." invalid-option} e
        set e
    } {*syntax*}

    test {RESTORE should not store key that are already expired, with REPLACE will propagate it as DEL or UNLINK} {
        r del key1{t} key2{t}
        r set key1{t} value2
        r lpush key2{t} 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65

        r set key{t} value
        set encoded [r dump key{t}]
        set now [clock milliseconds]

        set repl [attach_to_replication_stream]

        # Keys that have expired will not be stored.
        r config set lazyfree-lazy-server-del no
        assert_equal {OK} [r restore key1{t} [expr $now-5000] $encoded replace absttl]
        r config set lazyfree-lazy-server-del yes
        assert_equal {OK} [r restore key2{t} [expr $now-5000] $encoded replace absttl]
        assert_equal {0} [r exists key1{t} key2{t}]

        # Verify the propagate of DEL and UNLINK.
        assert_replication_stream $repl {
            {select *}
            {del key1{t}}
            {unlink key2{t}}
        }

        close_replication_stream $repl
    } {} {needs:repl}

    test {DUMP of non existing key returns nil} {
        r dump nonexisting_key
    } {}

    test {MIGRATE is caching connections} {
        # Note, we run this as first test so that the connection cache
        # is empty.
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert_match {*migrate_cached_sockets:0*} [r -1 info]
            r -1 migrate $second_host $second_port key 9 1000
            assert_match {*migrate_cached_sockets:1*} [r -1 info]
        }
    } {} {external:skip}

    test {MIGRATE cached connections are released after some time} {
        after 15000
        assert_match {*migrate_cached_sockets:0*} [r info]
    }

    test {MIGRATE is able to migrate a key between two instances} {
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}
            set ret [r -1 migrate $second_host $second_port key 9 5000]
            assert {$ret eq {OK}}
            assert {[$first exists key] == 0}
            assert {[$second exists key] == 1}
            assert {[$second get key] eq {Some Value}}
            assert {[$second ttl key] == -1}
        }
    } {} {external:skip}

    test {MIGRATE is able to copy a key between two instances} {
        set first [srv 0 client]
        r del list
        r lpush list a b c d
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists list] == 1}
            assert {[$second exists list] == 0}
            set ret [r -1 migrate $second_host $second_port list 9 5000 copy]
            assert {$ret eq {OK}}
            assert {[$first exists list] == 1}
            assert {[$second exists list] == 1}
            assert {[$first lrange list 0 -1] eq [$second lrange list 0 -1]}
        }
    } {} {external:skip}

    test {MIGRATE will not overwrite existing keys, unless REPLACE is used} {
        set first [srv 0 client]
        r del list
        r lpush list a b c d
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists list] == 1}
            assert {[$second exists list] == 0}
            $second set list somevalue
            catch {r -1 migrate $second_host $second_port list 9 5000 copy} e
            assert_match {ERR*} $e
            set ret [r -1 migrate $second_host $second_port list 9 5000 copy replace]
            assert {$ret eq {OK}}
            assert {[$first exists list] == 1}
            assert {[$second exists list] == 1}
            assert {[$first lrange list 0 -1] eq [$second lrange list 0 -1]}
        }
    } {} {external:skip}

    test {MIGRATE propagates TTL correctly} {
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}
            $first expire key 10
            set ret [r -1 migrate $second_host $second_port key 9 5000]
            assert {$ret eq {OK}}
            assert {[$first exists key] == 0}
            assert {[$second exists key] == 1}
            assert {[$second get key] eq {Some Value}}
            assert {[$second ttl key] >= 7 && [$second ttl key] <= 10}
        }
    } {} {external:skip}

    test {MIGRATE can correctly transfer large values} {
        set first [srv 0 client]
        r del key
        for {set j 0} {$j < 40000} {incr j} {
            r rpush key 1 2 3 4 5 6 7 8 9 10
            r rpush key "item 1" "item 2" "item 3" "item 4" "item 5" \
                        "item 6" "item 7" "item 8" "item 9" "item 10"
        }
        assert {[string length [r dump key]] > (1024*64)}
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}
            set ret [r -1 migrate $second_host $second_port key 9 10000]
            assert {$ret eq {OK}}
            assert {[$first exists key] == 0}
            assert {[$second exists key] == 1}
            assert {[$second ttl key] == -1}
            assert {[$second llen key] == 40000*20}
        }
    } {} {external:skip}

    test {MIGRATE can correctly transfer hashes} {
        set first [srv 0 client]
        r del key
        r hmset key field1 "item 1" field2 "item 2" field3 "item 3" \
                    field4 "item 4" field5 "item 5" field6 "item 6"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}
            set ret [r -1 migrate $second_host $second_port key 9 10000]
            assert {$ret eq {OK}}
            assert {[$first exists key] == 0}
            assert {[$second exists key] == 1}
            assert {[$second ttl key] == -1}
        }
    } {} {external:skip}

    test {MIGRATE timeout actually works} {
        set first [srv 0 client]
        r set key "Some Value"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key] == 1}
            assert {[$second exists key] == 0}

            set rd [redis_deferring_client]
            $rd debug sleep 1.0 ; # Make second server unable to reply.
            set e {}
            catch {r -1 migrate $second_host $second_port key 9 500} e
            assert_match {IOERR*} $e
        }
    } {} {external:skip}

    test {MIGRATE can migrate multiple keys at once} {
        set first [srv 0 client]
        r set key1 "v1"
        r set key2 "v2"
        r set key3 "v3"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert {[$first exists key1] == 1}
            assert {[$second exists key1] == 0}
            set ret [r -1 migrate $second_host $second_port "" 9 5000 keys key1 key2 key3]
            assert {$ret eq {OK}}
            assert {[$first exists key1] == 0}
            assert {[$first exists key2] == 0}
            assert {[$first exists key3] == 0}
            assert {[$second get key1] eq {v1}}
            assert {[$second get key2] eq {v2}}
            assert {[$second get key3] eq {v3}}
        }
    } {} {external:skip}

    test {MIGRATE with multiple keys must have empty key arg} {
        catch {r MIGRATE 127.0.0.1 6379 NotEmpty 9 5000 keys a b c} e
        set e
    } {*empty string*} {external:skip}

    test {MIGRATE with multiple keys migrate just existing ones} {
        set first [srv 0 client]
        r set key1 "v1"
        r set key2 "v2"
        r set key3 "v3"
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            set ret [r -1 migrate $second_host $second_port "" 9 5000 keys nokey-1 nokey-2 nokey-2]
            assert {$ret eq {NOKEY}}

            assert {[$first exists key1] == 1}
            assert {[$second exists key1] == 0}
            set ret [r -1 migrate $second_host $second_port "" 9 5000 keys nokey-1 key1 nokey-2 key2 nokey-3 key3]
            assert {$ret eq {OK}}
            assert {[$first exists key1] == 0}
            assert {[$first exists key2] == 0}
            assert {[$first exists key3] == 0}
            assert {[$second get key1] eq {v1}}
            assert {[$second get key2] eq {v2}}
            assert {[$second get key3] eq {v3}}
        }
    } {} {external:skip}

    test {MIGRATE with multiple keys: stress command rewriting} {
        set first [srv 0 client]
        r flushdb
        r mset a 1 b 2 c 3 d 4 c 5 e 6 f 7 g 8 h 9 i 10 l 11 m 12 n 13 o 14 p 15 q 16
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            set ret [r -1 migrate $second_host $second_port "" 9 5000 keys a b c d e f g h i l m n o p q]

            assert {[$first dbsize] == 0}
            assert {[$second dbsize] == 15}
        }
    } {} {external:skip}

    test {MIGRATE with multiple keys: delete just ack keys} {
        set first [srv 0 client]
        r flushdb
        r mset a 1 b 2 c 3 d 4 c 5 e 6 f 7 g 8 h 9 i 10 l 11 m 12 n 13 o 14 p 15 q 16
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            $second mset c _ d _; # Two busy keys and no REPLACE used

            catch {r -1 migrate $second_host $second_port "" 9 5000 keys a b c d e f g h i l m n o p q} e

            assert {[$first dbsize] == 2}
            assert {[$second dbsize] == 15}
            assert {[$first exists c] == 1}
            assert {[$first exists d] == 1}
        }
    } {} {external:skip}

    test {MIGRATE AUTH: correct and wrong password cases} {
        set first [srv 0 client]
        r del list
        r lpush list a b c d
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]
            $second config set requirepass foobar
            $second auth foobar

            assert {[$first exists list] == 1}
            assert {[$second exists list] == 0}
            set ret [r -1 migrate $second_host $second_port list 9 5000 AUTH foobar]
            assert {$ret eq {OK}}
            assert {[$second exists list] == 1}
            assert {[$second lrange list 0 -1] eq {d c b a}}

            r -1 lpush list a b c d
            $second config set requirepass foobar2
            catch {r -1 migrate $second_host $second_port list 9 5000 AUTH foobar} err
            assert_match {*WRONGPASS*} $err
        }
    } {} {external:skip}
}
