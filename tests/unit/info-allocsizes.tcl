################################################################################
# Test the "info allocsizes" command.
# The command returns a histogram of the allocation sizes of keys in the database.
#
# Note: The allocsizes histogram requires cluster mode with cluster-slot-stats-enabled
# set on startup (which enables memory_tracking_per_slot).
################################################################################

# Query and Strip result of "info allocsizes" from header, spaces, and newlines.
proc get_info_allocsizes_stripped {server} {
    set infoStripped [string map {
        "# Allocsizes" ""
        " " "" "\n" "" "\r" ""
    } [$server info allocsizes] ]
    return $infoStripped
}

# Verify that allocsizes histogram has entries for the expected types
proc verify_allocsizes_non_empty {server types} {
    set info [$server info allocsizes]
    foreach type $types {
        if {![string match "*distrib_${type}_allocsizes*" $info]} {
            fail "Expected allocsizes for type $type but not found in: $info"
        }
    }
}

# Verify that allocsizes histogram is empty
proc verify_allocsizes_empty {server} {
    set stripped [get_info_allocsizes_stripped $server]
    if {$stripped ne ""} {
        fail "Expected empty allocsizes but got: $stripped"
    }
}

# Test in cluster mode (allocsizes requires cluster mode with cluster-slot-stats-enabled)
start_cluster 1 0 {tags {external:skip cluster needs:debug} overrides {cluster-slot-stats-enabled yes}} {

    test "ALLOCSIZES - Verify info allocsizes section exists" {
        set info [r info allocsizes]
        assert {[string match "*# Allocsizes*" $info]}
    }

    test "ALLOCSIZES - Empty database should have empty allocsizes histogram" {
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - String keys should appear in allocsizes histogram" {
        r FLUSHALL
        r SET "mykey{t}" "hello world"
        verify_allocsizes_non_empty r {strings}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - List keys should appear in allocsizes histogram" {
        r FLUSHALL
        r RPUSH "mylist{t}" a b c d e
        verify_allocsizes_non_empty r {lists}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Set keys should appear in allocsizes histogram" {
        r FLUSHALL
        r SADD "myset{t}" a b c d e
        verify_allocsizes_non_empty r {sets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Zset keys should appear in allocsizes histogram" {
        r FLUSHALL
        r ZADD "myzset{t}" 1 a 2 b 3 c 4 d 5 e
        verify_allocsizes_non_empty r {zsets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Hash keys should appear in allocsizes histogram" {
        r FLUSHALL
        r HSET "myhash{t}" f1 v1 f2 v2 f3 v3
        verify_allocsizes_non_empty r {hashes}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Multiple data types should appear in allocsizes histogram" {
        r FLUSHALL
        r SET "str{t}" "hello"
        r RPUSH "list{t}" a b c
        r SADD "set{t}" x y z
        r ZADD "zset{t}" 1 a 2 b
        r HSET "hash{t}" f1 v1

        verify_allocsizes_non_empty r {strings lists sets zsets hashes}
    }

    test "ALLOCSIZES - Histogram bins should use power-of-2 labels" {
        r FLUSHALL
        # Create a string with known size
        r SET "small{t}" "x"
        set info [r info allocsizes]
        # Check that the output uses the expected exponential labels
        # The labels should be like 32, 64, 128, etc.
        assert {[regexp {distrib_strings_allocsizes:[0-9KM]+=[0-9]+} $info]}
    }

    test "ALLOCSIZES - DEL should remove key from allocsizes histogram" {
        r FLUSHALL
        r SET "k1{t}" "value1"
        verify_allocsizes_non_empty r {strings}
        r DEL "k1{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Overwriting a key should update allocsizes histogram" {
        r FLUSHALL
        # Set a small string
        r SET "mykey{t}" "a"
        set info1 [r info allocsizes]
        # Overwrite with a larger string
        r SET "mykey{t}" [string repeat "x" 10000]
        set info2 [r info allocsizes]
        # The histogram should have changed
        assert {$info1 ne $info2}
    }

    test "ALLOCSIZES - Type change should update allocsizes correctly" {
        r FLUSHALL
        r SET "k1{t}" "string"
        verify_allocsizes_non_empty r {strings}
        r DEL "k1{t}"
        verify_allocsizes_empty r
        r RPUSH "k1{t}" a b c
        verify_allocsizes_non_empty r {lists}
    }

    test "ALLOCSIZES - FLUSHALL clears allocsizes histogram" {
        r SET "k1{t}" "value1"
        r SET "k2{t}" "value2"
        verify_allocsizes_non_empty r {strings}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test List operations (RPUSH, LPUSH, RPOP, LPOP)" {
        r FLUSHALL
        # RPUSH
        r RPUSH "l1{t}" 1 2 3 4 5
        verify_allocsizes_non_empty r {lists}
        # LPUSH another list
        r LPUSH "l2{t}" a b c d e
        verify_allocsizes_non_empty r {lists}
        # RPOP
        r RPOP "l1{t}"
        verify_allocsizes_non_empty r {lists}
        # LPOP
        r LPOP "l2{t}"
        verify_allocsizes_non_empty r {lists}
        # Delete all
        r DEL "l1{t}" "l2{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test Set operations (SADD, SREM, SPOP)" {
        r FLUSHALL
        # SADD
        r SADD "s1{t}" 1 2 3 4 5
        verify_allocsizes_non_empty r {sets}
        # SADD more elements
        r SADD "s1{t}" 6 7 8 9 10
        verify_allocsizes_non_empty r {sets}
        # SREM
        r SREM "s1{t}" 1 2 3
        verify_allocsizes_non_empty r {sets}
        # SPOP until empty
        while {[r SCARD "s1{t}"] > 0} {
            r SPOP "s1{t}"
        }
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test Zset operations (ZADD, ZREM, ZPOPMIN)" {
        r FLUSHALL
        # ZADD
        r ZADD "z1{t}" 1 a 2 b 3 c 4 d 5 e
        verify_allocsizes_non_empty r {zsets}
        # ZADD more elements
        r ZADD "z1{t}" 6 f 7 g 8 h 9 i 10 j
        verify_allocsizes_non_empty r {zsets}
        # ZREM
        r ZREM "z1{t}" a b c
        verify_allocsizes_non_empty r {zsets}
        # ZPOPMIN
        r ZPOPMIN "z1{t}"
        verify_allocsizes_non_empty r {zsets}
        # DEL
        r DEL "z1{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test Hash operations (HSET, HDEL, HINCRBY)" {
        r FLUSHALL
        # HSET
        r HSET "h1{t}" f1 v1 f2 v2 f3 v3
        verify_allocsizes_non_empty r {hashes}
        # HDEL
        r HDEL "h1{t}" f1
        verify_allocsizes_non_empty r {hashes}
        # HINCRBY
        r HINCRBY "h1{t}" counter 10
        verify_allocsizes_non_empty r {hashes}
        # HINCRBYFLOAT
        r HINCRBYFLOAT "h1{t}" floatval 3.14
        verify_allocsizes_non_empty r {hashes}
        # DEL
        r DEL "h1{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test String APPEND grows allocation" {
        r FLUSHALL
        r SET "s1{t}" "hello"
        set info1 [r info allocsizes]
        # Append to significantly grow the string
        for {set i 0} {$i < 20} {incr i} {
            r APPEND "s1{t}" [string repeat "x" 500]
        }
        set info2 [r info allocsizes]
        # Histogram should have changed (larger allocation bin)
        assert {$info1 ne $info2}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test RESTORE adds to histogram" {
        r FLUSHALL
        r RPUSH "mylist{t}" 1 2 3 4
        set encoded [r dump "mylist{t}"]
        r DEL "mylist{t}"
        verify_allocsizes_empty r
        r RESTORE "mylist2{t}" 0 $encoded
        verify_allocsizes_non_empty r {lists}
    }

    test "ALLOCSIZES - Larger allocations go to higher bins" {
        r FLUSHALL
        # Create a small string
        r SET "small{t}" "x"
        set small_info [r info allocsizes]
        r FLUSHALL

        # Create a larger string
        r SET "large{t}" [string repeat "x" 100000]
        set large_info [r info allocsizes]

        # The bin labels should be different
        assert {$small_info ne $large_info}
    }

    test "ALLOCSIZES - EXPIRE eventually removes from histogram" {
        r FLUSHALL
        r SET "expiring{t}" "value"
        verify_allocsizes_non_empty r {strings}
        r PEXPIRE "expiring{t}" 50
        after 100
        # Wait for key to expire
        wait_for_condition 50 20 {
            [get_info_allocsizes_stripped r] eq ""
        } else {
            fail "Key did not expire from allocsizes histogram"
        }
    }

    foreach type {listpackex hashtable} {
        # Test different implementations of hash tables and listpacks
        if {$type eq "hashtable"} {
            r config set hash-max-listpack-entries 0
        } else {
            r config set hash-max-listpack-entries 512
        }

        test "ALLOCSIZES - Test HASH ($type) allocsizes" {
            r FLUSHALL
            r HSET "h1{t}" f1 v1 f2 v2 f3 v3
            verify_allocsizes_non_empty r {hashes}
            r HDEL "h1{t}" f1
            verify_allocsizes_non_empty r {hashes}
            r DEL "h1{t}"
            verify_allocsizes_empty r
        }
    }

    test "ALLOCSIZES - Test DEBUG ALLOCSIZES-HIST-ASSERT command in cluster mode" {
        r DEBUG ALLOCSIZES-HIST-ASSERT 1
        r FLUSHALL
        createComplexDataset r 100 {usetag}
        createComplexDataset r 100 {usetag useexpire usehexpire}
        # If we get here without crash, the assertion passed
        r DEBUG ALLOCSIZES-HIST-ASSERT 0
    }

    test "ALLOCSIZES - DEBUG RELOAD preserves allocsizes histogram" {
        r FLUSHALL
        r SET "str{t}" "hello world"
        r RPUSH "list{t}" 1 2 3 4 5
        verify_allocsizes_non_empty r {strings lists}
        r DEBUG RELOAD
        verify_allocsizes_non_empty r {strings lists}
        r DEL "list{t}"
        r DEBUG RELOAD
        verify_allocsizes_non_empty r {strings}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - RENAME should preserve allocsizes histogram" {
        r FLUSHALL
        r SET "oldkey{t}" "hello world"
        verify_allocsizes_non_empty r {strings}
        r RENAME "oldkey{t}" "newkey{t}"
        verify_allocsizes_non_empty r {strings}
        r DEL "newkey{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Empty string (length 0) should appear in histogram" {
        r FLUSHALL
        r SET "empty{t}" ""
        verify_allocsizes_non_empty r {strings}
        r SET "empty2{t}" ""
        verify_allocsizes_non_empty r {strings}
        r DEL "empty{t}" "empty2{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test String SETBIT operation" {
        r FLUSHALL
        r SETBIT "bits{t}" 71 1
        verify_allocsizes_non_empty r {strings}
        r SETBIT "bits{t}" 640 0
        verify_allocsizes_non_empty r {strings}
        r DEL "bits{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test String BITFIELD operation" {
        r FLUSHALL
        r BITFIELD "bf{t}" SET u8 6 255
        verify_allocsizes_non_empty r {strings}
        r BITFIELD "bf{t}" SET u8 65 255
        verify_allocsizes_non_empty r {strings}
        r DEL "bf{t}"
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test String BITOP operation" {
        r FLUSHALL
        r SET "b1{t}" "x123456789"
        r SET "b2{t}" "x12345678"
        verify_allocsizes_non_empty r {strings}
        r BITOP AND "b3{t}" "b1{t}" "b2{t}"
        verify_allocsizes_non_empty r {strings}
        r BITOP OR "b4{t}" "b1{t}" "b2{t}"
        r BITOP XOR "b5{t}" "b1{t}" "b2{t}"
        verify_allocsizes_non_empty r {strings}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test SDIFFSTORE operation" {
        r FLUSHALL
        r SADD "s1{t}" 1 2 3 4 5 6 7 8
        r SADD "s2{t}" 6 7 8 9 A B C D
        verify_allocsizes_non_empty r {sets}
        r SDIFFSTORE "s3{t}" "s1{t}" "s2{t}"
        verify_allocsizes_non_empty r {sets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test SINTERSTORE operation" {
        r FLUSHALL
        r SADD "s1{t}" 1 2 3 4 5 6 7 8
        r SADD "s2{t}" 6 7 8 9 A B C D
        verify_allocsizes_non_empty r {sets}
        r SINTERSTORE "s3{t}" "s1{t}" "s2{t}"
        verify_allocsizes_non_empty r {sets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test SUNIONSTORE operation" {
        r FLUSHALL
        r SADD "s1{t}" 1 2 3 4 5 6 7 8
        r SADD "s2{t}" 6 7 8 9 A B C D
        verify_allocsizes_non_empty r {sets}
        r SUNIONSTORE "s3{t}" "s1{t}" "s2{t}"
        verify_allocsizes_non_empty r {sets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test ZUNIONSTORE operation" {
        r FLUSHALL
        r ZADD "z1{t}" 1 a 2 b 3 c 4 d 5 e
        r ZADD "z2{t}" 6 f 7 g 8 h 9 i
        verify_allocsizes_non_empty r {zsets}
        r ZUNIONSTORE "z3{t}" 2 "z1{t}" "z2{t}"
        verify_allocsizes_non_empty r {zsets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test ZINTERSTORE operation" {
        r FLUSHALL
        r ZADD "z1{t}" 1 a 2 b 3 c 4 d 5 e
        r ZADD "z2{t}" 3 c 4 d 5 e 6 f
        verify_allocsizes_non_empty r {zsets}
        r ZINTERSTORE "z3{t}" 2 "z1{t}" "z2{t}"
        verify_allocsizes_non_empty r {zsets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test ZDIFFSTORE operation" {
        r FLUSHALL
        r ZADD "z1{t}" 1 a 2 b 3 c 4 d 5 e
        r ZADD "z2{t}" 3 c 4 d 5 e 6 f
        verify_allocsizes_non_empty r {zsets}
        r ZDIFFSTORE "z3{t}" 2 "z1{t}" "z2{t}"
        verify_allocsizes_non_empty r {zsets}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test ZMPOP operation" {
        r FLUSHALL
        r ZADD "z1{t}" 1 a 2 b 3 c
        verify_allocsizes_non_empty r {zsets}
        r ZMPOP 1 "z1{t}" MIN
        verify_allocsizes_non_empty r {zsets}
        r ZMPOP 1 "z1{t}" MAX COUNT 2
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test LMOVE operation" {
        r FLUSHALL
        r RPUSH "l1{t}" 1 2 3 4 5 6 7 8
        verify_allocsizes_non_empty r {lists}
        r LMOVE "l1{t}" "l2{t}" LEFT LEFT
        verify_allocsizes_non_empty r {lists}
        r LMOVE "l1{t}" "l2{t}" RIGHT RIGHT
        verify_allocsizes_non_empty r {lists}
        r FLUSHALL
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - Test LMPOP operation" {
        r FLUSHALL
        r RPUSH "l1{t}" 1 2 3 4 5 6 7 8 9 10
        verify_allocsizes_non_empty r {lists}
        r LMPOP 1 "l1{t}" LEFT COUNT 2
        verify_allocsizes_non_empty r {lists}
        r LMPOP 1 "l1{t}" LEFT COUNT 8
        verify_allocsizes_empty r
    }

    test "ALLOCSIZES - RDB save and restart preserves allocsizes histogram" {
        r FLUSHALL
        r SET "str{t}" "hello world"
        r RPUSH "list{t}" 1 2 3 4 5
        r SADD "set{t}" a b c d e
        r ZADD "zset{t}" 1 a 2 b 3 c
        r HSET "hash{t}" f1 v1 f2 v2
        verify_allocsizes_non_empty r {strings lists sets zsets hashes}
        r SAVE
        restart_server 0 true false
        wait_for_cluster_state ok
        verify_allocsizes_non_empty r {strings lists sets zsets hashes}
    }

    foreach type {listpackex hashtable} {
        if {$type eq "hashtable"} {
            r config set hash-max-listpack-entries 0
        } else {
            r config set hash-max-listpack-entries 512
        }

        test "ALLOCSIZES - Hash field lazy expiration ($type)" {
            r debug set-active-expire 0

            # HGET triggers lazy expiration
            r FLUSHALL
            r HSETEX "h1{t}" PX 1 FIELDS 2 f1 v1 f2 v2
            verify_allocsizes_non_empty r {hashes}
            after 5
            r HGET "h1{t}" f1
            verify_allocsizes_non_empty r {hashes}
            r HGET "h1{t}" f2
            verify_allocsizes_empty r

            # HGETDEL triggers lazy expiration
            r FLUSHALL
            r HSETEX "h1{t}" PX 1 FIELDS 2 f1 v1 f2 v2
            verify_allocsizes_non_empty r {hashes}
            after 5
            r HGETDEL "h1{t}" FIELDS 1 f1
            verify_allocsizes_non_empty r {hashes}
            r HGETDEL "h1{t}" FIELDS 1 f2
            verify_allocsizes_empty r

            # HEXISTS triggers lazy expiration
            r FLUSHALL
            r HSETEX "h1{t}" PX 1 FIELDS 2 f1 v1 f2 v2
            verify_allocsizes_non_empty r {hashes}
            after 5
            r HEXISTS "h1{t}" f1
            verify_allocsizes_non_empty r {hashes}
            r HEXISTS "h1{t}" f2
            verify_allocsizes_empty r

            # HINCRBY triggers lazy expiration
            r FLUSHALL
            r HSETEX "h1{t}" PX 1 FIELDS 1 f1 1
            verify_allocsizes_non_empty r {hashes}
            after 5
            r HINCRBY "h1{t}" f1 1
            verify_allocsizes_non_empty r {hashes}

            # HINCRBYFLOAT triggers lazy expiration
            r FLUSHALL
            r HSETEX "h1{t}" PX 1 FIELDS 1 f1 1
            verify_allocsizes_non_empty r {hashes}
            after 5
            r HINCRBYFLOAT "h1{t}" f1 1.5
            verify_allocsizes_non_empty r {hashes}

            r debug set-active-expire 1
            r FLUSHALL
        }
    }
}

# Test with replication in cluster mode
start_cluster 1 1 {tags {external:skip cluster needs:debug needs:repl} overrides {cluster-slot-stats-enabled yes}} {
    set primary_id 0
    set replica_id 1
    set primary [Rn $primary_id]
    set replica [Rn $replica_id]

    # Wait for replica to sync
    wait_for_condition 50 100 {
        [s -1 role] eq {slave}
    } else {
        fail "Replica did not start"
    }
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Replica link not up"
    }

    test "ALLOCSIZES - Replication updates allocsizes on replica" {
        $primary FLUSHALL
        wait_for_ofs_sync $primary $replica

        # Add data to primary
        $primary SET "str{t}" "hello world"
        $primary RPUSH "list{t}" 1 2 3 4 5
        $primary SADD "set{t}" a b c d e
        $primary ZADD "zset{t}" 1 a 2 b 3 c
        $primary HSET "hash{t}" f1 v1 f2 v2

        # Wait for replication
        wait_for_ofs_sync $primary $replica

        # Verify replica has allocsizes
        verify_allocsizes_non_empty $replica {strings lists sets zsets hashes}
    }

    test "ALLOCSIZES - DEL on primary updates allocsizes on replica" {
        $primary FLUSHALL
        wait_for_ofs_sync $primary $replica

        $primary SET "k1{t}" "value"
        wait_for_ofs_sync $primary $replica
        verify_allocsizes_non_empty $replica {strings}

        $primary DEL "k1{t}"
        wait_for_ofs_sync $primary $replica
        verify_allocsizes_empty $replica
    }

    test "ALLOCSIZES - Complex operations replicate allocsizes correctly" {
        $primary FLUSHALL
        wait_for_ofs_sync $primary $replica

        # Perform various operations on primary
        $primary SET "str1{t}" "value1"
        $primary SET "str2{t}" "value2"
        $primary RPUSH "list1{t}" a b c d e
        $primary SADD "set1{t}" 1 2 3 4 5
        $primary ZADD "zset1{t}" 1 a 2 b 3 c
        $primary HSET "hash1{t}" f1 v1 f2 v2

        # Modify some keys
        $primary APPEND "str1{t}" "_appended"
        $primary RPUSH "list1{t}" f g h
        $primary SADD "set1{t}" 6 7 8
        $primary ZADD "zset1{t}" 4 d 5 e
        $primary HSET "hash1{t}" f3 v3

        # Delete some keys
        $primary DEL "str2{t}"

        wait_for_ofs_sync $primary $replica

        # Verify replica has correct allocsizes
        verify_allocsizes_non_empty $replica {strings lists sets zsets hashes}

        # Cleanup
        $primary FLUSHALL
        wait_for_ofs_sync $primary $replica
        verify_allocsizes_empty $replica
    }
}
