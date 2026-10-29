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
        set expiredkeys [s expired_keys]
        r restore foo [expr $now-3000] $encoded absttl REPLACE
        catch {r debug object foo} e
        r debug set-active-expire 1
        # Verify that expired_keys was incremented, even though
        # the key was not added to the DB actually.
        assert_equal [expr $expiredkeys + 1] [s expired_keys]
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

    test {RESTORE fail with invalid payload size} {
        # Payload with mismatched size: claims 0xFFFFFFFFFFFFFFF7 bytes (max uint64 - 8) but provides no data
        # \x00 = String type
        # \x81 = 64-bit length marker
        # \xFF\xFF\xFF\xFF\xFF\xFF\xFF\xF7 = 18446744073709551607 in big-endian
        # \x0c\x00 = RDB version
        # \x00... = fake CRC64
        set encoded "\x00\x81\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xF7\x0c\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        r del test
        catch {r restore test 0 $encoded} e
        set e
    } {*Bad data format*}

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

    test {MIGRATE can correctly transfer template-encoded hashes} {
        set first [srv 0 client]
        r flushdb
        r config set hash-min-template-entries 0

        r himport prepare fs_short  f1 f2 f3
        r himport prepare fs_long f1 f2 [string repeat q 70]
        r himport set lp_short_fields fs_short v1 v2 v3
        r himport set arr_short_fields fs_short v1 [string repeat x 100] v3
        r himport set lp_long_fields fs_long v1 v2 v3
        r himport set arr_long_fields fs_long v1 v2 [string repeat x 100]
        start_server {tags {"repl"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]
            $second config set hash-min-template-entries 0

            set ret [r -1 migrate $second_host $second_port "" 9 10000 keys lp_short_fields arr_short_fields lp_long_fields arr_long_fields]
            assert {$ret eq {OK}}
            assert {[$first exists lp_short_fields] == 0}
            assert {[$first exists arr_short_fields] == 0}
            assert {[$first exists lp_long_fields] == 0}
            assert {[$first exists arr_long_fields] == 0}

            # verify keys are migrated with the correct content and encoding
            assert_equal {f1 v1 f2 v2 f3 v3} [$second hgetall lp_short_fields]
            assert_equal "f1 v1 f2 [string repeat x 100] f3 v3" [$second hgetall arr_short_fields]
            assert_equal "f1 v1 f2 v2 [string repeat q 70] v3" [$second hgetall lp_long_fields]
            assert_equal "f1 v1 f2 v2 [string repeat q 70] [string repeat x 100]" [$second hgetall arr_long_fields]
            assert_equal {template-listpack} [$second object encoding lp_short_fields]
            assert_equal {template-array} [$second object encoding arr_short_fields]
            assert_equal {template-listpack} [$second object encoding lp_long_fields]
            assert_equal {template-array} [$second object encoding arr_long_fields]

            # The two distinct field sets rebuild as two templates on the
            # destination registry, holding four keys in total.
            assert_equal 2 [status $second hash_templates]
            assert_equal 4 [status $second hash_template_keys]

            # Source has no template keys..
            $first himport discardall
            wait_for_condition 50 100 {
                [status $first hash_templates] == 0 &&
                [status $first hash_template_keys] == 0
            } else {
                fail "source registry not drained"
            }
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

# Listpacks store, after each entry, the length of that entry (backlen), so that
# they can be traversed backwards. For an entry whose encoded length is exactly
# 16383, 2097151 or 268435455, Redis 8.8 - 8.10 (#15151) wrote a backlen one
# byte shorter than any other version. Such listpacks are rewritten in the
# historical width when loaded from an RDB version these releases wrote.
set backlen_value [string repeat x 16378]

# Written by 8.6.6 (RDB 13): the historical form, backlen bytes 00 ff ff.
set backlen_payload_wide "\x12\x01\x02\xC3\x40\xD5\x80\x00\x00\x40\x0F\x0B\x0F\x40\x00\x00\x02\x00\xF0\xFA\x3F\x00\x00\x78\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\x00\x00\x09\x00\xFF\xFF\x84\x74\x61\x69\x6C\x05\xFF\x0D\x00\x2B\x90\xEC\xDA\xAF\x4B\xEE\x5A"

# Written by 8.8.2 (RDB 14): the compact form, backlen bytes 7f ff.
set backlen_payload_narrow "\x12\x01\x02\xC3\x40\xD4\x80\x00\x00\x40\x0E\x0B\x0E\x40\x00\x00\x02\x00\xF0\xFA\x3F\x00\x00\x78\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\xFF\x00\xE0\x00\x00\x08\x7F\xFF\x84\x74\x61\x69\x6C\x05\xFF\x0E\x00\xE1\x62\xD1\x9A\x6A\x83\x3A\x64"

# The same as written by 8.10 (RDB 15), without the checksum (all zeroes skips
# the check).
set backlen_payload_narrow_15 [string range $backlen_payload_narrow 0 end-10]
append backlen_payload_narrow_15 "\x0f\x00" [string repeat "\x00" 8]

start_server {tags {"dump"} overrides {list-max-listpack-size -4}} {
    r restore l 0 $backlen_payload_wide
    set backlen_dump_wide [r dump l]

    foreach {version payload} [list 8.6.6 $backlen_payload_wide 8.8.2 $backlen_payload_narrow 8.10 $backlen_payload_narrow_15] {
        foreach sanitize {yes no} {
            test "RESTORE listpack with boundary backlen written by $version (sanitize-dump-payload $sanitize)" {
                r config set sanitize-dump-payload $sanitize
                r del l
                r restore l 0 $payload
                assert_encoding listpack l
                # Forward traversal reads the backlen of the boundary entry to
                # find the next one, backward traversal to find it.
                assert_equal [list $backlen_value tail] [r lrange l 0 -1]
                assert_equal $backlen_value [r lindex l -2]

                # Kept in the historical width, whichever version wrote it.
                assert_equal $backlen_dump_wide [r dump l]

                # Insert and delete after the boundary entry, then reload.
                r rpush l more
                r rpop l
                r debug reload
                assert_equal [list $backlen_value tail] [r lrange l 0 -1]
            } {} {needs:debug}
        }
    }

    test "RESTORE rejects the compact boundary backlen from RDB versions that never wrote it" {
        r config set sanitize-dump-payload yes
        # Claim RDB version 13 and drop the checksum (all zeroes skips the check).
        set payload [string range $backlen_payload_narrow 0 end-10]
        append payload "\x0d\x00" [string repeat "\x00" 8]
        r del l
        assert_error {*Bad data format*} {r restore l 0 $payload}
        assert_equal 0 [r exists l]
    }
}

# RDB files saved with the "lp-backlen-fixed" AUX field hold no compact
# boundary backlen, so their listpacks are loaded without the upgrade above.
#
# Write an RDB file of version 'rdbver' holding the list 'l' of the DUMP
# 'payload', with the "lp-backlen-fixed" AUX field if 'fixed' is set. The
# checksum is all zeroes, which skips its check.
proc write_backlen_rdb {path rdbver payload fixed} {
    set fd [open $path w]
    fconfigure $fd -translation binary
    puts -nonewline $fd [format "REDIS%04d" $rdbver]
    if {$fixed} {
        # RDB_OPCODE_AUX, the field name and its value 1 (int encoded).
        puts -nonewline $fd "\xfa\x10lp-backlen-fixed\xc0\x01"
    }
    # RDB_OPCODE_SELECTDB 9 (the DB of the test client), then the value type,
    # the key and the value.
    puts -nonewline $fd "\xfe\x09"
    puts -nonewline $fd [string index $payload 0]
    puts -nonewline $fd "\x01l"
    puts -nonewline $fd [string range $payload 1 end-10]
    # RDB_OPCODE_EOF and the checksum.
    puts -nonewline $fd "\xff[string repeat "\x00" 8]"
    close $fd
}

start_server {tags {"dump external:skip"} overrides {list-max-listpack-size -4}} {
    set rdb_path [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]

    foreach rdbver {14 15} {
        test "Load RDB $rdbver with the compact boundary backlen and no lp-backlen-fixed AUX field" {
            r config set sanitize-dump-payload yes
            write_backlen_rdb $rdb_path $rdbver $backlen_payload_narrow 0
            r debug reload nosave
            assert_encoding listpack l
            assert_equal [list $backlen_value tail] [r lrange l 0 -1]
            assert_equal $backlen_value [r lindex l -2]
            assert_equal $backlen_dump_wide [r dump l]
        } {} {needs:debug}
    }

    test "SAVE writes the lp-backlen-fixed AUX field" {
        r save
        set fd [open $rdb_path r]
        fconfigure $fd -translation binary
        set content [read $fd]
        close $fd
        assert_morethan_equal [string first "\x10lp-backlen-fixed\xc0\x01" $content] 0

        r debug reload nosave
        assert_equal [list $backlen_value tail] [r lrange l 0 -1]
        assert_equal $backlen_dump_wide [r dump l]
    } {} {needs:debug}
}

start_server {tags {"dump external:skip"} overrides {list-max-listpack-size -4}} {
    test "The lp-backlen-fixed AUX field skips the listpack upgrade" {
        # No version saves this file: the compact backlen is only there to
        # show that the listpack is loaded as is, and fails the check.
        set rdb_path [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
        r config set sanitize-dump-payload yes
        write_backlen_rdb $rdb_path 14 $backlen_payload_narrow 1
        assert_error {*Error trying to load the RDB*} {r debug reload nosave}
        verify_log_message 0 "*Listpack integrity check failed*" 0
    } {} {needs:debug}
}
