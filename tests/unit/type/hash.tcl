start_server {tags {"hash"}} {
    r config set hash-max-listpack-value 64
    r config set hash-max-listpack-entries 512

    test {HSET/HLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key __avoid_collisions__[randstring 0 8 alpha]
            set val __avoid_collisions__[randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r hset smallhash $key $val
            set smallhash($key) $val
        }
        list [r hlen smallhash]
    } {8}

    test {Is the small hash encoded with a listpack?} {
        assert_encoding listpack smallhash
    }

    proc create_hash {key entries} {
        r del $key
        foreach entry $entries {
            r hset $key [lindex $entry 0] [lindex $entry 1]
        }
    }

    proc get_keys {l} {
        set res {}
        foreach entry $l {
            set key [lindex $entry 0]
            lappend res $key
        }
        return $res
    }

    foreach {type contents} "listpack {{a 1} {b 2} {c 3}} hashtable {{a 1} {b 2} {[randstring 70 90 alpha] 3}}" {
        set original_max_value [lindex [r config get hash-max-ziplist-value] 1]
        r config set hash-max-ziplist-value 10
        create_hash myhash $contents
        assert_encoding $type myhash

        # coverage for objectComputeSize
        assert_morethan [memory_usage myhash] 0

        test "HRANDFIELD - $type" {
            unset -nocomplain myhash
            array set myhash {}
            for {set i 0} {$i < 100} {incr i} {
                set key [r hrandfield myhash]
                set myhash($key) 1
            }
            assert_equal [lsort [get_keys $contents]] [lsort [array names myhash]]
        }
        r config set hash-max-ziplist-value $original_max_value
    }

    test "HRANDFIELD with RESP3" {
        r hello 3
        set res [r hrandfield myhash 3 withvalues]
        assert_equal [llength $res] 3
        assert_equal [llength [lindex $res 1]] 2

        set res [r hrandfield myhash 3]
        assert_equal [llength $res] 3
        assert_equal [llength [lindex $res 1]] 1
        r hello 2
    }

    test "HRANDFIELD count of 0 is handled correctly" {
        r hrandfield myhash 0
    } {}

    test "HRANDFIELD count overflow" {
        r hmset myhash a 1
        assert_error {*value is out of range*} {r hrandfield myhash -9223372036854770000 withvalues}
        assert_error {*value is out of range*} {r hrandfield myhash -9223372036854775808 withvalues}
        assert_error {*value is out of range*} {r hrandfield myhash -9223372036854775808}
    } {}

    test "HRANDFIELD with <count> against non existing key" {
        r hrandfield nonexisting_key 100
    } {}

    # Make sure we can distinguish between an empty array and a null response
    r readraw 1

    test "HRANDFIELD count of 0 is handled correctly - emptyarray" {
        r hrandfield myhash 0
    } {*0}

    test "HRANDFIELD with <count> against non existing key - emptyarray" {
        r hrandfield nonexisting_key 100
    } {*0}

    r readraw 0

    foreach {type contents} "
        hashtable {{a 1} {b 2} {c 3} {d 4} {e 5} {6 f} {7 g} {8 h} {9 i} {[randstring 70 90 alpha] 10}}
        listpack {{a 1} {b 2} {c 3} {d 4} {e 5} {6 f} {7 g} {8 h} {9 i} {10 j}} " {
        test "HRANDFIELD with <count> - $type" {
            set original_max_value [lindex [r config get hash-max-ziplist-value] 1]
            r config set hash-max-ziplist-value 10
            create_hash myhash $contents
            assert_encoding $type myhash

            # create a dict for easy lookup
            set mydict [dict create {*}[r hgetall myhash]]

            # We'll stress different parts of the code, see the implementation
            # of HRANDFIELD for more information, but basically there are
            # four different code paths.

            # PATH 1: Use negative count.

            # 1) Check that it returns repeated elements with and without values.
            set res [r hrandfield myhash -20]
            assert_equal [llength $res] 20
            set res [r hrandfield myhash -1001]
            assert_equal [llength $res] 1001
            # again with WITHVALUES
            set res [r hrandfield myhash -20 withvalues]
            assert_equal [llength $res] 40
            set res [r hrandfield myhash -1001 withvalues]
            assert_equal [llength $res] 2002

            # Test random uniform distribution
            # df = 9, 40 means 0.00001 probability
            set res [r hrandfield myhash -1000]
            assert_lessthan [chi_square_value $res] 40

            # 2) Check that all the elements actually belong to the original hash.
            foreach {key val} $res {
                assert {[dict exists $mydict $key]}
            }

            # 3) Check that eventually all the elements are returned.
            #    Use both WITHVALUES and without
            unset -nocomplain auxset
            set iterations 1000
            while {$iterations != 0} {
                incr iterations -1
                if {[expr {$iterations % 2}] == 0} {
                    set res [r hrandfield myhash -3 withvalues]
                    foreach {key val} $res {
                        dict append auxset $key $val
                    }
                } else {
                    set res [r hrandfield myhash -3]
                    foreach key $res {
                        dict append auxset $key $val
                    }
                }
                if {[lsort [dict keys $mydict]] eq
                    [lsort [dict keys $auxset]]} {
                    break;
                }
            }
            assert {$iterations != 0}

            # PATH 2: positive count (unique behavior) with requested size
            # equal or greater than set size.
            foreach size {10 20} {
                set res [r hrandfield myhash $size]
                assert_equal [llength $res] 10
                assert_equal [lsort $res] [lsort [dict keys $mydict]]

                # again with WITHVALUES
                set res [r hrandfield myhash $size withvalues]
                assert_equal [llength $res] 20
                assert_equal [lsort $res] [lsort $mydict]
            }

            # PATH 3: Ask almost as elements as there are in the set.
            # In this case the implementation will duplicate the original
            # set and will remove random elements up to the requested size.
            #
            # PATH 4: Ask a number of elements definitely smaller than
            # the set size.
            #
            # We can test both the code paths just changing the size but
            # using the same code.
            foreach size {8 2} {
                set res [r hrandfield myhash $size]
                assert_equal [llength $res] $size
                # again with WITHVALUES
                set res [r hrandfield myhash $size withvalues]
                assert_equal [llength $res] [expr {$size * 2}]

                # 1) Check that all the elements actually belong to the
                # original set.
                foreach ele [dict keys $res] {
                    assert {[dict exists $mydict $ele]}
                }

                # 2) Check that eventually all the elements are returned.
                #    Use both WITHVALUES and without
                unset -nocomplain auxset
                unset -nocomplain allkey
                set iterations [expr {1000 / $size}]
                set all_ele_return false
                while {$iterations != 0} {
                    incr iterations -1
                    if {[expr {$iterations % 2}] == 0} {
                        set res [r hrandfield myhash $size withvalues]
                        foreach {key value} $res {
                            dict append auxset $key $value
                            lappend allkey $key
                        }
                    } else {
                        set res [r hrandfield myhash $size]
                        foreach key $res {
                            dict append auxset $key
                            lappend allkey $key
                        }
                    }
                    if {[lsort [dict keys $mydict]] eq
                        [lsort [dict keys $auxset]]} {
                        set all_ele_return true
                    }
                }
                assert_equal $all_ele_return true
                # df = 9, 40 means 0.00001 probability
                assert_lessthan [chi_square_value $allkey] 40
            }
        }
        r config set hash-max-ziplist-value $original_max_value
    }


    test {HSET/HLEN - Big hash creation} {
        array set bighash {}
        for {set i 0} {$i < 1024} {incr i} {
            set key __avoid_collisions__[randstring 0 8 alpha]
            set val __avoid_collisions__[randstring 0 8 alpha]
            if {[info exists bighash($key)]} {
                incr i -1
                continue
            }
            r hset bighash $key $val
            set bighash($key) $val
        }
        list [r hlen bighash]
    } {1024}

    test {Is the big hash encoded with an hash table?} {
        assert_encoding hashtable bighash
    }

    test {HGET against the small hash} {
        set err {}
        foreach k [array names smallhash *] {
            if {$smallhash($k) ne [r hget smallhash $k]} {
                set err "$smallhash($k) != [r hget smallhash $k]"
                break
            }
        }
        set _ $err
    } {}

    test {HGET against the big hash} {
        set err {}
        foreach k [array names bighash *] {
            if {$bighash($k) ne [r hget bighash $k]} {
                set err "$bighash($k) != [r hget bighash $k]"
                break
            }
        }
        set _ $err
    } {}

    test {HGET against non existing key} {
        set rv {}
        lappend rv [r hget smallhash __123123123__]
        lappend rv [r hget bighash __123123123__]
        set _ $rv
    } {{} {}}

    test {HSET in update and insert mode} {
        set rv {}
        set k [lindex [array names smallhash *] 0]
        lappend rv [r hset smallhash $k newval1]
        set smallhash($k) newval1
        lappend rv [r hget smallhash $k]
        lappend rv [r hset smallhash __foobar123__ newval]
        set k [lindex [array names bighash *] 0]
        lappend rv [r hset bighash $k newval2]
        set bighash($k) newval2
        lappend rv [r hget bighash $k]
        lappend rv [r hset bighash __foobar123__ newval]
        lappend rv [r hdel smallhash __foobar123__]
        lappend rv [r hdel bighash __foobar123__]
        set _ $rv
    } {0 newval1 1 0 newval2 1 1 1}

    test {HSETNX target key missing - small hash} {
        r hsetnx smallhash __123123123__ foo
        r hget smallhash __123123123__
    } {foo}

    test {HSETNX target key exists - small hash} {
        r hsetnx smallhash __123123123__ bar
        set result [r hget smallhash __123123123__]
        r hdel smallhash __123123123__
        set _ $result
    } {foo}

    test {HSETNX target key missing - big hash} {
        r hsetnx bighash __123123123__ foo
        r hget bighash __123123123__
    } {foo}

    test {HSETNX target key exists - big hash} {
        r hsetnx bighash __123123123__ bar
        set result [r hget bighash __123123123__]
        r hdel bighash __123123123__
        set _ $result
    } {foo}

    test {HSET/HMSET wrong number of args} {
        assert_error {*wrong number of arguments for 'hset' command} {r hset smallhash key1 val1 key2}
        assert_error {*wrong number of arguments for 'hmset' command} {r hmset smallhash key1 val1 key2}
    }

    test {HMSET - small hash} {
        set args {}
        foreach {k v} [array get smallhash] {
            set newval [randstring 0 8 alpha]
            set smallhash($k) $newval
            lappend args $k $newval
        }
        r hmset smallhash {*}$args
    } {OK}

    test {HMSET - big hash} {
        set args {}
        foreach {k v} [array get bighash] {
            set newval [randstring 0 8 alpha]
            set bighash($k) $newval
            lappend args $k $newval
        }
        r hmset bighash {*}$args
    } {OK}

    test {HMGET against non existing key and fields} {
        set rv {}
        lappend rv [r hmget doesntexist __123123123__ __456456456__]
        lappend rv [r hmget smallhash __123123123__ __456456456__]
        lappend rv [r hmget bighash __123123123__ __456456456__]
        set _ $rv
    } {{{} {}} {{} {}} {{} {}}}

    test {Hash commands against wrong type} {
        r set wrongtype somevalue
        assert_error "WRONGTYPE Operation against a key*" {r hmget wrongtype field1 field2}
        assert_error "WRONGTYPE Operation against a key*" {r hrandfield wrongtype}
        assert_error "WRONGTYPE Operation against a key*" {r hget wrongtype field1}
        assert_error "WRONGTYPE Operation against a key*" {r hgetall wrongtype}
        assert_error "WRONGTYPE Operation against a key*" {r hdel wrongtype field1}
        assert_error "WRONGTYPE Operation against a key*" {r hincrby wrongtype field1 2}
        assert_error "WRONGTYPE Operation against a key*" {r hincrbyfloat wrongtype field1 2.5}
        assert_error "WRONGTYPE Operation against a key*" {r hstrlen wrongtype field1}
        assert_error "WRONGTYPE Operation against a key*" {r hvals wrongtype}
        assert_error "WRONGTYPE Operation against a key*" {r hkeys wrongtype}
        assert_error "WRONGTYPE Operation against a key*" {r hexists wrongtype field1}
        assert_error "WRONGTYPE Operation against a key*" {r hset wrongtype field1 val1}
        assert_error "WRONGTYPE Operation against a key*" {r hmset wrongtype field1 val1 field2 val2}
        assert_error "WRONGTYPE Operation against a key*" {r hsetnx wrongtype field1 val1}
        assert_error "WRONGTYPE Operation against a key*" {r hlen wrongtype}
        assert_error "WRONGTYPE Operation against a key*" {r hscan wrongtype 0}
        assert_error "WRONGTYPE Operation against a key*" {r hgetdel wrongtype fields 1 a}
    }

    test {HMGET - small hash} {
        set keys {}
        set vals {}
        foreach {k v} [array get smallhash] {
            lappend keys $k
            lappend vals $v
        }
        set err {}
        set result [r hmget smallhash {*}$keys]
        if {$vals ne $result} {
            set err "$vals != $result"
            break
        }
        set _ $err
    } {}

    test {HMGET - big hash} {
        set keys {}
        set vals {}
        foreach {k v} [array get bighash] {
            lappend keys $k
            lappend vals $v
        }
        set err {}
        set result [r hmget bighash {*}$keys]
        if {$vals ne $result} {
            set err "$vals != $result"
            break
        }
        set _ $err
    } {}

    test {HKEYS - small hash} {
        lsort [r hkeys smallhash]
    } [lsort [array names smallhash *]]

    test {HKEYS - big hash} {
        lsort [r hkeys bighash]
    } [lsort [array names bighash *]]

    test {HVALS - small hash} {
        set vals {}
        foreach {k v} [array get smallhash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    } [lsort [r hvals smallhash]]

    test {HVALS - big hash} {
        set vals {}
        foreach {k v} [array get bighash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    } [lsort [r hvals bighash]]

    test {HGETALL - small hash} {
        lsort [r hgetall smallhash]
    } [lsort [array get smallhash]]

    test {HGETALL - big hash} {
        lsort [r hgetall bighash]
    } [lsort [array get bighash]]

    test {HGETALL against non-existing key} {
        r del htest
        r hgetall htest
    } {}

    test {HDEL and return value} {
        set rv {}
        lappend rv [r hdel smallhash nokey]
        lappend rv [r hdel bighash nokey]
        set k [lindex [array names smallhash *] 0]
        lappend rv [r hdel smallhash $k]
        lappend rv [r hdel smallhash $k]
        lappend rv [r hget smallhash $k]
        unset smallhash($k)
        set k [lindex [array names bighash *] 0]
        lappend rv [r hdel bighash $k]
        lappend rv [r hdel bighash $k]
        lappend rv [r hget bighash $k]
        unset bighash($k)
        set _ $rv
    } {0 0 1 0 {} 1 0 {}}

    test {HDEL - more than a single value} {
        set rv {}
        r del myhash
        r hmset myhash a 1 b 2 c 3
        assert_equal 0 [r hdel myhash x y]
        assert_equal 2 [r hdel myhash a c f]
        r hgetall myhash
    } {b 2}

    test {HDEL - hash becomes empty before deleting all specified fields} {
        r del myhash
        r hmset myhash a 1 b 2 c 3
        assert_equal 3 [r hdel myhash a b c d e]
        assert_equal 0 [r exists myhash]
    }

    test {HEXISTS} {
        set rv {}
        set k [lindex [array names smallhash *] 0]
        lappend rv [r hexists smallhash $k]
        lappend rv [r hexists smallhash nokey]
        set k [lindex [array names bighash *] 0]
        lappend rv [r hexists bighash $k]
        lappend rv [r hexists bighash nokey]
    } {1 0 1 0}

    test {Is a ziplist encoded Hash promoted on big payload?} {
        r hset smallhash foo [string repeat a 1024]
        r debug object smallhash
    } {*hashtable*} {needs:debug}

    test {HINCRBY against non existing database key} {
        r del htest
        list [r hincrby htest foo 2]
    } {2}

    test {HINCRBY HINCRBYFLOAT against non-integer increment value} {
        r del incrhash
        r hset incrhash field 5
        assert_error "*value is not an integer*" {r hincrby incrhash field v}
        assert_error "*value is not a*" {r hincrbyfloat incrhash field v}
    }

    test {HINCRBY against non existing hash key} {
        set rv {}
        r hdel smallhash tmp
        r hdel bighash tmp
        lappend rv [r hincrby smallhash tmp 2]
        lappend rv [r hget smallhash tmp]
        lappend rv [r hincrby bighash tmp 2]
        lappend rv [r hget bighash tmp]
    } {2 2 2 2}

    test {HINCRBY against hash key created by hincrby itself} {
        set rv {}
        lappend rv [r hincrby smallhash tmp 3]
        lappend rv [r hget smallhash tmp]
        lappend rv [r hincrby bighash tmp 3]
        lappend rv [r hget bighash tmp]
    } {5 5 5 5}

    test {HINCRBY against hash key originally set with HSET} {
        r hset smallhash tmp 100
        r hset bighash tmp 100
        list [r hincrby smallhash tmp 2] [r hincrby bighash tmp 2]
    } {102 102}

    test {HINCRBY over 32bit value} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrby smallhash tmp 1] [r hincrby bighash tmp 1]
    } {17179869185 17179869185}

    test {HINCRBY over 32bit value with over 32bit increment} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrby smallhash tmp 17179869184] [r hincrby bighash tmp 17179869184]
    } {34359738368 34359738368}

    test {HINCRBY fails against hash value with spaces (left)} {
        r hset smallhash str " 11"
        r hset bighash str " 11"
        catch {r hincrby smallhash str 1} smallerr
        catch {r hincrby bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR *not an integer*" $smallerr]
        lappend rv [string match "ERR *not an integer*" $bigerr]
    } {1 1}

    test {HINCRBY fails against hash value with spaces (right)} {
        r hset smallhash str "11 "
        r hset bighash str "11 "
        catch {r hincrby smallhash str 1} smallerr
        catch {r hincrby bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR *not an integer*" $smallerr]
        lappend rv [string match "ERR *not an integer*" $bigerr]
    } {1 1}

    test {HINCRBY can detect overflows} {
        set e {}
        r hset hash n -9223372036854775484
        assert {[r hincrby hash n -1] == -9223372036854775485}
        catch {r hincrby hash n -10000} e
        set e
    } {*overflow*}

    test {HINCRBYFLOAT against non existing database key} {
        r del htest
        list [r hincrbyfloat htest foo 2.5]
    } {2.5}

    test {HINCRBYFLOAT against non existing hash key} {
        set rv {}
        r hdel smallhash tmp
        r hdel bighash tmp
        lappend rv [roundFloat [r hincrbyfloat smallhash tmp 2.5]]
        lappend rv [roundFloat [r hget smallhash tmp]]
        lappend rv [roundFloat [r hincrbyfloat bighash tmp 2.5]]
        lappend rv [roundFloat [r hget bighash tmp]]
    } {2.5 2.5 2.5 2.5}

    test {HINCRBYFLOAT against hash key created by hincrby itself} {
        set rv {}
        lappend rv [roundFloat [r hincrbyfloat smallhash tmp 3.5]]
        lappend rv [roundFloat [r hget smallhash tmp]]
        lappend rv [roundFloat [r hincrbyfloat bighash tmp 3.5]]
        lappend rv [roundFloat [r hget bighash tmp]]
    } {6 6 6 6}

    test {HINCRBYFLOAT against hash key originally set with HSET} {
        r hset smallhash tmp 100
        r hset bighash tmp 100
        list [roundFloat [r hincrbyfloat smallhash tmp 2.5]] \
             [roundFloat [r hincrbyfloat bighash tmp 2.5]]
    } {102.5 102.5}

    test {HINCRBYFLOAT over 32bit value} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrbyfloat smallhash tmp 1] \
             [r hincrbyfloat bighash tmp 1]
    } {17179869185 17179869185}

    test {HINCRBYFLOAT over 32bit value with over 32bit increment} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrbyfloat smallhash tmp 17179869184] \
             [r hincrbyfloat bighash tmp 17179869184]
    } {34359738368 34359738368}

    test {HINCRBYFLOAT fails against hash value with spaces (left)} {
        r hset smallhash str " 11"
        r hset bighash str " 11"
        catch {r hincrbyfloat smallhash str 1} smallerr
        catch {r hincrbyfloat bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR *not*float*" $smallerr]
        lappend rv [string match "ERR *not*float*" $bigerr]
    } {1 1}

    test {HINCRBYFLOAT fails against hash value with spaces (right)} {
        r hset smallhash str "11 "
        r hset bighash str "11 "
        catch {r hincrbyfloat smallhash str 1} smallerr
        catch {r hincrbyfloat bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR *not*float*" $smallerr]
        lappend rv [string match "ERR *not*float*" $bigerr]
    } {1 1}

    test {HINCRBYFLOAT fails against hash value that contains a null-terminator in the middle} {
        r hset h f "1\x002"
        catch {r hincrbyfloat h f 1} err
        set rv {}
        lappend rv [string match "ERR *not*float*" $err]
    } {1}

    test {HSTRLEN against the small hash} {
        set err {}
        foreach k [array names smallhash *] {
            if {[string length $smallhash($k)] ne [r hstrlen smallhash $k]} {
                set err "[string length $smallhash($k)] != [r hstrlen smallhash $k]"
                break
            }
        }
        set _ $err
    } {}

    test {HSTRLEN against the big hash} {
        set err {}
        foreach k [array names bighash *] {
            if {[string length $bighash($k)] ne [r hstrlen bighash $k]} {
                set err "[string length $bighash($k)] != [r hstrlen bighash $k]"
                puts "HSTRLEN and logical length mismatch:"
                puts "key: $k"
                puts "Logical content: $bighash($k)"
                puts "Server  content: [r hget bighash $k]"
            }
        }
        set _ $err
    } {}

    test {HSTRLEN against non existing field} {
        set rv {}
        lappend rv [r hstrlen smallhash __123123123__]
        lappend rv [r hstrlen bighash __123123123__]
        set _ $rv
    } {0 0}

    test {HSTRLEN corner cases} {
        set vals {
            -9223372036854775808 9223372036854775807 9223372036854775808
            {} 0 -1 x
        }
        foreach v $vals {
            r hmset smallhash field $v
            r hmset bighash field $v
            set len1 [string length $v]
            set len2 [r hstrlen smallhash field]
            set len3 [r hstrlen bighash field]
            assert {$len1 == $len2}
            assert {$len2 == $len3}
        }
    }

    test {HINCRBYFLOAT over hash-max-listpack-value encoded with a listpack} {
        set original_max_value [lindex [r config get hash-max-ziplist-value] 1]
        r config set hash-max-listpack-value 8
        
        # hash's value exceeds hash-max-listpack-value
        r del smallhash
        r del bighash
        r hset smallhash tmp 0
        r hset bighash tmp 0
        r hincrbyfloat smallhash tmp 0.000005
        r hincrbyfloat bighash tmp 0.0000005
        assert_encoding listpack smallhash
        assert_encoding hashtable bighash

        # hash's field exceeds hash-max-listpack-value
        r del smallhash
        r del bighash
        r hincrbyfloat smallhash abcdefgh 1
        r hincrbyfloat bighash abcdefghi 1
        assert_encoding listpack smallhash
        assert_encoding hashtable bighash

        r config set hash-max-listpack-value $original_max_value
    }

    test {HGETDEL input validation} {
        r del key1
        assert_error "*wrong number of arguments*" {r hgetdel}
        assert_error "*wrong number of arguments*" {r hgetdel key1}
        assert_error "*wrong number of arguments*" {r hgetdel key1 FIELDS}
        assert_error "*wrong number of arguments*" {r hgetdel key1 FIELDS 0}
        assert_error "*wrong number of arguments*" {r hgetdel key1 FIELDX}
        assert_error "*argument FIELDS is missing*" {r hgetdel key1 XFIELDX 1 a}
        assert_error "*numfields*parameter*must match*number of arguments*" {r hgetdel key1 FIELDS 2 a}
        assert_error "*numfields*parameter*must match*number of arguments*" {r hgetdel key1 FIELDS 2 a b c}
        assert_error "*Number of fields must be a positive integer*" {r hgetdel key1 FIELDS 0 a}
        assert_error "*Number of fields must be a positive integer*" {r hgetdel key1 FIELDS -1 a}
        assert_error "*Number of fields must be a positive integer*" {r hgetdel key1 FIELDS b a}
        assert_error "*Number of fields must be a positive integer*" {r hgetdel key1 FIELDS 9223372036854775808 a}
    }

    foreach type {listpack ht} {
        set orig_config [lindex [r config get hash-max-listpack-entries] 1]
        r del key1

        if {$type == "listpack"} {
            r config set hash-max-listpack-entries $orig_config
            r hset key1 f1 1 f2 2 f3 3 strfield strval
            assert_encoding listpack key1
        } else {
            r config set hash-max-listpack-entries 0
            r hset key1 f1 1 f2 2 f3 3 strfield strval
            assert_encoding hashtable key1
        }

        test {HGETDEL basic test} {
            r del key1
            r hset key1 f1 1 f2 2 f3 3 strfield strval
            assert_equal [r hgetdel key1 fields 1 f2] 2
            assert_equal [r hlen key1] 3
            assert_equal [r hget key1 f1] 1
            assert_equal [r hget key1 f2] ""
            assert_equal [r hget key1 f3] 3
            assert_equal [r hget key1 strfield] strval

            assert_equal [r hgetdel key1 fields 1 f1] 1
            assert_equal [lsort [r hgetall key1]] [lsort "f3 3 strfield strval"]
            assert_equal [r hgetdel key1 fields 1 f3] 3
            assert_equal [r hgetdel key1 fields 1 strfield] strval
            assert_equal [r hgetall key1] ""
            assert_equal [r exists key1] 0
        }

        test {HGETDEL test with non existing fields} {
             r del key1
             r hset key1 f1 1 f2 2 f3 3
             assert_equal [r hgetdel key1 fields 4 x1 x2 x3 x4] "{} {} {} {}"
             assert_equal [r hgetdel key1 fields 4 x1 x2 f3 x4] "{} {} 3 {}"
             assert_equal [lsort [r hgetall key1]] [lsort "f1 1 f2 2"]
             assert_equal [r hgetdel key1 fields 3 f1 f2 f3] "1 2 {}"
             assert_equal [r hgetdel key1 fields 3 f1 f2 f3] "{} {} {}"
        }

        r config set hash-max-listpack-entries $orig_config
    }

    test {HGETDEL propagated as HDEL command to replica} {
        set repl [attach_to_replication_stream]
        r hset key1 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5
        r hgetdel key1 fields 1 f1
        r hgetdel key1 fields 2 f2 f3

        # make sure non-existing fields are not replicated
        r hgetdel key1 fields 2 f7 f8

        # delete more
        r hgetdel key1 fields 3 f4 f5 f6

        assert_replication_stream $repl {
            {select *}
            {hset key1 f1 v1 f2 v2 f3 v3 f4 v4 f5 v5}
            {hdel key1 f1}
            {hdel key1 f2 f3}
            {hdel key1 f4 f5 f6}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {Hash ziplist regression test for large keys} {
        r hset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk a
        r hset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk b
        r hget hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk
    } {b}

    foreach size {10 512} {
        test "Hash fuzzing #1 - $size fields" {
            for {set times 0} {$times < 10} {incr times} {
                catch {unset hash}
                array set hash {}
                r del hash

                # Create
                for {set j 0} {$j < $size} {incr j} {
                    set field [randomValue]
                    set value [randomValue]
                    r hset hash $field $value
                    set hash($field) $value
                }

                # Verify
                foreach {k v} [array get hash] {
                    assert_equal $v [r hget hash $k]
                }
                assert_equal [array size hash] [r hlen hash]
            }
        }

        test "Hash fuzzing #2 - $size fields" {
            for {set times 0} {$times < 10} {incr times} {
                catch {unset hash}
                array set hash {}
                r del hash

                # Create
                for {set j 0} {$j < $size} {incr j} {
                    randpath {
                        set field [randomValue]
                        set value [randomValue]
                        r hset hash $field $value
                        set hash($field) $value
                    } {
                        set field [randomSignedInt 512]
                        set value [randomSignedInt 512]
                        r hset hash $field $value
                        set hash($field) $value
                    } {
                        randpath {
                            set field [randomValue]
                        } {
                            set field [randomSignedInt 512]
                        }
                        r hdel hash $field
                        unset -nocomplain hash($field)
                    }
                }

                # Verify
                foreach {k v} [array get hash] {
                    assert_equal $v [r hget hash $k]
                }
                assert_equal [array size hash] [r hlen hash]
            }
        }
    }

    test {Stress test the hash ziplist -> hashtable encoding conversion} {
        r config set hash-max-ziplist-entries 32
        for {set j 0} {$j < 100} {incr j} {
            r del myhash
            for {set i 0} {$i < 64} {incr i} {
                r hset myhash [randomValue] [randomValue]
            }
            assert_encoding hashtable myhash
        }
    }

    # The following test can only be executed if we don't use Valgrind, and if
    # we are using x86_64 architecture, because:
    #
    # 1) Valgrind has floating point limitations, no support for 80 bits math.
    # 2) Other archs may have the same limits.
    #
    # 1.23 cannot be represented correctly with 64 bit doubles, so we skip
    # the test, since we are only testing pretty printing here and is not
    # a bug if the program outputs things like 1.299999...
    if {!$::valgrind && [string match *x86_64* [exec uname -a]]} {
        test {Test HINCRBYFLOAT for correct float representation (issue #2846)} {
            r del myhash
            assert {[r hincrbyfloat myhash float 1.23] eq {1.23}}
            assert {[r hincrbyfloat myhash float 0.77] eq {2}}
            assert {[r hincrbyfloat myhash float -0.1] eq {1.9}}
        }
    }

    test {Hash ziplist of various encodings} {
        r del k
        config_set hash-max-ziplist-entries 1000000000
        config_set hash-max-ziplist-value 1000000000
        r hset k ZIP_INT_8B 127
        r hset k ZIP_INT_16B 32767
        r hset k ZIP_INT_32B 2147483647
        r hset k ZIP_INT_64B 9223372036854775808
        r hset k ZIP_INT_IMM_MIN 0
        r hset k ZIP_INT_IMM_MAX 12
        r hset k ZIP_STR_06B [string repeat x 31]
        r hset k ZIP_STR_14B [string repeat x 8191]
        r hset k ZIP_STR_32B [string repeat x 65535]
        set k [r hgetall k]
        set dump [r dump k]

        # will be converted to dict at RESTORE
        config_set hash-max-ziplist-entries 2
        config_set sanitize-dump-payload no mayfail
        r restore kk 0 $dump
        set kk [r hgetall kk]

        # make sure the values are right
        assert_equal [lsort $k] [lsort $kk]
        assert_equal [dict get $k ZIP_STR_06B] [string repeat x 31]
        set k [dict remove $k ZIP_STR_06B]
        assert_equal [dict get $k ZIP_STR_14B] [string repeat x 8191]
        set k [dict remove $k ZIP_STR_14B]
        assert_equal [dict get $k ZIP_STR_32B] [string repeat x 65535]
        set k [dict remove $k ZIP_STR_32B]
        set _ $k
    } {ZIP_INT_8B 127 ZIP_INT_16B 32767 ZIP_INT_32B 2147483647 ZIP_INT_64B 9223372036854775808 ZIP_INT_IMM_MIN 0 ZIP_INT_IMM_MAX 12}

    test {Hash ziplist of various encodings - sanitize dump} {
        config_set sanitize-dump-payload yes mayfail
        r restore kk 0 $dump replace
        set k [r hgetall k]
        set kk [r hgetall kk]

        # make sure the values are right
        assert_equal [lsort $k] [lsort $kk]
        assert_equal [dict get $k ZIP_STR_06B] [string repeat x 31]
        set k [dict remove $k ZIP_STR_06B]
        assert_equal [dict get $k ZIP_STR_14B] [string repeat x 8191]
        set k [dict remove $k ZIP_STR_14B]
        assert_equal [dict get $k ZIP_STR_32B] [string repeat x 65535]
        set k [dict remove $k ZIP_STR_32B]
        set _ $k
    } {ZIP_INT_8B 127 ZIP_INT_16B 32767 ZIP_INT_32B 2147483647 ZIP_INT_64B 9223372036854775808 ZIP_INT_IMM_MIN 0 ZIP_INT_IMM_MAX 12}

    # On some platforms strtold("+inf") with valgrind returns a non-inf result
    if {!$::valgrind} {
        test {HINCRBYFLOAT does not allow NaN or Infinity} {
            assert_error "*value is NaN or Infinity*" {r hincrbyfloat hfoo field +inf}
            assert_equal 0 [r exists hfoo]
        }
    }
}
