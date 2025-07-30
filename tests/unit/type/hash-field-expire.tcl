######## HEXPIRE family commands
# Field does not exists
set E_NO_FIELD     -2
# Specified NX | XX | GT | LT condition not met
set E_FAIL         0
# expiration time set/updated
set E_OK           1
# Field deleted because the specified expiration time is in the past
set E_DELETED      2

######## HTTL family commands
set T_NO_FIELD    -2
set T_NO_EXPIRY   -1

######## HPERIST
set P_NO_FIELD    -2
set P_NO_EXPIRY   -1
set P_OK           1

############################### AUX FUNCS ######################################

proc get_stat_subexpiry {r} {
    set input_string [r info keyspace]
    set hash_count 0

    foreach line [split $input_string \n] {
        if {[regexp {subexpiry=(\d+)} $line -> value]} {
            return $value
        }
    }

    return 0
}

proc get_keys {l} {
    set res {}
    foreach entry $l {
        set key [lindex $entry 0]
        lappend res $key
    }
    return $res
}

proc dumpAllHashes {client} {
    set keyAndFields(0,0) 0
    unset keyAndFields
    # keep keys sorted for comparison
    foreach key [lsort [$client keys *]] {
        set fields [$client hgetall $key]
        foreach f $fields {
            set keyAndFields($key,$f) [$client hpexpiretime $key FIELDS 1 $f]
        }
    }
    return [array get keyAndFields]
}

############################### TESTS #########################################

start_server {tags {"external:skip needs:debug"}} {
    foreach type {listpackex hashtable} {
        if {$type eq "hashtable"} {
            r config set hash-max-listpack-entries 0
        } else {
            r config set hash-max-listpack-entries 512
        }

        test "HEXPIRE/HEXPIREAT/HPEXPIRE/HPEXPIREAT - Returns array if the key does not exist" {
            r del myhash
            assert_equal [r HEXPIRE myhash 1000 FIELDS 1 a] [list $E_NO_FIELD]
            assert_equal [r HEXPIREAT myhash 1000 FIELDS 1 a] [list $E_NO_FIELD]
            assert_equal [r HPEXPIRE myhash 1000 FIELDS 2 a b] [list $E_NO_FIELD $E_NO_FIELD]
            assert_equal [r HPEXPIREAT myhash 1000 FIELDS 2 a b] [list $E_NO_FIELD $E_NO_FIELD]
        }

        test "HEXPIRE/HEXPIREAT/HPEXPIRE/HPEXPIREAT - Verify that the expire time does not overflow" {
            r del myhash
            r hset myhash f1 v1
            # The expire time can't be negative.
            assert_error {ERR invalid expire time, must be >= 0} {r HEXPIRE myhash -1 FIELDS 1 f1}
            assert_error {ERR invalid expire time, must be >= 0} {r HEXPIRE myhash -9223372036854775808 FIELDS 1 f1}
            # The expire time can't be greater than the EB_EXPIRE_TIME_MAX
            assert_error {ERR invalid expire time in 'hexpire' command} {r HEXPIRE myhash [expr (1<<48) / 1000] FIELDS 1 f1}
            assert_error {ERR invalid expire time in 'hexpireat' command} {r HEXPIREAT myhash [expr (1<<48) / 1000 + [clock seconds] + 100] FIELDS 1 f1}
            assert_error {ERR invalid expire time in 'hpexpire' command} {r HPEXPIRE myhash [expr (1<<48)] FIELDS 1 f1}
            assert_error {ERR invalid expire time in 'hpexpireat' command} {r HPEXPIREAT myhash [expr (1<<48) + [clock milliseconds] + 100] FIELDS 1 f1}
        }

        test "HPEXPIRE(AT) - Test 'NX' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpire myhash 1000 NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpire myhash 1000 NX FIELDS 2 field1 field2] [list  $E_FAIL  $E_OK]

            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] NX FIELDS 2 field1 field2] [list  $E_FAIL  $E_OK]
        }

        test "HPEXPIRE(AT) - Test 'XX' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpire myhash 1000 NX FIELDS 2 field1 field2] [list  $E_OK  $E_OK]
            assert_equal [r hpexpire myhash 1000 XX FIELDS 2 field1 field3] [list  $E_OK  $E_FAIL]

            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] NX FIELDS 2 field1 field2] [list  $E_OK  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] XX FIELDS 2 field1 field3] [list  $E_OK  $E_FAIL]
        }

        test "HPEXPIRE(AT) - Test 'GT' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2
            assert_equal [r hpexpire myhash 1000 NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpire myhash 2000 NX FIELDS 1 field2] [list  $E_OK]
            assert_equal [r hpexpire myhash 1500 GT FIELDS 2 field1 field2] [list  $E_OK  $E_FAIL]

            r del myhash
            r hset myhash field1 value1 field2 value2
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+2000)*1000}] NX FIELDS 1 field2] [list  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1500)*1000}] GT FIELDS 2 field1 field2] [list  $E_OK  $E_FAIL]
        }

        test "HPEXPIRE(AT) - Test 'LT' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpire myhash 1000 NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpire myhash 2000 NX FIELDS 1 field2] [list  $E_OK]
            assert_equal [r hpexpire myhash 1500 LT FIELDS 3 field1 field2 field3] [list  $E_FAIL $E_OK $E_OK]

            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1000)*1000}] NX FIELDS 1 field1] [list  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+2000)*1000}] NX FIELDS 1 field2] [list  $E_OK]
            assert_equal [r hpexpireat myhash [expr {([clock seconds]+1500)*1000}] LT FIELDS 3 field1 field2 field3] [list  $E_FAIL $E_OK $E_OK]
        }

        test "HPEXPIREAT - field not exists or TTL is in the past ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2 f4 v4
            r hexpire myhash 1000 NX FIELDS 1 f4
            assert_equal [r hpexpireat myhash [expr {([clock seconds]-1)*1000}] NX FIELDS 4 f1 f2 f3 f4] "$E_DELETED $E_DELETED $E_NO_FIELD $E_FAIL"
            assert_equal [r hexists myhash field1] 0
        }

        test "HPEXPIRE - wrong number of arguments ($type)" {
            r del myhash
            r hset myhash f1 v1
            assert_error {*Parameter `numFields` should be greater than 0} {r hpexpire myhash 1000 NX FIELDS 0 f1 f2 f3}
            # <count> not match with actual number of fields
            assert_error {*parameter must match the number*} {r hpexpire myhash 1000 NX FIELDS 4 f1 f2 f3}
            assert_error {*parameter must match the number*} {r hpexpire myhash 1000 NX FIELDS 2 f1 f2 f3}
        }

        test "HPEXPIRE - parameter expire-time near limit of  2^46 ($type)" {
            r del myhash
            r hset myhash f1 v1
            # below & above
            assert_equal [r hpexpire myhash [expr (1<<46) - [clock milliseconds] - 1000 ] FIELDS 1 f1] [list  $E_OK]
            assert_error {*invalid expire time*} {r hpexpire myhash [expr (1<<46) - [clock milliseconds] + 100 ] FIELDS 1 f1}
        }

        test "Lazy Expire - fields are lazy deleted ($type)" {
            r debug set-active-expire 0
            r del myhash

            r hset myhash f1 v1 f2 v2 f3 v3
            r hpexpire myhash 1 NX FIELDS 3 f1 f2 f3
            after 5

            # Verify that still exists even if all fields are expired
            assert_equal 1 [r EXISTS myhash]

            # Verify that len counts also expired fields
            assert_equal 3 [r HLEN myhash]

            # Trying access to expired field should delete it. Len should be updated
            assert_equal 0 [r hexists myhash f1]
            assert_equal 2 [r HLEN myhash]

            # Trying access another expired field should delete it. Len should be updated
            assert_equal "" [r hget myhash f2]
            assert_equal 1 [r HLEN myhash]

            # Trying access last expired field should delete it. hash shouldn't exists afterward.
            assert_equal 0 [r hstrlen myhash f3]
            assert_equal 0 [r HLEN myhash]
            assert_equal 0 [r EXISTS myhash]

            # Restore default
            r debug set-active-expire 1
        }

        test "Active Expire - deletes hash that all its fields got expired ($type)" {
            r flushall

            set hash_sizes {1 15 16 17 31 32 33 40}
            foreach h $hash_sizes {
                for {set i 1} {$i <= $h} {incr i} {
                    # Random expiration time (Take care expired not after "mix$h")
                    r hset hrand$h f$i v$i
                    r hpexpire hrand$h [expr {70 + int(rand() * 30)}] FIELDS 1 f$i
                    assert_equal 1 [r HEXISTS hrand$h f$i]

                    # Same expiration time (Take care expired not after "mix$h")
                    r hset same$h f$i v$i
                    r hpexpire same$h 100 FIELDS 1 f$i
                    assert_equal 1 [r HEXISTS same$h f$i]

                    # same expiration time
                    r hset mix$h f$i v$i fieldWithoutExpire$i v$i
                    r hpexpire mix$h 100 FIELDS 1 f$i
                    assert_equal 1 [r HEXISTS mix$h f$i]
                }
            }

            # Wait for active expire
            wait_for_condition 50 20 { [r EXISTS same40] == 0 } else { fail "hash `same40` should be expired" }

            # Verify that all fields got expired and keys got deleted
            foreach h $hash_sizes {
                wait_for_condition 50 20 {
                    [r HLEN mix$h] == $h
                } else {
                    fail "volatile fields of hash `mix$h` should be expired"
                }

                for {set i 1} {$i <= $h} {incr i} {
                    assert_equal 0 [r HEXISTS mix$h f$i]
                }
                assert_equal 0 [r EXISTS hrand$h]
                assert_equal 0 [r EXISTS same$h]
            }
        }

        test "HPEXPIRE - Flushall deletes all pending expired fields ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2
            r hpexpire myhash 10000 NX FIELDS 1 field1
            r hpexpire myhash 10000 NX FIELDS 1 field2
            r flushall
            r del myhash
            r hset myhash field1 value1 field2 value2
            r hpexpire myhash 10000 NX FIELDS 1 field1
            r hpexpire myhash 10000 NX FIELDS 1 field2
            r flushall async
        }

        test "HTTL/HPTTL - Returns array if the key does not exist" {
            r del myhash
            assert_equal [r HTTL myhash FIELDS 1 a] [list $T_NO_FIELD]
            assert_equal [r HPTTL myhash FIELDS 2 a b] [list $T_NO_FIELD $T_NO_FIELD]
        }

        test "HTTL/HPTTL - Input validation gets failed on nonexists field or field without expire ($type)" {
            r del myhash
            r HSET myhash field1 value1 field2 value2
            r HPEXPIRE myhash 1000 NX FIELDS 1 field1

            foreach cmd {HTTL HPTTL} {
                assert_equal [r $cmd myhash FIELDS 2 field2 non_exists_field] "$T_NO_EXPIRY $T_NO_FIELD"
                # <count> not match with actual number of fields
                assert_error {*parameter must match the number*} {r $cmd myhash FIELDS 1 non_exists_field1 non_exists_field2}
                assert_error {*parameter must match the number*} {r $cmd myhash FIELDS 3 non_exists_field1 non_exists_field2}
            }
        }

        test "HTTL/HPTTL - returns time to live in seconds/msillisec ($type)" {
            r del myhash
            r HSET myhash field1 value1 field2 value2
            r HPEXPIRE myhash 2000 NX FIELDS 2 field1 field2
            set ttlArray [r HTTL myhash FIELDS 2 field1 field2]
            assert_range [lindex $ttlArray 0] 1 2
            set ttl [r HPTTL myhash FIELDS 1 field1]
            assert_range $ttl 1000 2000
        }

        test "HEXPIRETIME/HPEXPIRETIME - Returns array if the key does not exist" {
            r del myhash
            assert_equal [r HEXPIRETIME myhash FIELDS 1 a] [list $T_NO_FIELD]
            assert_equal [r HPEXPIRETIME myhash FIELDS 2 a b] [list $T_NO_FIELD $T_NO_FIELD]
        }

        test "HEXPIRETIME - returns TTL in Unix timestamp ($type)" {
            r del myhash
            r HSET myhash field1 value1
            set lo [expr {[clock seconds] + 1}]
            set hi [expr {[clock seconds] + 2}]
            r HPEXPIRE myhash 1000 NX FIELDS 1 field1
            assert_range [r HEXPIRETIME myhash FIELDS 1 field1] $lo $hi
            assert_range [r HPEXPIRETIME myhash FIELDS 1 field1] [expr $lo*1000] [expr $hi*1000]
        }

        test "HTTL/HPTTL - Verify TTL progress until expiration ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2
            r hpexpire myhash 1000 NX FIELDS 1 field1
            assert_range [r HPTTL myhash FIELDS 1 field1] 100 1000
            assert_range [r HTTL myhash FIELDS 1 field1] 0 1
            after 100
            assert_range [r HPTTL myhash FIELDS 1 field1] 1 901
            after 910
            assert_equal [r HPTTL myhash FIELDS 1 field1] $T_NO_FIELD
            assert_equal [r HTTL myhash FIELDS 1 field1] $T_NO_FIELD
        }

        test "HPEXPIRE - DEL hash with non expired fields (valgrind test) ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2
            r hpexpire myhash 10000 NX FIELDS 1 field1
            r del myhash
        }

        test "HEXPIREAT - Set time in the past ($type)" {
            r del myhash
            r hset myhash field1 value1
            assert_equal [r hexpireat myhash [expr {[clock seconds] - 1}] NX FIELDS 1 field1] $E_DELETED
            assert_equal [r hexists myhash field1] 0
        }

        test "HEXPIREAT - Set time and then get TTL ($type)" {
            r del myhash
            r hset myhash field1 value1

            r hexpireat myhash [expr {[clock seconds] + 2}] NX FIELDS 1 field1
            assert_range [r hpttl myhash FIELDS 1 field1] 500 2000
            assert_range [r httl myhash FIELDS 1 field1] 1 2

            r hexpireat myhash [expr {[clock seconds] + 5}] XX FIELDS 1 field1
            assert_range [r httl myhash FIELDS 1 field1] 4 5
        }

        test "Lazy Expire - delete hash with expired fields ($type)" {
            r del myhash
            r debug set-active-expire 0
            r hset myhash k v
            r hpexpire myhash 1 NX FIELDS 1 k
            after 5
            r del myhash
            r debug set-active-expire 1
        }

        test "Test HRANDFIELD deletes all expired fields ($type)" {
            r debug set-active-expire 0
            r flushall
            r config resetstat
            r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
            r hpexpire myhash 1 FIELDS 2 f1 f2
            after 5
            assert_equal [lsort [r hrandfield myhash 5]] "f3 f4 f5"
            assert_equal [s expired_subkeys] 2
            r hpexpire myhash 1 FIELDS 3 f3 f4 f5
            after 5
            assert_equal [lsort [r hrandfield myhash 5]] ""
            assert_equal [r keys *] ""

            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3
            r hpexpire myhash 1 FIELDS 1 f1
            after 5
            set res [r hrandfield myhash]
            assert {$res == "f2" || $res == "f3"}
            r hpexpire myhash 1 FIELDS 1 f2
            after 5
            assert_equal [lsort [r hrandfield myhash 5]] "f3"
            r hpexpire myhash 1 FIELDS 1 f3
            after 5
            assert_equal [r hrandfield myhash] ""
            assert_equal [r keys *] ""

            r debug set-active-expire 1
        }

        test "Lazy Expire - HLEN does count expired fields ($type)" {
            # Enforce only lazy expire
            r debug set-active-expire 0

            r del h1 h4 h18 h20
            r hset h1 k1 v1
            r hpexpire h1 1 NX FIELDS 1 k1

            r hset h4 k1 v1 k2 v2 k3 v3 k4 v4
            r hpexpire h4 1 NX FIELDS 3 k1 k3 k4

            # beyond 16 fields: HFE DS (ebuckets) converts from list to rax

            r hset h18 k1 v1 k2 v2 k3 v3 k4 v4 k5 v5 k6 v6 k7 v7 k8 v8 k9 v9 k10 v10 k11 v11 k12 v12 k13 v13 k14 v14 k15 v15 k16 v16 k17 v17 k18 v18
            r hpexpire h18 1 NX FIELDS 18 k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12 k13 k14 k15 k16 k17 k18

            r hset h20 k1 v1 k2 v2 k3 v3 k4 v4 k5 v5 k6 v6 k7 v7 k8 v8 k9 v9 k10 v10 k11 v11 k12 v12 k13 v13 k14 v14 k15 v15 k16 v16 k17 v17 k18 v18 k19 v19 k20 v20
            r hpexpire h20 1 NX FIELDS 2 k1 k2

            after 10

            assert_equal [r hlen h1] 1
            assert_equal [r hlen h4] 4
            assert_equal [r hlen h18] 18
            assert_equal [r hlen h20] 20
            # Restore to support active expire
            r debug set-active-expire 1
        }

        test "Lazy Expire - HSCAN does not report expired fields ($type)" {
            # Enforce only lazy expire
            r debug set-active-expire 0

            r del h1 h20 h4 h18 h20
            r hset h1 01 01
            r hpexpire h1 1 NX FIELDS 1 01

            r hset h4 01 01 02 02 03 03 04 04
            r hpexpire h4 1 NX FIELDS 3 01 03 04

            # beyond 16 fields hash-field expiration DS (ebuckets) converts from list to rax

            r hset h18 01 01 02 02 03 03 04 04 05 05 06 06 07 07 08 08 09 09 10 10 11 11 12 12 13 13 14 14 15 15 16 16 17 17 18 18
            r hpexpire h18 1 NX FIELDS 18 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18

            r hset h20 01 01 02 02 03 03 04 04 05 05 06 06 07 07 08 08 09 09 10 10 11 11 12 12 13 13 14 14 15 15 16 16 17 17 18 18 19 19 20 20
            r hpexpire h20 1 NX FIELDS 2 01 02

            after 10

            # Verify SCAN does not report expired fields
            assert_equal [lsort -unique [lindex [r hscan h1 0 COUNT 10] 1]] ""
            assert_equal [lsort -unique [lindex [r hscan h4 0 COUNT 10] 1]] "02"
            assert_equal [lsort -unique [lindex [r hscan h18 0 COUNT 10] 1]] ""
            assert_equal [lsort -unique [lindex [r hscan h20 0 COUNT 100] 1]] "03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20"
            # Restore to support active expire
            r debug set-active-expire 1
        }

        test "Test HSCAN with mostly expired fields return empty result ($type)" {
            r debug set-active-expire 0

            # Create hash with 1000 fields and 999 of them will be expired
            r del myhash
            for {set i 1} {$i <= 1000} {incr i} {
                r hset myhash field$i value$i
                if {$i > 1} {
                    r hpexpire myhash 1 NX FIELDS 1 field$i
                }
            }
            after 3

            # Verify iterative HSCAN returns either empty result or only the first field
            set countEmptyResult 0
            set cur 0
            while 1 {
                set res [r hscan myhash $cur]
                set cur [lindex $res 0]
                # if the result is not empty, it should contain only the first field
                if {[llength [lindex $res 1]] > 0} {
                    assert_equal [lindex $res 1] "field1 value1"
                } else {
                    incr countEmptyResult
                }
                if {$cur == 0} break
            }
            assert {$countEmptyResult > 0}
            r debug set-active-expire 1
        }

        test "Lazy Expire - verify various HASH commands handling expired fields ($type)" {
            # Enforce only lazy expire
            r debug set-active-expire 0
            r del h1 h2 h3 h4 h5 h18
            r hset h1 01 01
            r hset h2 01 01 02 02
            r hset h3 01 01 02 02 03 03
            r hset h4 1 99 2 99 3 99 4 99
            r hset h5 1 1 2 22 3 333 4 4444 5 55555
            r hset h6 01 01 02 02 03 03 04 04 05 05 06 06
            r hset h18 01 01 02 02 03 03 04 04 05 05 06 06 07 07 08 08 09 09 10 10 11 11 12 12 13 13 14 14 15 15 16 16 17 17 18 18
            r hpexpire h1 1 NX FIELDS 1 01
            r hpexpire h2 1 NX FIELDS 1 01
            r hpexpire h2 1 NX FIELDS 1 02
            r hpexpire h3 1 NX FIELDS 1 01
            r hpexpire h4 1 NX FIELDS 1 2
            r hpexpire h5 1 NX FIELDS 1 3
            r hpexpire h6 1 NX FIELDS 1 05
            r hpexpire h18 1 NX FIELDS 17 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17

            after 5

            # Verify HDEL not ignore expired field. It is too much overhead to check
            # if the field is expired before deletion.
            assert_equal [r HDEL h1 01] "1"

            # Verify HGET ignore expired field
            r config resetstat
            assert_equal [r HGET h2 01] ""
            assert_equal [s expired_subkeys] 1
            assert_equal [r HGET h2 02] ""
            assert_equal [s expired_subkeys] 2
            assert_equal [r HGET h3 01] ""
            assert_equal [r HGET h3 02] "02"
            assert_equal [r HGET h3 03] "03"
            assert_equal [s expired_subkeys] 3
            # Verify HINCRBY ignore expired field
            assert_equal [r HINCRBY h4 2 1] "1"
            assert_equal [s expired_subkeys] 4
            assert_equal [r HINCRBY h4 3 1] "100"
            # Verify HSTRLEN ignore expired field
            assert_equal [r HSTRLEN h5 3] "0"
            assert_equal [s expired_subkeys] 5
            assert_equal [r HSTRLEN h5 4] "4"
            assert_equal [lsort [r HKEYS h6]] "01 02 03 04 06"
            assert_equal [s expired_subkeys] 5
            # Verify HEXISTS ignore expired field
            assert_equal [r HEXISTS h18 07] "0"
            assert_equal [s expired_subkeys] 6
            assert_equal [r HEXISTS h18 18] "1"
            # Verify HVALS ignore expired field
            assert_equal [lsort [r HVALS h18]] "18"
            assert_equal [s expired_subkeys] 6
            # Restore to support active expire
            r debug set-active-expire 1
        }

        test "A field with TTL overridden with another value (TTL discarded) ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            r hpexpire myhash 10000 NX FIELDS 1 field1
            r hpexpire myhash 1 NX FIELDS 1 field2

            # field2 TTL will be discarded
            r hset myhash field2 value4
            after 5
            # Expected TTL will be discarded
            assert_equal [r hget myhash field2] "value4"
            assert_equal [r httl myhash FIELDS 2 field2 field3] "$T_NO_EXPIRY $T_NO_EXPIRY"
            assert_not_equal [r httl myhash FIELDS 1 field1] "$T_NO_EXPIRY"
        }

        test "Modify TTL of a field ($type)" {
            r del myhash
            r hset myhash field1 value1
            r hpexpire myhash 200000 NX FIELDS 1 field1
            r hpexpire myhash 1000000 XX FIELDS 1 field1
            after 15
            assert_equal [r hget myhash field1] "value1"
            assert_range [r hpttl myhash FIELDS 1 field1] 900000 1000000
        }

        test "Test return value of set operation ($type)" {
             r del myhash
             r hset myhash f1 v1 f2 v2
             r hexpire myhash 100000 FIELDS 1 f1
             assert_equal [r hset myhash f2 v2] 0
             assert_equal [r hset myhash f3 v3] 1
             assert_equal [r hset myhash f3 v3 f4 v4] 1
             assert_equal [r hset myhash f3 v3 f5 v5 f6 v6] 2
        }

        test "Test HGETALL not return expired fields ($type)" {
            # Test with small hash
            r debug set-active-expire 0
            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6
            r hpexpire myhash 1 NX FIELDS 3 f2 f4 f6
            after 10
            assert_equal [lsort [r hgetall myhash]] "f1 f3 f5 v1 v3 v5"

            # Test with large hash
            r del myhash
            for {set i 1} {$i <= 600} {incr i} {
                r hset myhash f$i v$i
                if {$i > 3} { r hpexpire myhash 1 NX FIELDS 1 f$i }
            }
            after 10
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 f2 f3 v1 v2 v3"]

            # hash that all fields are expired return empty result
            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6
            r hpexpire myhash 1 FIELDS 6 f1 f2 f3 f4 f5 f6
            after 10
            assert_equal [r hgetall myhash] ""
            r debug set-active-expire 1
        }

        test "Test RENAME hash with fields to be expired ($type)" {
            r debug set-active-expire 0
            r del myhash
            r hset myhash field1 value1
            r hpexpire myhash 20 NX FIELDS 1 field1
            r rename myhash myhash2
            assert_equal [r exists myhash] 0
            assert_range [r hpttl myhash2 FIELDS 1 field1] 1 20
            after 25
            # Verify the renamed key exists
            assert_equal [r exists myhash2] 1
            r debug set-active-expire 1
            # Only active expire will delete the key
            wait_for_condition 30 10 { [r exists myhash2] == 0 } else { fail "`myhash2` should be expired" }
        }

        test "Test RENAME hash that had HFEs but not during the rename ($type)" {
            r del h1
            r hset h1 f1 v1 f2 v2
            r hpexpire h1 1 FIELDS 1 f1
            after 20
            r rename h1 h1_renamed
            assert_equal [r exists h1] 0
            assert_equal [r exists h1_renamed] 1
            assert_equal [r hgetall h1_renamed] {f2 v2}
            r hpexpire h1_renamed 1 FIELDS 1 f2
            # Only active expire will delete the key
            wait_for_condition 30 10 { [r exists h1_renamed] == 0 } else { fail "`h1_renamed` should be expired" }
        }

        test "MOVE to another DB hash with fields to be expired ($type)" {
            r select 9
            r flushall
            r hset myhash field1 value1
            r expireat myhash 2000000000000 ;# Force kvobj reallocation during move command
            r hpexpire myhash 100 NX FIELDS 1 field1
            r move myhash 10
            assert_equal [r exists myhash] 0
            assert_equal [r dbsize] 0

            # Verify the key and its field exists in the target DB
            r select 10
            assert_equal [r hget myhash field1] "value1"
            assert_equal [r exists myhash] 1

            # Eventually the field will be expired and the key will be deleted
            wait_for_condition 40 10 { [r hget myhash field1] == "" } else { fail "`field1` should be expired" }
            wait_for_condition 40 10 { [r exists myhash] == 0 } else { fail "db should be empty" }
        } {} {singledb:skip}

        test "Test COPY hash with fields to be expired ($type)" {
            r flushall
            r hset h1 f1 v1 f2 v2
            r hset h2 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6 f7 v7 f8 v8 f9 v9 f10 v10 f11 v11 f12 v12 f13 v13 f14 v14 f15 v15 f16 v16 f17 v17 f18 v18
            r hpexpire h1 100 NX FIELDS 1 f1
            r hpexpire h2 100 NX FIELDS 18 f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 f13 f14 f15 f16 f17 f18
            r COPY h1 h1copy
            r COPY h2 h2copy
            assert_equal [r hget h1 f1] "v1"
            assert_equal [r hget h1copy f1] "v1"
            assert_equal [r exists h2] 1
            assert_equal [r exists h2copy] 1
            after 105

            # Verify lazy expire of field in h1 and its copy
            assert_equal [r hget h1 f1] ""
            assert_equal [r hget h1copy f1] ""

            # Verify lazy expire of field in h2 and its copy. Verify the key deleted as well.
            wait_for_condition 40 10 { [r exists h2] == 0 } else { fail "`h2` should be expired" }
            wait_for_condition 40 10 { [r exists h2copy] == 0 } else { fail "`h2copy` should be expired" }

        } {} {singledb:skip}

        test "Test COPY hash that had HFEs but not during the copy ($type)" {
            r del h1
            r hset h1 f1 v1 f2 v2
            r hpexpire h1 1 FIELDS 1 f1
            after 20
            r COPY h1 h1_copy
            assert_equal [r exists h1] 1
            assert_equal [r exists h1_copy] 1
            assert_equal [r hgetall h1_copy] {f2 v2}
            r hpexpire h1_copy 1 FIELDS 1 f2
            # Only active expire will delete the key
            wait_for_condition 30 10 { [r exists h1_copy] == 0 } else { fail "`h1_copy` should be expired" }
        }

        test "Test SWAPDB hash-fields to be expired ($type)" {
            r select 9
            r flushall
            r hset myhash field1 value1
            r hpexpire myhash 50 NX FIELDS 1 field1

            r swapdb 9 10

            # Verify the key and its field doesn't exist in the source DB
            assert_equal [r exists myhash] 0
            assert_equal [r dbsize] 0

            # Verify the key and its field exists in the target DB
            r select 10
            assert_equal [r hget myhash field1] "value1"
            assert_equal [r dbsize] 1

            # Eventually the field will be expired and the key will be deleted
            wait_for_condition 20 10 { [r exists myhash] == 0 } else { fail "'myhash' should be expired" }
        } {} {singledb:skip}

        test "Test SWAPDB hash that had HFEs but not during the swap ($type)" {
            r select 9
            r flushall
            r hset myhash f1 v1 f2 v2
            r hpexpire myhash 1 NX FIELDS 1 f1
            after 10

            r swapdb 9 10

            # Verify the key and its field doesn't exist in the source DB
            assert_equal [r exists myhash] 0
            assert_equal [r dbsize] 0

            # Verify the key and its field exists in the target DB
            r select 10
            assert_equal [r hgetall myhash] {f2 v2}
            assert_equal [r dbsize] 1
            r hpexpire myhash 1 NX FIELDS 1 f2

            # Eventually the field will be expired and the key will be deleted
            wait_for_condition 20 10 { [r exists myhash] == 0 } else { fail "'myhash' should be expired" }
        } {} {singledb:skip}

        test "HMGET - returns empty entries if fields or hash expired ($type)" {
            r debug set-active-expire 0
            r del h1 h2
            r hset h1 f1 v1 f2 v2 f3 v3
            r hset h2 f1 v1 f2 v2 f3 v3
            r hpexpire h1 10000000 NX FIELDS 1 f1
            r hpexpire h1 1 NX FIELDS 2 f2 f3
            r hpexpire h2 1 NX FIELDS 3 f1 f2 f3
            after 5
            assert_equal [r hmget h1 f1 f2 f3] {v1 {} {}}
            assert_equal [r hmget h2 f1 f2 f3] {{} {} {}}
            r debug set-active-expire 1
        }

        test "HPERSIST - Returns array if the key does not exist ($type)" {
            r del myhash
            assert_equal [r HPERSIST myhash FIELDS 1 a] [list $P_NO_FIELD]
            assert_equal [r HPERSIST myhash FIELDS 2 a b] [list $P_NO_FIELD $P_NO_FIELD]
        }

        test "HPERSIST - input validation ($type)" {
            # HPERSIST key <num-fields> <field [field ...]>
            r del myhash
            r hset myhash f1 v1 f2 v2
            r hexpire myhash 1000 NX FIELDS 1 f1
            assert_error {*wrong number of arguments*} {r hpersist myhash}
            assert_error {*wrong number of arguments*} {r hpersist myhash FIELDS 1}
            assert_equal [r hpersist myhash FIELDS 2 f1 not-exists-field] "$P_OK $P_NO_FIELD"
            assert_equal [r hpersist myhash FIELDS 1 f2] "$P_NO_EXPIRY"
            # <count> not match with actual number of fields
            assert_error {*parameter must match the number*} {r hpersist myhash FIELDS 2 f1 f2 f3}
            assert_error {*parameter must match the number*} {r hpersist myhash FIELDS 4 f1 f2 f3}
        }

        test "HPERSIST - verify fields with TTL are persisted ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2
            r hexpire myhash 20 NX FIELDS 2 f1 f2
            r hpersist myhash FIELDS 2 f1 f2
            after 25
            assert_equal [r hget myhash f1] "v1"
            assert_equal [r hget myhash f2] "v2"
            assert_equal [r HTTL myhash FIELDS 2 f1 f2] "$T_NO_EXPIRY $T_NO_EXPIRY"
        }

        test "HTTL/HPERSIST - Test expiry commands with non-volatile hash ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r httl myhash FIELDS 1 field1] $T_NO_EXPIRY
            assert_equal [r httl myhash FIELDS 1 fieldnonexist] $E_NO_FIELD

            assert_equal [r hpersist myhash FIELDS 1 field1] $P_NO_EXPIRY
            assert_equal [r hpersist myhash FIELDS 1 fieldnonexist] $P_NO_FIELD
        }

        test {DUMP / RESTORE are able to serialize / unserialize a hash} {
            r config set sanitize-dump-payload yes
            r del myhash
            r hmset myhash a 1 b 2 c 3
            r hexpireat myhash 2524600800 fields 1 a
            r hexpireat myhash 2524600801 fields 1 b
            set encoded [r dump myhash]
            r del myhash
            r restore myhash 0 $encoded
            assert_equal [lsort [r hgetall myhash]] "1 2 3 a b c"
            assert_equal [r hexpiretime myhash FIELDS 3 a b c] {2524600800 2524600801 -1}
        }

        test {RESTORE hash that had in the past HFEs but not during the dump} {
            r config set sanitize-dump-payload yes
            r del myhash
            r hmset myhash a 1 b 2 c 3
            r hpexpire myhash 1 fields 1 a
            after 10
            set encoded [r dump myhash]
            r del myhash
            r restore myhash 0 $encoded
            assert_equal [lsort [r hgetall myhash]] "2 3 b c"
            r hpexpire myhash 1 fields 2 b c
            wait_for_condition 30 10 { [r exists myhash] == 0 } else { fail "`myhash` should be expired" }
        }

        test {DUMP / RESTORE are able to serialize / unserialize a hash with TTL 0 for all fields} {
            r config set sanitize-dump-payload yes
            r del myhash
            r hmset myhash a 1 b 2 c 3
            r hexpire myhash 9999999 fields 1 a ;# make all TTLs of fields to 0
            r hpersist myhash fields 1 a
            assert_encoding $type myhash
            set encoded [r dump myhash]
            r del myhash
            r restore myhash 0 $encoded
            assert_equal [lsort [r hgetall myhash]] "1 2 3 a b c"
            assert_equal [r hexpiretime myhash FIELDS 3 a b c] {-1 -1 -1}
        }

        test {HINCRBY - discards pending expired field and reset its value} {
            r debug set-active-expire 0
            r del h1 h2
            r hset h1 f1 10 f2 2
            r hset h2 f1 10
            assert_equal [r HINCRBY h1 f1 2] 12
            assert_equal [r HINCRBY h2 f1 2] 12
            r HPEXPIRE h1 10 FIELDS 1 f1
            r HPEXPIRE h2 10 FIELDS 1 f1
            after 15
            assert_equal [r HINCRBY h1 f1 1] 1
            assert_equal [r HINCRBY h2 f1 1] 1
            r debug set-active-expire 1
        }

        test {HINCRBY - preserve expiration time of the field} {
            r del h1
            r hset h1 f1 10
            r hpexpire h1 20 FIELDS 1 f1
            assert_equal [r HINCRBY h1 f1 2] 12
            assert_range [r HPTTL h1 FIELDS 1 f1] 1 20
        }


        test {HINCRBYFLOAT - discards pending expired field and reset its value} {
            r debug set-active-expire 0
            r del h1 h2
            r hset h1 f1 10 f2 2
            r hset h2 f1 10
            assert_equal [r HINCRBYFLOAT h1 f1 2] 12
            assert_equal [r HINCRBYFLOAT h2 f1 2] 12
            r HPEXPIRE h1 10 FIELDS 1 f1
            r HPEXPIRE h2 10 FIELDS 1 f1
            after 15
            assert_equal [r HINCRBYFLOAT h1 f1 1] 1
            assert_equal [r HINCRBYFLOAT h2 f1 1] 1
            r debug set-active-expire 1
        }

        test {HINCRBYFLOAT - preserve expiration time of the field} {
            r del h1
            r hset h1 f1 10
            r hpexpire h1 20 FIELDS 1 f1
            assert_equal [r HINCRBYFLOAT h1 f1 2.5] 12.5
            assert_range [r HPTTL h1 FIELDS 1 f1] 1 20
        }

        test "HGETDEL - delete field with ttl ($type)" {
            r debug set-active-expire 0
            r del h1

            # Test deleting only field in a hash. Due to lazy expiry,
            # reply will be null and the field and the key will be deleted.
            r hsetex h1 PX 5 FIELDS 1 f1 10
            after 15
            assert_equal [r hgetdel h1 fields 1 f1] "{}"
            assert_equal [r exists h1]  0

            # Test deleting one field among many. f2 will lazily expire
            r hsetex h1 FIELDS 3 f1 10 f2 20 f3 value3
            r hpexpire h1 5 FIELDS 1 f2
            after 15
            assert_equal [r hgetdel h1 fields 2 f2 f3] "{} value3"
            assert_equal [lsort [r hgetall h1]] [lsort "f1 10"]

            # Try to delete the last field, along with non-existing fields
            assert_equal [r hgetdel h1 fields 4 f1 f2 f3 f4] "10 {} {} {}"
            r debug set-active-expire 1
        }

        test "HGETEX - input validation ($type)" {
            r del h1
            assert_error "*wrong number of arguments*" {r HGETEX}
            assert_error "*wrong number of arguments*" {r HGETEX h1}
            assert_error "*wrong number of arguments*" {r HGETEX h1 FIELDS}
            assert_error "*wrong number of arguments*" {r HGETEX h1 FIELDS 0}
            assert_error "*wrong number of arguments*" {r HGETEX h1 FIELDS 1}
            assert_error "*argument FIELDS is missing*" {r HGETEX h1 XFIELDX 1 a}
            assert_error "*argument FIELDS is missing*" {r HGETEX h1 PXAT 1 1}
            assert_error "*argument FIELDS is missing*" {r HGETEX h1 PERSIST 1 FIELDS 1 a}
            assert_error "*must match the number of arguments*" {r HGETEX h1 FIELDS 2 a}
            assert_error "*Number of fields must be a positive integer*" {r HGETEX h1 FIELDS 0 a}
            assert_error "*Number of fields must be a positive integer*" {r HGETEX h1 FIELDS -1 a}
            assert_error "*Number of fields must be a positive integer*" {r HGETEX h1 FIELDS 9223372036854775808 a}
        }

        test "HGETEX - input validation (expire time) ($type)" {
            assert_error "*value is not an integer or out of range*" {r HGETEX h1 EX bla FIELDS 1 a}
            assert_error "*value is not an integer or out of range*" {r HGETEX h1 EX 9223372036854775808 FIELDS 1 a}
            assert_error "*value is not an integer or out of range*" {r HGETEX h1 EXAT 9223372036854775808 FIELDS 1 a}
            assert_error "*invalid expire time, must be >= 0*" {r HGETEX h1 PX -1 FIELDS 1 a}
            assert_error "*invalid expire time, must be >= 0*" {r HGETEX h1 PXAT -1 FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 EX -1 FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 EX [expr (1<<48)] FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 EX [expr (1<<46) - [clock seconds] + 100 ] FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 EXAT [expr (1<<46) + 100 ] FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 PX [expr (1<<46) - [clock milliseconds] + 100 ] FIELDS 1 a}
            assert_error "*invalid expire time*" {r HGETEX h1 PXAT [expr (1<<46) + 100 ] FIELDS 1 a}
        }

        test "HGETEX - get without setting ttl ($type)" {
            r del h1
            r hset h1 a 1 b 2 c strval
            assert_equal [r hgetex h1 fields 1 a] "1"
            assert_equal [r hgetex h1 fields 2 a b] "1 2"
            assert_equal [r hgetex h1 fields 3 a b c] "1 2 strval"
            assert_equal [r HTTL h1 FIELDS 3 a b c] "$T_NO_EXPIRY $T_NO_EXPIRY $T_NO_EXPIRY"
        }

        test "HGETEX - get and set the ttl ($type)" {
            r del h1
            r hset h1 a 1 b 2 c strval
            assert_equal [r hgetex h1 EX 10000 fields 1 a] "1"
            assert_range [r HTTL h1 FIELDS 1 a] 9000 10000
            assert_equal [r hgetex h1 EX 10000 fields 1 c] "strval"
            assert_range [r HTTL h1 FIELDS 1 c] 9000 10000
        }

        test "HGETEX - Test 'EX' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hgetex myhash EX 1000 FIELDS 1 field1] [list "value1"]
            assert_range [r httl myhash FIELDS 1 field1] 1 1000
        }

        test "HGETEX - Test 'EXAT' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hgetex myhash EXAT [expr [clock seconds] + 10] FIELDS 1 field2] [list "value2"]
            assert_range [r httl myhash FIELDS 1 field2] 5 10
        }

        test "HGETEX - Test 'PX' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hgetex myhash PX 1000000 FIELDS 1 field3] [list "value3"]
            assert_range [r httl myhash FIELDS 1 field3] 900 1000
        }

        test "HGETEX - Test 'PXAT' flag ($type)" {
            r del myhash
            r hset myhash field1 value1 field2 value2 field3 value3
            assert_equal [r hgetex myhash PXAT [expr [clock milliseconds] + 10000] FIELDS 1 field3] [list "value3"]
            assert_range [r httl myhash FIELDS 1 field3] 5 10
        }

        test "HGETEX - Test 'PERSIST' flag ($type)" {
            r del myhash
            r debug set-active-expire 0

            r hsetex myhash PX 5000 FIELDS 3 f1 v1 f2 v2 f3 v3
            assert_not_equal [r httl myhash FIELDS 1 f1] "$T_NO_EXPIRY"
            assert_not_equal [r httl myhash FIELDS 1 f2] "$T_NO_EXPIRY"
            assert_not_equal [r httl myhash FIELDS 1 f3] "$T_NO_EXPIRY"

            # Persist f1 and verify it does not have TTL anymore
            assert_equal [r hgetex myhash PERSIST FIELDS 1 f1] "v1"
            assert_equal [r httl myhash FIELDS 1 f1] "$T_NO_EXPIRY"

            # Persist rest of the fields
            assert_equal [r hgetex myhash PERSIST FIELDS 2 f2 f3] "v2 v3"
            assert_equal [r httl myhash FIELDS 2 f2 f3]  "$T_NO_EXPIRY $T_NO_EXPIRY"

            # Redo the operation. It should be noop as fields are persisted already.
            assert_equal [r hgetex myhash PERSIST FIELDS 2 f2 f3] "v2 v3"
            assert_equal [r httl myhash FIELDS 2 f2 f3]  "$T_NO_EXPIRY $T_NO_EXPIRY"

            # Final sanity, fields exist and have no attached ttl.
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2 f3 v3"]
            assert_equal [r httl myhash FIELDS 3 f1 f2 f3]  "$T_NO_EXPIRY $T_NO_EXPIRY $T_NO_EXPIRY"
            r debug set-active-expire 1
        }

        test "HGETEX - Test setting ttl in the past will delete the key ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3

            # hgetex without setting ttl
            assert_equal [lsort [r hgetex myhash fields 3 f1 f2 f3]] [lsort "v1 v2 v3"]
            assert_equal [r httl myhash FIELDS 3 f1 f2 f3] "$T_NO_EXPIRY $T_NO_EXPIRY $T_NO_EXPIRY"

            # set an expired ttl and verify the key is deleted
            r hgetex myhash PXAT 1 fields 3 f1 f2 f3
            assert_equal [r exists myhash] 0
        }

        test "HGETEX - Test active expiry ($type)" {
            r del myhash
            r debug set-active-expire 0

            r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
            assert_equal [lsort [r hgetex myhash PXAT 1 FIELDS 5 f1 f2 f3 f4 f5]] [lsort "v1 v2 v3 v4 v5"]

            r debug set-active-expire 1
            wait_for_condition 50 20 { [r EXISTS myhash] == 0 } else { fail "'myhash' should be expired" }
        }

        test "HGETEX - A field with TTL overridden with another value (TTL discarded) ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3
            r hgetex myhash PX 10000 FIELDS 1 f1
            r hgetex myhash EX 100 FIELDS 1 f2

            # f2 ttl will be discarded
            r hset myhash f2 v22
            assert_equal [r hget myhash f2] "v22"
            assert_equal [r httl myhash FIELDS 2 f2 f3] "$T_NO_EXPIRY $T_NO_EXPIRY"

            # Other field is not affected (still has TTL)
            assert_not_equal [r httl myhash FIELDS 1 f1] "$T_NO_EXPIRY"
        }

        test "HGETEX - Test with lazy expiry ($type)" {
            r del myhash
            r debug set-active-expire 0

            r hsetex myhash PX 1 FIELDS 2 f1 v1 f2 v2
            after 5
            assert_equal [r hgetex myhash FIELDS 2 f1 f2] "{} {}"
            assert_equal [r exists myhash] 0

            r debug set-active-expire 1
        }

        test "HSETEX - input validation ($type)" {
            assert_error {*wrong number of arguments*} {r hsetex myhash}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 1}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 2 a b}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 2 a b c}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 2 a b c d e}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 3 a b c d}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 3 a b c d e}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 3 a b c d e f g}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 3 a b}
            assert_error {*wrong number of arguments*} {r hsetex myhash fields 1 a b unknown}
            assert_error {*unknown argument*} {r hsetex myhash nx fields 1 a b}
            assert_error {*unknown argument*} {r hsetex myhash 1 fields 1 a b}

            # Only one of FNX or FXX
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fxx fxx EX 100 fields 1 a b}
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fxx fnx EX 100 fields 1 a b}
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fnx fxx EX 100 fields 1 a b}
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fnx fnx EX 100 fields 1 a b}
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fxx fnx fxx EX 100 fields 1 a b}
            assert_error {*Only one of FXX or FNX arguments *} {r hsetex myhash fnx fxx fnx EX 100 fields 1 a b}

            # Only one of EX, PX, EXAT, PXAT or KEEPTTL can be specified
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EX 100 PX 1000 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EX 100 EXAT 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EXAT 100 EX 1000 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EXAT 100 PX 1000 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PX 100 EXAT 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PX 100 PXAT 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PXAT 100 EX 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PXAT 100 EXAT 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EX 100 KEEPTTL fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash KEEPTTL EX 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EX 100 EX 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash EXAT 100 EXAT 100 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PX 10 PX 10 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash PXAT 10 PXAT 10 fields 1 a b}
            assert_error {*Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments*} {r hsetex myhash KEEPTTL KEEPTTL fields 1 a b}

            # missing expire time
            assert_error {*not an integer or out of range*} {r hsetex myhash ex fields 1 a b}
            assert_error {*not an integer or out of range*} {r hsetex myhash px fields 1 a b}
            assert_error {*not an integer or out of range*} {r hsetex myhash exat fields 1 a b}
            assert_error {*not an integer or out of range*} {r hsetex myhash pxat fields 1 a b}

            # expire time more than 2 ^ 48
            assert_error {*invalid expire time*} {r hsetex myhash EXAT [expr (1<<48)] 1 a b}
            assert_error {*invalid expire time*} {r hsetex myhash PXAT [expr (1<<48)] 1 a b}
            assert_error {*invalid expire time*} {r hsetex myhash EX [expr (1<<48) - [clock seconds] + 1000 ] 1 a b}
            assert_error {*invalid expire time*} {r hsetex myhash PX [expr (1<<48) - [clock milliseconds] + 1000 ] 1 a b}

            # invalid expire time
            assert_error {*invalid expire time*} {r hsetex myhash EXAT -1 1 a b}
            assert_error {*not an integer or out of range*} {r hsetex myhash EXAT 9223372036854775808 1 a b}
            assert_error {*not an integer or out of range*} {r hsetex myhash EXAT x 1 a b}

            # invalid numfields arg
            assert_error {*invalid number of fields*} {r hsetex myhash fields x a b}
            assert_error {*invalid number of fields*} {r hsetex myhash fields 9223372036854775808 a b}
            assert_error {*invalid number of fields*} {r hsetex myhash fields 0 a b}
            assert_error {*invalid number of fields*} {r hsetex myhash fields -1 a b}
        }
        
        test "HSETEX - Basic test ($type)" {
            r del myhash

            # set field
            assert_equal [r hsetex myhash FIELDS 1 f1 v1] 1
            assert_equal [r hget myhash f1] "v1"

            # override
            assert_equal [r hsetex myhash FIELDS 1 f1 v11] 1
            assert_equal [r hget myhash f1] "v11"

            # set multiple
            assert_equal [r hsetex myhash FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2"]
            assert_equal [r hsetex myhash FIELDS 3 f1 v111 f2 v222 f3 v333] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v111 f2 v222 f3 v333"]
        }

        test "HSETEX - Test FXX flag ($type)" {
            r del myhash

            # Key is empty, command fails due to FXX
            assert_equal [r hsetex myhash FXX FIELDS 2 f1 v1 f2 v2] 0
            # Verify it did not leave the key empty
            assert_equal [r exists myhash] 0

            # Command fails and no change on fields
            r hset myhash f1 v1
            assert_equal [r hsetex myhash FXX FIELDS 2 f1 v1 f2 v2] 0
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1"]

            # Command executed successfully
            assert_equal [r hsetex myhash FXX FIELDS 1 f1 v11] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v11"]

            # Try with multiple fields
            r hset myhash f2 v2
            assert_equal [r hsetex myhash FXX FIELDS 2 f1 v111 f2 v222] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v111 f2 v222"]

            # Try with expiry
            assert_equal [r hsetex myhash FXX EX 100 FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2"]
            assert_range [r httl myhash FIELDS 1 f1] 80 100
            assert_range [r httl myhash FIELDS 1 f2] 80 100

            # Try with expiry, FXX arg comes after TTL
            assert_equal [r hsetex myhash PX 5000 FXX FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2"]
            assert_range [r hpttl myhash FIELDS 1 f1] 4500 5000
            assert_range [r hpttl myhash FIELDS 1 f2] 4500 5000
        }

        test "HSETEX - Test FXX flag with lazy expire ($type)" {
            r del myhash
            r debug set-active-expire 0

            r hsetex myhash PX 10 FIELDS 1 f1 v1
            after 15
            assert_equal [r hsetex myhash FXX FIELDS 1 f1 v11] 0
            assert_equal [r exists myhash] 0
            r debug set-active-expire 1
        }

        test "HSETEX - Test FNX flag ($type)" {
            r del myhash

            # Command successful on an empty key
            assert_equal [r hsetex myhash FNX FIELDS 1 f1 v1] 1

            # Command fails and no change on fields
            assert_equal [r hsetex myhash FNX FIELDS 2 f1 v1 f2 v2] 0
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1"]

            # Command executed successfully
            assert_equal [r hsetex myhash FNX FIELDS 2 f2 v2 f3 v3] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2 f3 v3"]
            assert_equal [r hsetex myhash FXX FIELDS 3 f1 v11 f2 v22 f3 v33] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v11 f2 v22 f3 v33"]

            # Try with expiry
            r del myhash
            assert_equal [r hsetex myhash FNX EX 100 FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2"]
            assert_range [r httl myhash FIELDS 1 f1] 80 100
            assert_range [r httl myhash FIELDS 1 f2] 80 100

            # Try with expiry, FNX arg comes after TTL
            assert_equal [r hsetex myhash PX 5000 FNX FIELDS 1 f3 v3] 1
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v1 f2 v2 f3 v3"]
            assert_range [r hpttl myhash FIELDS 1 f3] 4500 5000
        }

        test "HSETEX - Test 'EX' flag ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2
            assert_equal [r hsetex myhash EX 1000 FIELDS 1 f3 v3 ] 1
            assert_range [r httl myhash FIELDS 1 f3] 900 1000
        }

        test "HSETEX - Test 'EXAT' flag ($type)" {
            r del myhash
            r hset myhash f1 v1 f2 v2
            assert_equal [r hsetex myhash EXAT [expr [clock seconds] + 10] FIELDS 1 f3 v3] 1
            assert_range [r httl myhash FIELDS 1 f3] 5 10
        }

        test "HSETEX - Test 'PX' flag ($type)" {
            r del myhash
            assert_equal [r hsetex myhash PX 1000000 FIELDS 1 f3 v3] 1
            assert_range [r httl myhash FIELDS 1 f3] 990 1000
        }

        test "HSETEX - Test 'PXAT' flag ($type)" {
            r del myhash
            r hset myhash f1 v2 f2 v2 f3 v3
            assert_equal [r hsetex myhash PXAT [expr [clock milliseconds] + 10000] FIELDS 1 f2 v2] 1
            assert_range [r httl myhash FIELDS 1 f2] 5 10
        }

        test "HSETEX - Test 'KEEPTTL' flag ($type)" {
            r del myhash

            r hsetex myhash FIELDS 2 f1 v1 f2 v2
            r hsetex myhash PX 20000 FIELDS 1 f2 v2

            # f1 does not have ttl
            assert_equal [r httl myhash FIELDS 1 f1] "$T_NO_EXPIRY"

            # f2 has ttl
            assert_not_equal [r httl myhash FIELDS 1 f2] "$T_NO_EXPIRY"

            # Validate KEEPTTL preserves the TTL
            assert_equal [r hsetex myhash KEEPTTL FIELDS 1 f2 v22] 1
            assert_equal [r hget myhash f2] "v22"
            assert_not_equal [r httl myhash FIELDS 1 f2] "$T_NO_EXPIRY"

            # Try with multiple fields. First, set fields and TTL
            r hsetex myhash EX 10000 FIELDS 3 f1 v1 f2 v2 f3 v3

            # Update fields with KEEPTTL flag
            r hsetex myhash KEEPTTL FIELDS 3 f1 v111 f2 v222 f3 v333

            # Verify values are set, ttls are untouched
            assert_equal [lsort [r hgetall myhash]] [lsort "f1 v111 f2 v222 f3 v333"]
            assert_range [r httl myhash FIELDS 1 f1] 9000 10000
            assert_range [r httl myhash FIELDS 1 f2] 9000 10000
            assert_range [r httl myhash FIELDS 1 f3] 9000 10000
        }

        test "HSETEX - Test no expiry flag discards TTL ($type)" {
            r del myhash

            r hsetex myhash FIELDS 1 f1 v1
            r hsetex myhash PX 100000 FIELDS 1 f2 v2
            assert_range [r hpttl myhash FIELDS 1 f2] 1 100000

            assert_equal [r hsetex myhash FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [r httl myhash FIELDS 2 f1 f2] "$T_NO_EXPIRY $T_NO_EXPIRY"
        }

        test "HSETEX - Test with active expiry" {
            r del myhash
            r debug set-active-expire 0

            r hsetex myhash PX 10 FIELDS 5 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
            r debug set-active-expire 1
            wait_for_condition 50 20 { [r EXISTS myhash] == 0 } else { fail "'myhash' should be expired" }
        }

        test "HSETEX - Set time in the past ($type)" {
            r del myhash

            # Try on an empty key
            assert_equal [r hsetex myhash EXAT [expr {[clock seconds] - 1}] FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [r hexists myhash field1] 0

            # Try with existing fields
            r hset myhash fields 2 f1 v1 f2 v2
            assert_equal [r hsetex myhash EXAT [expr {[clock seconds] - 1}] FIELDS 2 f1 v1 f2 v2] 1
            assert_equal [r hexists myhash field1] 0
        }
    }

    test "Statistics - Hashes with HFEs ($type)" {
        r config resetstat
        r flushall

        # hash1: 5 fields, 3 with TTL. subexpiry incr +1
        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hpexpire myhash 150 FIELDS 3 f1 f2 f3
        assert_match  [get_stat_subexpiry r] 1

        # hash2: 5 fields, 3 with TTL. subexpiry incr +1
        r hset myhash2 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        assert_match  [get_stat_subexpiry r] 1
        r hpexpire myhash2 100 FIELDS 3 f1 f2 f3
        assert_match  [get_stat_subexpiry r] 2

        # hash3: 2 fields, 1 with TTL. HDEL field with TTL. subexpiry decr -1
        r hset myhash3 f1 v1 f2 v2
        r hpexpire myhash3 100 FIELDS 1 f2
        assert_match  [get_stat_subexpiry r] 3
        r hdel myhash3 f2
        assert_match  [get_stat_subexpiry r] 2

        # hash4: 2 fields, 1 with TTL. HGETDEL field with TTL. subexpiry decr -1
        r hset myhash4 f1 v1 f2 v2
        r hpexpire myhash4 100 FIELDS 1 f2
        assert_match [get_stat_subexpiry r] 3
        r hgetdel myhash4 FIELDS 1 f2
        assert_match [get_stat_subexpiry r] 2

        # Expired fields of hash1 and hash2. subexpiry decr -2
        wait_for_condition 50 50 {
                [get_stat_subexpiry r] == 0
        } else {
                fail "Hash field expiry statistics failed"
        }
    }

    test "HFE commands against wrong type" {
        r set wrongtype somevalue
        assert_error "WRONGTYPE Operation against a key*" {r hexpire wrongtype 10 fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hexpireat wrongtype 10 fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hpexpire wrongtype 10 fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hpexpireat wrongtype 10 fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hexpiretime wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hpexpiretime wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r httl wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hpttl wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hpersist wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hgetex wrongtype fields 1 f1}
        assert_error "WRONGTYPE Operation against a key*" {r hsetex wrongtype fields 1 f1 v1}
    }

    r config set hash-max-listpack-entries 512
}

start_server {tags {"external:skip needs:debug"}} {

    # Tests that only applies to listpack

    test "Test listpack memory usage" {
        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hpexpire myhash 5 FIELDS 2 f2 f4

        # Just to have code coverage for the new listpack encoding
        r memory usage myhash
    }

    test "Test listpack object encoding" {
        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hpexpire myhash 5 FIELDS 2 f2 f4

        # Just to have code coverage for the listpackex encoding
        assert_equal [r object encoding myhash] "listpackex"
    }

    test "Test listpack debug listpack" {
        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5

        # Just to have code coverage for the listpackex encoding
        r debug listpack myhash
    }

    test "Test listpack converts to ht and passive expiry works" {
        set prev [lindex [r config get hash-max-listpack-entries] 1]
        r config set hash-max-listpack-entries 10
        r debug set-active-expire 0

        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hpexpire myhash 5 FIELDS 2 f2 f4

        for {set i 6} {$i < 11} {incr i} {
            r hset myhash f$i v$i
        }
        after 50
        assert_equal [lsort [r hgetall myhash]] [lsort "f1 f3 f5 f6 f7 f8 f9 f10 v1 v3 v5 v6 v7 v8 v9 v10"]
        r config set hash-max-listpack-entries $prev
        r debug set-active-expire 1
    }

    test "Test listpack converts to ht and active expiry works" {
        r del myhash
        r debug set-active-expire 0

        r hset myhash f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hpexpire myhash 10 FIELDS 1 f1

        for {set i 0} {$i < 2048} {incr i} {
            r hset myhash f$i v$i
        }

        for {set i 0} {$i < 2048} {incr i} {
            r hpexpire myhash 10 FIELDS 1 f$i
        }

        r debug set-active-expire 1
        wait_for_condition 50 20 { [r EXISTS myhash] == 0 } else { fail "'myhash' should be expired" }
    }

    test "Test listpack converts to ht and active expiry works" {
        r del myhash
        r debug set-active-expire 0

        # Check expiry works after listpack converts to ht
        for {set i 0} {$i < 1024} {incr i} {
            r hset myhash f1_$i v1_$i f2_$i v2_$i f3_$i v3_$i f4_$i v4_$i
            r hpexpire myhash 10 FIELDS 4 f1_$i f2_$i f3_$i f4_$i
        }

        assert_encoding hashtable myhash
        assert_equal [r hlen myhash] 4096

        r debug set-active-expire 1
        wait_for_condition 50 20 { [r EXISTS myhash] == 0 } else { fail "'myhash' should be expired" }
    }

    test "HPERSIST/HEXPIRE - Test listpack with large values" {
        r del myhash

        # Test with larger values to verify we successfully move fields in
        # listpack when we are ordering according to TTL. This config change
        # will make code to use temporary heap allocation when moving fields.
        # See listpackExUpdateExpiry() for details.
        r config set hash-max-listpack-value 2048

        set payload1 [string repeat v3 1024]
        set payload2 [string repeat v1 1024]

        # Test with single item list
        r hset myhash f1 $payload1
        r hexpire myhash 2000 FIELDS 1 f1
        assert_equal [r hget myhash f1] $payload1
        r del myhash

        # Test with multiple items
        r hset myhash f1 $payload2 f2 v2 f3 $payload1 f4 v4
        r hexpire myhash 100000 FIELDS 1 f3
        r hpersist myhash FIELDS 1 f3
        assert_equal [r hpersist myhash FIELDS 1 f3] $P_NO_EXPIRY

        r hpexpire myhash 10 FIELDS 1 f1
        after 20
        assert_equal [lsort [r hgetall myhash]] [lsort "f2 f3 f4 v2 $payload1 v4"]

        r config set hash-max-listpack-value 64
    }

    test {Test HEXPIRE coexists with EXPIRE} {
        # Verify HEXPIRE & EXPIRE coexists. When setting EXPIRE a new kvobj might be
        # created whereas the old one can be ref by hash field expiration DS.
        # Take care to set hexpire before expire. Verify all combinations of
        # which expired first.
        # Another point to verify is that whether hexpire deletes the last field
        # and in turn the key (See f2).
        foreach etime {10 1000} htime {10 1000} f2 {0 1} {
            r del myhash
            r hset myhash f1 v1
            if {$f2} { r hset myhash f2 v2 }
            r hpexpire myhash $etime FIELDS 1 f1
            r pexpire myhash $htime
            after 20
            # If EXPIRE is shorter, it should delete the key.
            if {$etime == 10} {
                assert_equal [r httl myhash FIELDS 1 f1] $T_NO_FIELD
                assert_equal [r exists myhash] 0
            } else {
                if {$htime == 10} {
                    assert_equal [r httl myhash FIELDS 1 f1] $T_NO_FIELD
                    assert_range [r pttl myhash] 500 1000
                } else {
                    assert_range [r httl myhash FIELDS 1 f1] 1 1000
                    assert_range [r pttl myhash] 500 1000
                }
            }
        }
    }
}

start_server {tags {"external:skip needs:debug"}} {
    foreach type {listpack ht} {
        if {$type eq "ht"} {
            r config set hash-max-listpack-entries 0
        } else {
            r config set hash-max-listpack-entries 512
        }

        test "Test Command propagated to replica as expected ($type)" {
            start_server {overrides {appendonly {yes} appendfsync always} tags {external:skip}} {

                set aof [get_last_incr_aof_path r]
                r debug set-active-expire 0 ;# Prevent fields from being expired during data preparation

                # Time is in the past so it should propagate HDELs to replica
                # and delete the fields
                r hset h0 x1 y1 x2 y2
                r hexpireat h0 1 fields 3 x1 x2 non_exists_field

                r hset h1 f1 v1 f2 v2

                # Next command won't be propagated to replica
                # because XX condition not met or field not exists
                r hexpire h1 10 XX FIELDS 3 f1 f2 non_exists_field

                r hpexpire h1 20 FIELDS 1 f1

                # Next command will be propagate with only field 'f2'
                # because NX condition not met for field 'f1'
                r hpexpire h1 30 NX FIELDS 2 f1 f2

                # Non exists field should be ignored
                r hpexpire h1 30 FIELDS 1 non_exists_field
                r hset h2 f1 v1 f2 v2 f3 v3 f4 v4
                r hpexpire h2 40 FIELDS 2 f1 non_exists_field
                r hpexpire h2 50 FIELDS 1 f2
                r hpexpireat h2 [expr [clock seconds]*1000+100000] LT FIELDS 1 f3
                r hexpireat h2 [expr [clock seconds]+10] NX FIELDS 1 f4

                r debug set-active-expire 1
                wait_for_condition 50 100 {
                    [r hlen h2] eq 2
                } else {
                    fail "Field f2 of hash h2 wasn't deleted"
                }

                # HSETEX
                r hsetex h3 FIELDS 1 f1 v1
                r hsetex h3 FXX FIELDS 1 f1 v11
                r hsetex h3 FNX FIELDS 1 f2 v22
                r hsetex h3 KEEPTTL FIELDS 1 f2 v22

                # Next one will fail due to FNX arg and it won't be replicated
                r hsetex h3 FNX FIELDS 2 f1 v1 f2 v2

                # Commands with EX/PX/PXAT/EXAT will be replicated as PXAT
                r hsetex h3 EX 10000 FIELDS 1 f1 v111
                r hsetex h3 PX 10000 FIELDS 1 f1 v111
                r hsetex h3 PXAT [expr [clock milliseconds]+100000] FIELDS 1 f1 v111
                r hsetex h3 EXAT [expr [clock seconds]+100000] FIELDS 1 f1 v111

                # Following commands will set and then delete the fields because
                # of TTL in the past. HDELs will be propagated.
                r hsetex h3 PX 0 FIELDS 1 f1 v111
                r hsetex h3 PX 0 FIELDS 3 f1 v2 f2 v2 f3 v3

                # HGETEX
                r hsetex h4 FIELDS 3 f1 v1 f2 v2 f3 v3
                # No change on expiry, it won't be replicated.
                r hgetex h4 FIELDS 1 f1

                # Commands with EX/PX/PXAT/EXAT will be replicated as
                # HPEXPIREAT command.
                r hgetex h4 EX 10000 FIELDS 1 f1
                r hgetex h4 PX 10000 FIELDS 1 f1
                r hgetex h4 PXAT [expr [clock milliseconds]+100000] FIELDS 1 f1
                r hgetex h4 EXAT [expr [clock seconds]+100000] FIELDS 1 f1

                # Following commands will delete the fields because of TTL in
                # the past. HDELs will be propagated.
                r hgetex h4 PX 0 FIELDS 1 f1
                # HDELs will be propagated for f2 and f3 as only those exist.
                r hgetex h4 PX 0 FIELDS 3 f1 f2 f3

                # HGETEX with PERSIST flag will be replicated as HPERSIST
                r hsetex h4 EX 1000 FIELDS 1 f4 v4
                r hgetex h4 PERSIST FIELDS 1 f4

                # Nothing will be replicated as f4 is persisted already.
                r hgetex h4 PERSIST FIELDS 1 f4

                # Replicated as hdel
                r hgetdel h4 FIELDS 1 f4

                # Assert that each TTL-related command are persisted with absolute timestamps in AOF
                assert_aof_content $aof {
                    {select *}
                    {hset h0 x1 y1 x2 y2}
                    {multi}
                        {hdel h0 x1}
                        {hdel h0 x2}
                    {exec}
                    {hset h1 f1 v1 f2 v2}
                    {hpexpireat h1 * FIELDS 1 f1}
                    {hpexpireat h1 * FIELDS 1 f2}
                    {hset h2 f1 v1 f2 v2 f3 v3 f4 v4}
                    {hpexpireat h2 * FIELDS 1 f1}
                    {hpexpireat h2 * FIELDS 1 f2}
                    {hpexpireat h2 * FIELDS 1 f3}
                    {hpexpireat h2 * FIELDS 1 f4}
                    {hdel h1 f1}
                    {hdel h1 f2}
                    {hdel h2 f1}
                    {hdel h2 f2}
                    {hsetex h3 FIELDS 1 f1 v1}
                    {hsetex h3 FXX FIELDS 1 f1 v11}
                    {hsetex h3 FNX FIELDS 1 f2 v22}
                    {hsetex h3 KEEPTTL FIELDS 1 f2 v22}
                    {hsetex h3 PXAT * 1 f1 v111}
                    {hsetex h3 PXAT * 1 f1 v111}
                    {hsetex h3 PXAT * 1 f1 v111}
                    {hsetex h3 PXAT * 1 f1 v111}
                    {hdel h3 f1}
                    {multi}
                        {hdel h3 f1}
                        {hdel h3 f2}
                        {hdel h3 f3}
                    {exec}
                    {hsetex h4 FIELDS 3 f1 v1 f2 v2 f3 v3}
                    {hpexpireat h4 * FIELDS 1 f1}
                    {hpexpireat h4 * FIELDS 1 f1}
                    {hpexpireat h4 * FIELDS 1 f1}
                    {hpexpireat h4 * FIELDS 1 f1}
                    {hdel h4 f1}
                    {multi}
                        {hdel h4 f2}
                        {hdel h4 f3}
                    {exec}
                    {hsetex h4 PXAT * FIELDS 1 f4 v4}
                    {hpersist h4 FIELDS 1 f4}
                    {hdel h4 f4}
                }
            }
        } {} {needs:debug}

        test "Lazy Expire - fields are lazy deleted and propagated to replicas ($type)" {
            start_server {overrides {appendonly {yes} appendfsync always} tags {external:skip}} {
                r debug set-active-expire 0
                set aof [get_last_incr_aof_path r]

                r del myhash

                r hset myhash f1 v1 f2 v2 f3 v3
                r hpexpire myhash 1 NX FIELDS 3 f1 f2 f3
                after 5

                # Verify that still exists even if all fields are expired
                assert_equal 1 [r EXISTS myhash]

                # Verify that len counts also expired fields
                assert_equal 3 [r HLEN myhash]

                # Trying access to expired field should delete it. Len should be updated
                assert_equal 0 [r hexists myhash f1]
                assert_equal 2 [r HLEN myhash]

                # Trying access another expired field should delete it. Len should be updated
                assert_equal "" [r hget myhash f2]
                assert_equal 1 [r HLEN myhash]

                # Trying access last expired field should delete it. hash shouldn't exists afterward.
                assert_equal 0 [r hstrlen myhash f3]
                assert_equal 0 [r HLEN myhash]
                assert_equal 0 [r EXISTS myhash]

                wait_for_condition 50 100 { [r exists h1] == 0 } else { fail "hash h1 wasn't deleted" }

                # HDEL are propagated as expected
                assert_aof_content $aof {
                    {select *}
                    {hset myhash f1 v1 f2 v2 f3 v3}
                    {hpexpireat myhash * NX FIELDS 3 f1 f2 f3}
                    {hdel myhash f1}
                    {hdel myhash f2}
                    {hdel myhash f3}
                }
                r debug set-active-expire 1
            }
        }

        # Start a new server with empty data and AOF file.
        start_server {overrides {appendonly {yes} appendfsync always} tags {external:skip}} {

            # Based on test at expire.tcl: " All time-to-live(TTL) in commands are propagated as absolute ..."
            test {All TTLs in commands are propagated as absolute timestamp in milliseconds in AOF} {

                set aof [get_last_incr_aof_path r]

                r hset h1 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6
                r hexpireat h1 [expr [clock seconds]+100] NX FIELDS 1 f1
                r hpexpireat h1 [expr [clock seconds]*1000+100000] NX FIELDS 1 f2
                r hpexpire h1 100000 NX FIELDS 3 f3 f4 f5
                r hexpire h1 100000 FIELDS 1 f6

                r hset h2 f1 v1 f2 v2
                r hpexpire h2 1 FIELDS 2 f1 f2
                after 200

                r hsetex h3 EX 100000 FIELDS 2 f1 v1 f2 v2
                r hsetex h3 EXAT [expr [clock seconds] + 1000] FIELDS 2 f1 v1 f2 v2
                r hsetex h3 PX 100000 FIELDS 2 f1 v1 f2 v2
                r hsetex h3 PXAT [expr [clock milliseconds]+100000] FIELDS 2 f1 v1 f2 v2

                r hgetex h3 EX 100000 FIELDS 2 f1 f2
                r hgetex h3 EXAT [expr [clock seconds] + 1000] FIELDS 2 f1 f2
                r hgetex h3 PX 100000 FIELDS 2 f1 f2
                r hgetex h3 PXAT [expr [clock milliseconds]+100000] FIELDS 2 f1 f2

                assert_aof_content $aof {
                    {select *}
                    {hset h1 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5 f6 v6}
                    {hpexpireat h1 * FIELDS 1 f1}
                    {hpexpireat h1 * FIELDS 1 f2}
                    {hpexpireat h1 * NX FIELDS 3 f3 f4 f5}
                    {hpexpireat h1 * FIELDS 1 f6}
                    {hset h2 f1 v1 f2 v2}
                    {hpexpireat h2 * FIELDS 2 f1 f2}
                    {hdel h2 *}
                    {hdel h2 *}
                    {hsetex h3 PXAT * FIELDS 2 f1 v1 f2 v2}
                    {hsetex h3 PXAT * FIELDS 2 f1 v1 f2 v2}
                    {hsetex h3 PXAT * FIELDS 2 f1 v1 f2 v2}
                    {hsetex h3 PXAT * FIELDS 2 f1 v1 f2 v2}
                    {hpexpireat h3 * FIELDS 2 f1 f2}
                    {hpexpireat h3 * FIELDS 2 f1 f2}
                    {hpexpireat h3 * FIELDS 2 f1 f2}
                    {hpexpireat h3 * FIELDS 2 f1 f2}
                }

                array set keyAndFields1 [dumpAllHashes r]
                r debug loadaof
                array set keyAndFields2 [dumpAllHashes r]

                # Assert that absolute TTLs are the same
                assert_equal [array get keyAndFields1] [array get keyAndFields2]

            } {} {needs:debug}
        }

        # Based on test, with same name, at expire.tcl:
        test {All TTL in commands are propagated as absolute timestamp in replication stream} {
            # Make sure that both relative and absolute expire commands are propagated
            # Consider also comment of the test, with same name, at expire.tcl

            r flushall ; # Clean up keyspace to avoid interference by keys from other tests
            set repl [attach_to_replication_stream]

            # HEXPIRE/HPEXPIRE should be translated into HPEXPIREAT
            r hset h1 f1 v1
            r hexpireat h1 [expr [clock seconds]+100] NX FIELDS 1 f1
            r hset h2 f2 v2
            r hpexpireat h2 [expr [clock seconds]*1000+100000] NX FIELDS 1 f2
            r hset h3 f3 v3 f4 v4 f5 v5
            # hpersist does nothing here. Verify it is not propagated.
            r hpersist h3 FIELDS 1 f5
            r hexpire h3 100 FIELDS 3 f3 f4 non_exists_field
            r hpersist h3 FIELDS 1 f3

            assert_replication_stream $repl {
                {select *}
                {hset h1 f1 v1}
                {hpexpireat h1 * NX FIELDS 1 f1}
                {hset h2 f2 v2}
                {hpexpireat h2 * NX FIELDS 1 f2}
                {hset h3 f3 v3 f4 v4 f5 v5}
                {hpexpireat h3 * FIELDS 2 f3 f4}
                {hpersist h3 FIELDS 1 f3}
            }
            close_replication_stream $repl
        } {} {needs:repl}

        test {HRANDFIELD delete expired fields and propagate DELs to replica} {
            r debug set-active-expire 0
            r flushall
            set repl [attach_to_replication_stream]

            # HRANDFIELD delete expired fields and propagate MULTI-EXEC DELs. Reply none.
            r hset h1 f1 v1 f2 v2
            r hpexpire h1 1 FIELDS 2 f1 f2
            after 5
            assert_equal [r hrandfield h1 2] ""

            # HRANDFIELD delete expired field and propagate DEL. Reply non-expired field.
            r hset h2 f1 v1 f2 v2
            r hpexpire h2 1 FIELDS 1 f1
            after 5
            assert_equal [r hrandfield h2 2] "f2"

            # HRANDFIELD delete expired field and propagate DEL. Reply none.
            r hset h3 f1 v1
            r hpexpire h3 1 FIELDS 1 f1
            after 5
            assert_equal [r hrandfield h3 2] ""

            assert_replication_stream $repl {
                {select *}
                {hset h1 f1 v1 f2 v2}
                {hpexpireat h1 * FIELDS 2 f1 f2}
                {multi}
                {hdel h1 *}
                {hdel h1 *}
                {exec}
                {hset h2 f1 v1 f2 v2}
                {hpexpireat h2 * FIELDS 1 f1}
                {hdel h2 f1}
                {hset h3 f1 v1}
                {hpexpireat h3 * FIELDS 1 f1}
                {hdel h3 f1}
            }
            close_replication_stream $repl
            r debug set-active-expire 1
        } {OK} {needs:repl}

        # Start another server to test replication of TTLs
        start_server {tags {needs:repl external:skip}} {
            # Set the outer layer server as primary
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]
            # Set this inner layer server as replica
            set replica [srv 0 client]

            # Server should have role slave
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 role] eq {slave}
            } else {
                fail "Replication not started."
            }

            # Based on test, with same name, at expire.tcl
            test {For all replicated TTL-related commands, absolute expire times are identical on primary and replica} {
                # Apply each TTL-related command to a unique key on primary
                $primary flushall
                $primary hset h1 f v
                $primary hexpireat h1 [expr [clock seconds]+10000] FIELDS 1 f
                $primary hset h2 f v
                $primary hpexpireat h2 [expr [clock milliseconds]+100000] FIELDS 1 f
                $primary hset h3 f v
                $primary hexpire h3 100 NX FIELDS 1 f
                $primary hset h4 f v
                $primary hpexpire h4 100000 NX FIELDS 1 f
                $primary hset h5 f v
                $primary hpexpireat h5 [expr [clock milliseconds]-100000] FIELDS 1 f
                $primary hset h9 f v

                $primary hsetex h10 EX 100000 FIELDS 1 f v
                $primary hsetex h11 EXAT [expr [clock seconds] + 1000] FIELDS 1 f v
                $primary hsetex h12 PX 100000 FIELDS 1 f v
                $primary hsetex h13 PXAT [expr [clock milliseconds]+100000] FIELDS 1 f v
                $primary hsetex h14 PXAT 1 FIELDS 1 f v

                $primary hsetex h15 FIELDS 1 f v
                $primary hgetex h15 EX 100000 FIELDS 1 f
                $primary hsetex h16 FIELDS 1 f v
                $primary hgetex h16 EXAT [expr [clock seconds] + 1000] FIELDS 1 f
                $primary hsetex h17 FIELDS 1 f v
                $primary hgetex h17 PX 100000 FIELDS 1 f
                $primary hsetex h18 FIELDS 1 f v
                $primary hgetex h18 PXAT [expr [clock milliseconds]+100000] FIELDS 1 f
                $primary hsetex h19 FIELDS 1 f v
                $primary hgetex h19 PXAT 1 FIELDS 1 f

                # Wait for replica to get the keys and TTLs
                assert {[$primary wait 1 0] == 1}

                # Verify absolute TTLs are identical on primary and replica for all keys
                # This is because TTLs are always replicated as absolute values
                assert_equal [dumpAllHashes $primary] [dumpAllHashes $replica]
            }
        }

        test "Test HSETEX command replication" {
            r flushall
            set repl [attach_to_replication_stream]

            # Create a field and delete it in a single command due to timestamp
            # being in the past. It will be propagated as HDEL.
            r hsetex h1 PXAT 1 FIELDS 1 f1 v1

            # Following ones will be propagated with PXAT arg
            r hsetex h1 EX 100000 FIELDS 1 f1 v1
            r hsetex h1 EXAT [expr [clock seconds] + 1000] FIELDS 1 f1 v1
            r hsetex h1 PX 100000 FIELDS 1 f1 v1
            r hsetex h1 PXAT [expr [clock milliseconds]+100000] FIELDS 1 f1 v1

            # Propagate with KEEPTTL flag
            r hsetex h1 KEEPTTL FIELDS 1 f1 v1

            # Following commands will fail and won't be propagated
            r hsetex h1 FNX FIELDS 1 f1 v11
            r hsetex h1 FXX FIELDS 1 f2 v2

            # Propagate with FNX and FXX flags
            r hsetex h1 FNX FIELDS 1 f2 v2
            r hsetex h1 FXX FIELDS 1 f2 v22

            assert_replication_stream $repl {
                {select *}
                {hdel h1 f1}
                {hsetex h1 PXAT * FIELDS 1 f1 v1}
                {hsetex h1 PXAT * FIELDS 1 f1 v1}
                {hsetex h1 PXAT * FIELDS 1 f1 v1}
                {hsetex h1 PXAT * FIELDS 1 f1 v1}
                {hsetex h1 KEEPTTL FIELDS 1 f1 v1}
                {hsetex h1 FNX FIELDS 1 f2 v2}
                {hsetex h1 FXX FIELDS 1 f2 v22}
            }
            close_replication_stream $repl
        } {} {needs:repl}

        test "Test HGETEX command replication" {
            r flushall
            r debug set-active-expire 0
            set repl [attach_to_replication_stream]

            # If no fields are found, command won't be replicated
            r hgetex h1 EX 10000 FIELDS 1 f0
            r hgetex h1 PERSIST FIELDS 1 f0

            # Get without setting expiry will not be replicated
            r hsetex h1 FIELDS 1 f0 v0
            r hgetex h1 FIELDS 1 f0

            # Lazy expired field will be replicated as HDEL
            r hsetex h1 PX 10 FIELDS 1 f1 v1
            after 15
            r hgetex h1 EX 1000 FIELDS 1 f1

            # If new TTL is in the past, it will be replicated as HDEL
            r hsetex h1 EX 10000 FIELDS 1 f2 v2
            r hgetex h1 EXAT 1 FIELDS 1 f2

            # A field will expire lazily and other field will be deleted due to
            # TTL is being in the past. It'll be propagated as two HDEL's.
            r hsetex h1 PX 10 FIELDS 1 f3 v3
            after 15
            r hsetex h1 FIELDS 1 f4 v4
            r hgetex h1 EXAT 1 FIELDS 2 f3 f4

            # TTL update, it will be replicated as HPEXPIREAT
            r hsetex h1 FIELDS 1 f5 v5
            r hgetex h1 EX 10000 FIELDS 1 f5

            # If PERSIST flag is used, it will be replicated as HPERSIST
            r hsetex h1 EX 10000 FIELDS 1 f6 v6
            r hgetex h1 PERSIST FIELDS 1 f6

            assert_replication_stream $repl {
                {select *}
                {hsetex h1 FIELDS 1 f0 v0}
                {hsetex h1 PXAT * FIELDS 1 f1 v1}
                {hdel h1 f1}
                {hsetex h1 PXAT * FIELDS 1 f2 v2}
                {hdel h1 f2}
                {hsetex h1 PXAT * FIELDS 1 f3 v3}
                {hsetex h1 FIELDS 1 f4 v4}
                {multi}
                    {hdel h1 f3}
                    {hdel h1 f4}
                {exec}
                {hsetex h1 FIELDS 1 f5 v5}
                {hpexpireat h1 * FIELDS 1 f5}
                {hsetex h1 PXAT * FIELDS 1 f6 v6}
                {hpersist h1 FIELDS 1 f6}
            }
            close_replication_stream $repl
        } {} {needs:repl}

        test "HINCRBYFLOAT command won't remove field expiration on replica ($type)" {
            r flushall
            set repl [attach_to_replication_stream]

            r hsetex h1 EX 100 FIELDS 1 f1 1
            r hset h1 f2 1
            r hincrbyfloat h1 f1 1.1
            r hincrbyfloat h1 f2 1.1

            # HINCRBYFLOAT will be replicated as HSETEX with KEEPTTL flag
            assert_replication_stream $repl {
                {select *}
                {hsetex h1 PXAT * FIELDS 1 f1 1}
                {hset h1 f2 1}
                {hsetex h1 KEEPTTL FIELDS 1 f1 *}
                {hsetex h1 KEEPTTL FIELDS 1 f2 *}
            }
            close_replication_stream $repl

            start_server {tags {external:skip}} {
                r -1 flushall
                r slaveof [srv -1 host] [srv -1 port]
                wait_for_sync r

                r -1 hsetex h1 EX 100 FIELDS 1 f1 1
                r -1 hset h1 f2 1
                wait_for_ofs_sync  [srv -1 client]  [srv 0 client]
                assert_range [r httl h1 FIELDS 1 f1] 90 100
                assert_equal {-1} [r httl h1 FIELDS 1 f2]

                r -1 hincrbyfloat h1 f1 1.1
                r -1 hincrbyfloat h1 f2 1.1

                # Expiration time should not be removed on replica and the value
                # should be equal to the master.
                wait_for_ofs_sync  [srv -1 client]  [srv 0 client]
                assert_range [r httl h1 FIELDS 1 f1] 90 100
                assert_equal [r -1 hget h1 f1] [r hget h1 f1]

                # The field f2 should not have any expiration on replica either even
                # though it was set using HSET with KEEPTTL flag.
                assert_equal {-1} [r httl h1 FIELDS 1 f2]
                assert_equal [r -1 hget h1 f2] [r hget h1 f2]
            }
        } {} {needs:repl external:skip}
    }
}
