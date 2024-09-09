start_server {
    tags {"set"}
    overrides {
        "set-max-intset-entries" 512
        "set-max-listpack-entries" 128
        "set-max-listpack-value" 32
    }
} {
    proc create_set {key entries} {
        r del $key
        foreach entry $entries { r sadd $key $entry }
    }

    # Values for initialing sets, per encoding.
    array set initelems {listpack {foo} hashtable {foo}}
    for {set i 0} {$i < 130} {incr i} {
        lappend initelems(hashtable) [format "i%03d" $i]
    }

    foreach type {listpack hashtable} {
    test "SADD, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS basics - $type" {
        create_set myset $initelems($type)
        assert_encoding $type myset
        assert_equal 1 [r sadd myset bar]
        assert_equal 0 [r sadd myset bar]
        assert_equal [expr [llength $initelems($type)] + 1] [r scard myset]
        assert_equal 1 [r sismember myset foo]
        assert_equal 1 [r sismember myset bar]
        assert_equal 0 [r sismember myset bla]
        assert_equal {1} [r smismember myset foo]
        assert_equal {1 1} [r smismember myset foo bar]
        assert_equal {1 0} [r smismember myset foo bla]
        assert_equal {0 1} [r smismember myset bla foo]
        assert_equal {0} [r smismember myset bla]
        assert_equal "bar $initelems($type)" [lsort [r smembers myset]]
    }
    }

    test {SADD, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS basics - intset} {
        create_set myset {17}
        assert_encoding intset myset
        assert_equal 1 [r sadd myset 16]
        assert_equal 0 [r sadd myset 16]
        assert_equal 2 [r scard myset]
        assert_equal 1 [r sismember myset 16]
        assert_equal 1 [r sismember myset 17]
        assert_equal 0 [r sismember myset 18]
        assert_equal {1} [r smismember myset 16]
        assert_equal {1 1} [r smismember myset 16 17]
        assert_equal {1 0} [r smismember myset 16 18]
        assert_equal {0 1} [r smismember myset 18 16]
        assert_equal {0} [r smismember myset 18]
        assert_equal {16 17} [lsort [r smembers myset]]
    }

    test {SMISMEMBER SMEMBERS SCARD against non set} {
        r lpush mylist foo
        assert_error WRONGTYPE* {r smismember mylist bar}
        assert_error WRONGTYPE* {r smembers mylist}
        assert_error WRONGTYPE* {r scard mylist}
    }

    test {SMISMEMBER SMEMBERS SCARD against non existing key} {
        assert_equal {0} [r smismember myset1 foo]
        assert_equal {0 0} [r smismember myset1 foo bar]
        assert_equal {} [r smembers myset1]
        assert_equal {0} [r scard myset1]
    }

    test {SMISMEMBER requires one or more members} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        r zadd zmscoretest 20 y
        
        catch {r smismember zmscoretest} e
        assert_match {*ERR*wrong*number*arg*} $e
    }

    test {SADD against non set} {
        r lpush mylist foo
        assert_error WRONGTYPE* {r sadd mylist bar}
    }

    test "SADD a non-integer against a small intset" {
        create_set myset {1 2 3}
        assert_encoding intset myset
        assert_equal 1 [r sadd myset a]
        assert_encoding listpack myset
    }

    test "SADD a non-integer against a large intset" {
        create_set myset {0}
        for {set i 1} {$i < 130} {incr i} {r sadd myset $i}
        assert_encoding intset myset
        assert_equal 1 [r sadd myset a]
        assert_encoding hashtable myset
    }

    test "SADD an integer larger than 64 bits" {
        create_set myset {213244124402402314402033402}
        assert_encoding listpack myset
        assert_equal 1 [r sismember myset 213244124402402314402033402]
        assert_equal {1} [r smismember myset 213244124402402314402033402]
    }

    test "SADD an integer larger than 64 bits to a large intset" {
        create_set myset {0}
        for {set i 1} {$i < 130} {incr i} {r sadd myset $i}
        assert_encoding intset myset
        r sadd myset 213244124402402314402033402
        assert_encoding hashtable myset
        assert_equal 1 [r sismember myset 213244124402402314402033402]
        assert_equal {1} [r smismember myset 213244124402402314402033402]
    }

foreach type {single multiple single_multiple} {
    test "SADD overflows the maximum allowed integers in an intset - $type" {
        r del myset

        if {$type == "single"} {
            # All are single sadd commands.
            for {set i 0} {$i < 512} {incr i} { r sadd myset $i }
        } elseif {$type == "multiple"} {
            # One sadd command to add all elements.
            set args {}
            for {set i 0} {$i < 512} {incr i} { lappend args $i }
            r sadd myset {*}$args
        } elseif {$type == "single_multiple"} {
            # First one sadd adds an element (creates a key) and then one sadd adds all elements.
            r sadd myset 1
            set args {}
            for {set i 0} {$i < 512} {incr i} { lappend args $i }
            r sadd myset {*}$args
        }

        assert_encoding intset myset
        assert_equal 512 [r scard myset]
        assert_equal 1 [r sadd myset 512]
        assert_encoding hashtable myset
    }

    test "SADD overflows the maximum allowed elements in a listpack - $type" {
        r del myset

        if {$type == "single"} {
            # All are single sadd commands.
            r sadd myset a
            for {set i 0} {$i < 127} {incr i} { r sadd myset $i }
        } elseif {$type == "multiple"} {
            # One sadd command to add all elements.
            set args {}
            lappend args a
            for {set i 0} {$i < 127} {incr i} { lappend args $i }
            r sadd myset {*}$args
        } elseif {$type == "single_multiple"} {
            # First one sadd adds an element (creates a key) and then one sadd adds all elements.
            r sadd myset a
            set args {}
            lappend args a
            for {set i 0} {$i < 127} {incr i} { lappend args $i }
            r sadd myset {*}$args
        }

        assert_encoding listpack myset
        assert_equal 128 [r scard myset]
        assert_equal 1 [r sadd myset b]
        assert_encoding hashtable myset
    }
}

    test {Variadic SADD} {
        r del myset
        assert_equal 3 [r sadd myset a b c]
        assert_equal 2 [r sadd myset A a b c B]
        assert_equal [lsort {A a b c B}] [lsort [r smembers myset]]
    }

    test "Set encoding after DEBUG RELOAD" {
        r del myintset
        r del myhashset
        r del mylargeintset
        r del mysmallset
        for {set i 0} {$i <  100} {incr i} { r sadd myintset $i }
        for {set i 0} {$i < 1280} {incr i} { r sadd mylargeintset $i }
        for {set i 0} {$i <   50} {incr i} { r sadd mysmallset [format "i%03d" $i] }
        for {set i 0} {$i <  256} {incr i} { r sadd myhashset [format "i%03d" $i] }
        assert_encoding intset myintset
        assert_encoding hashtable mylargeintset
        assert_encoding listpack mysmallset
        assert_encoding hashtable myhashset

        r debug reload
        assert_encoding intset myintset
        assert_encoding hashtable mylargeintset
        assert_encoding listpack mysmallset
        assert_encoding hashtable myhashset
    } {} {needs:debug}

    foreach type {listpack hashtable} {
        test {SREM basics - $type} {
            create_set myset $initelems($type)
            r sadd myset ciao
            assert_encoding $type myset
            assert_equal 0 [r srem myset qux]
            assert_equal 1 [r srem myset ciao]
            assert_equal $initelems($type) [lsort [r smembers myset]]
        }
    }

    test {SREM basics - intset} {
        create_set myset {3 4 5}
        assert_encoding intset myset
        assert_equal 0 [r srem myset 6]
        assert_equal 1 [r srem myset 4]
        assert_equal {3 5} [lsort [r smembers myset]]
    }

    test {SREM with multiple arguments} {
        r del myset
        r sadd myset a b c d
        assert_equal 0 [r srem myset k k k]
        assert_equal 2 [r srem myset b d x y]
        lsort [r smembers myset]
    } {a c}

    test {SREM variadic version with more args needed to destroy the key} {
        r del myset
        r sadd myset 1 2 3
        r srem myset 1 2 3 4 5 6 7 8
    } {3}

    test "SINTERCARD with illegal arguments" {
        assert_error "ERR wrong number of arguments for 'sintercard' command" {r sintercard}
        assert_error "ERR wrong number of arguments for 'sintercard' command" {r sintercard 1}

        assert_error "ERR numkeys*" {r sintercard 0 myset{t}}
        assert_error "ERR numkeys*" {r sintercard a myset{t}}

        assert_error "ERR Number of keys*" {r sintercard 2 myset{t}}
        assert_error "ERR Number of keys*" {r sintercard 3 myset{t} myset2{t}}

        assert_error "ERR syntax error*" {r sintercard 1 myset{t} myset2{t}}
        assert_error "ERR syntax error*" {r sintercard 1 myset{t} bar_arg}
        assert_error "ERR syntax error*" {r sintercard 1 myset{t} LIMIT}

        assert_error "ERR LIMIT*" {r sintercard 1 myset{t} LIMIT -1}
        assert_error "ERR LIMIT*" {r sintercard 1 myset{t} LIMIT a}
    }

    test "SINTERCARD against non-set should throw error" {
        r del set{t}
        r sadd set{t} a b c
        r set key1{t} x

        assert_error "WRONGTYPE*" {r sintercard 1 key1{t}}
        assert_error "WRONGTYPE*" {r sintercard 2 set{t} key1{t}}
        assert_error "WRONGTYPE*" {r sintercard 2 key1{t} noset{t}}
    }

    test "SINTERCARD against non-existing key" {
        assert_equal 0 [r sintercard 1 non-existing-key]
        assert_equal 0 [r sintercard 1 non-existing-key limit 0]
        assert_equal 0 [r sintercard 1 non-existing-key limit 10]
    }

    foreach {type} {regular intset} {
        # Create sets setN{t} where N = 1..5
        if {$type eq "regular"} {
            set smallenc listpack
            set bigenc hashtable
        } else {
            set smallenc intset
            set bigenc intset
        }
        # Sets 1, 2 and 4 are big; sets 3 and 5 are small.
        array set encoding "1 $bigenc 2 $bigenc 3 $smallenc 4 $bigenc 5 $smallenc"

        for {set i 1} {$i <= 5} {incr i} {
            r del [format "set%d{t}" $i]
        }
        for {set i 0} {$i < 200} {incr i} {
            r sadd set1{t} $i
            r sadd set2{t} [expr $i+195]
        }
        foreach i {199 195 1000 2000} {
            r sadd set3{t} $i
        }
        for {set i 5} {$i < 200} {incr i} {
            r sadd set4{t} $i
        }
        r sadd set5{t} 0

        # To make sure the sets are encoded as the type we are testing -- also
        # when the VM is enabled and the values may be swapped in and out
        # while the tests are running -- an extra element is added to every
        # set that determines its encoding.
        set large 200
        if {$type eq "regular"} {
            set large foo
        }

        for {set i 1} {$i <= 5} {incr i} {
            r sadd [format "set%d{t}" $i] $large
        }

        test "Generated sets must be encoded correctly - $type" {
            for {set i 1} {$i <= 5} {incr i} {
                assert_encoding $encoding($i) [format "set%d{t}" $i]
            }
        }

        test "SINTER with two sets - $type" {
            assert_equal [list 195 196 197 198 199 $large] [lsort [r sinter set1{t} set2{t}]]
        }

        test "SINTERCARD with two sets - $type" {
            assert_equal 6 [r sintercard 2 set1{t} set2{t}]
            assert_equal 6 [r sintercard 2 set1{t} set2{t} limit 0]
            assert_equal 3 [r sintercard 2 set1{t} set2{t} limit 3]
            assert_equal 6 [r sintercard 2 set1{t} set2{t} limit 10]
        }

        test "SINTERSTORE with two sets - $type" {
            r sinterstore setres{t} set1{t} set2{t}
            assert_encoding $smallenc setres{t}
            assert_equal [list 195 196 197 198 199 $large] [lsort [r smembers setres{t}]]
        }

        test "SINTERSTORE with two sets, after a DEBUG RELOAD - $type" {
            r debug reload
            r sinterstore setres{t} set1{t} set2{t}
            assert_encoding $smallenc setres{t}
            assert_equal [list 195 196 197 198 199 $large] [lsort [r smembers setres{t}]]
        } {} {needs:debug}

        test "SUNION with two sets - $type" {
            set expected [lsort -uniq "[r smembers set1{t}] [r smembers set2{t}]"]
            assert_equal $expected [lsort [r sunion set1{t} set2{t}]]
        }

        test "SUNIONSTORE with two sets - $type" {
            r sunionstore setres{t} set1{t} set2{t}
            assert_encoding $bigenc setres{t}
            set expected [lsort -uniq "[r smembers set1{t}] [r smembers set2{t}]"]
            assert_equal $expected [lsort [r smembers setres{t}]]
        }

        test "SINTER against three sets - $type" {
            assert_equal [list 195 199 $large] [lsort [r sinter set1{t} set2{t} set3{t}]]
        }

        test "SINTERCARD against three sets - $type" {
            assert_equal 3 [r sintercard 3 set1{t} set2{t} set3{t}]
            assert_equal 3 [r sintercard 3 set1{t} set2{t} set3{t} limit 0]
            assert_equal 2 [r sintercard 3 set1{t} set2{t} set3{t} limit 2]
            assert_equal 3 [r sintercard 3 set1{t} set2{t} set3{t} limit 10]
        }

        test "SINTERSTORE with three sets - $type" {
            r sinterstore setres{t} set1{t} set2{t} set3{t}
            assert_equal [list 195 199 $large] [lsort [r smembers setres{t}]]
        }

        test "SUNION with non existing keys - $type" {
            set expected [lsort -uniq "[r smembers set1{t}] [r smembers set2{t}]"]
            assert_equal $expected [lsort [r sunion nokey1{t} set1{t} set2{t} nokey2{t}]]
        }

        test "SDIFF with two sets - $type" {
            assert_equal {0 1 2 3 4} [lsort [r sdiff set1{t} set4{t}]]
        }

        test "SDIFF with three sets - $type" {
            assert_equal {1 2 3 4} [lsort [r sdiff set1{t} set4{t} set5{t}]]
        }

        test "SDIFFSTORE with three sets - $type" {
            r sdiffstore setres{t} set1{t} set4{t} set5{t}
            # When we start with intsets, we should always end with intsets.
            if {$type eq {intset}} {
                assert_encoding intset setres{t}
            }
            assert_equal {1 2 3 4} [lsort [r smembers setres{t}]]
        }

        test "SINTER/SUNION/SDIFF with three same sets - $type" {
            set expected [lsort "[r smembers set1{t}]"]
            assert_equal $expected [lsort [r sinter set1{t} set1{t} set1{t}]]
            assert_equal $expected [lsort [r sunion set1{t} set1{t} set1{t}]]
            assert_equal {} [lsort [r sdiff set1{t} set1{t} set1{t}]]
        }
    }

    test "SINTERSTORE with two listpack sets where result is intset" {
        r del setres{t} set1{t} set2{t}
        r sadd set1{t} a b c 1 3 6 x y z
        r sadd set2{t} e f g 1 2 3 u v w
        assert_encoding listpack set1{t}
        assert_encoding listpack set2{t}
        r sinterstore setres{t} set1{t} set2{t}
        assert_equal [list 1 3] [lsort [r smembers setres{t}]]
        assert_encoding intset setres{t}
    }

    test "SINTERSTORE with two hashtable sets where result is intset" {
        r del setres{t} set1{t} set2{t}
        r sadd set1{t} a b c 444 555 666
        r sadd set2{t} e f g 111 222 333
        set expected {}
        for {set i 1} {$i < 130} {incr i} {
            r sadd set1{t} $i
            r sadd set2{t} $i
            lappend expected $i
        }
        assert_encoding hashtable set1{t}
        assert_encoding hashtable set2{t}
        r sinterstore setres{t} set1{t} set2{t}
        assert_equal [lsort $expected] [lsort [r smembers setres{t}]]
        assert_encoding intset setres{t}
    }

    test "SUNION hashtable and listpack" {
        # This adds code coverage for adding a non-sds string to a hashtable set
        # which already contains the string.
        r del set1{t} set2{t}
        set union {abcdefghijklmnopqrstuvwxyz1234567890 a b c 1 2 3}
        create_set set1{t} $union
        create_set set2{t} {a b c}
        assert_encoding hashtable set1{t}
        assert_encoding listpack set2{t}
        assert_equal [lsort $union] [lsort [r sunion set1{t} set2{t}]]
    }

    test "SDIFF with first set empty" {
        r del set1{t} set2{t} set3{t}
        r sadd set2{t} 1 2 3 4
        r sadd set3{t} a b c d
        r sdiff set1{t} set2{t} set3{t}
    } {}

    test "SDIFF with same set two times" {
        r del set1
        r sadd set1 a b c 1 2 3 4 5 6
        r sdiff set1 set1
    } {}

    test "SDIFF fuzzing" {
        for {set j 0} {$j < 100} {incr j} {
            unset -nocomplain s
            array set s {}
            set args {}
            set num_sets [expr {[randomInt 10]+1}]
            for {set i 0} {$i < $num_sets} {incr i} {
                set num_elements [randomInt 100]
                r del set_$i{t}
                lappend args set_$i{t}
                while {$num_elements} {
                    set ele [randomValue]
                    r sadd set_$i{t} $ele
                    if {$i == 0} {
                        set s($ele) x
                    } else {
                        unset -nocomplain s($ele)
                    }
                    incr num_elements -1
                }
            }
            set result [lsort [r sdiff {*}$args]]
            assert_equal $result [lsort [array names s]]
        }
    }

    test "SDIFF against non-set should throw error" {
        # with an empty set
        r set key1{t} x
        assert_error "WRONGTYPE*" {r sdiff key1{t} noset{t}}
        # different order
        assert_error "WRONGTYPE*" {r sdiff noset{t} key1{t}}

        # with a legal set
        r del set1{t}
        r sadd set1{t} a b c
        assert_error "WRONGTYPE*" {r sdiff key1{t} set1{t}}
        # different order
        assert_error "WRONGTYPE*" {r sdiff set1{t} key1{t}}
    }

    test "SDIFF should handle non existing key as empty" {
        r del set1{t} set2{t} set3{t}

        r sadd set1{t} a b c
        r sadd set2{t} b c d
        assert_equal {a} [lsort [r sdiff set1{t} set2{t} set3{t}]]
        assert_equal {} [lsort [r sdiff set3{t} set2{t} set1{t}]]
    }

    test "SDIFFSTORE against non-set should throw error" {
        r del set1{t} set2{t} set3{t} key1{t}
        r set key1{t} x

        # with en empty dstkey
        assert_error "WRONGTYPE*" {r SDIFFSTORE set3{t} key1{t} noset{t}}
        assert_equal 0 [r exists set3{t}]
        assert_error "WRONGTYPE*" {r SDIFFSTORE set3{t} noset{t} key1{t}}
        assert_equal 0 [r exists set3{t}]

        # with a legal dstkey
        r sadd set1{t} a b c
        r sadd set2{t} b c d
        r sadd set3{t} e
        assert_error "WRONGTYPE*" {r SDIFFSTORE set3{t} key1{t} set1{t} noset{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]

        assert_error "WRONGTYPE*" {r SDIFFSTORE set3{t} set1{t} key1{t} set2{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]
    }

    test "SDIFFSTORE should handle non existing key as empty" {
        r del set1{t} set2{t} set3{t}

        r set setres{t} xxx
        assert_equal 0 [r sdiffstore setres{t} foo111{t} bar222{t}]
        assert_equal 0 [r exists setres{t}]

        # with a legal dstkey, should delete dstkey
        r sadd set3{t} a b c
        assert_equal 0 [r sdiffstore set3{t} set1{t} set2{t}]
        assert_equal 0 [r exists set3{t}]

        r sadd set1{t} a b c
        assert_equal 3 [r sdiffstore set3{t} set1{t} set2{t}]
        assert_equal 1 [r exists set3{t}]
        assert_equal {a b c} [lsort [r smembers set3{t}]]

        # with a legal dstkey and empty set2, should delete the dstkey
        r sadd set3{t} a b c
        assert_equal 0 [r sdiffstore set3{t} set2{t} set1{t}]
        assert_equal 0 [r exists set3{t}]
    }

    test "SINTER against non-set should throw error" {
        r set key1{t} x
        assert_error "WRONGTYPE*" {r sinter key1{t} noset{t}}
        # different order
        assert_error "WRONGTYPE*" {r sinter noset{t} key1{t}}

        r sadd set1{t} a b c
        assert_error "WRONGTYPE*" {r sinter key1{t} set1{t}}
        # different order
        assert_error "WRONGTYPE*" {r sinter set1{t} key1{t}}
    }

    test "SINTER should handle non existing key as empty" {
        r del set1{t} set2{t} set3{t}
        r sadd set1{t} a b c
        r sadd set2{t} b c d
        r sinter set1{t} set2{t} set3{t}
    } {}

    test "SINTER with same integer elements but different encoding" {
        r del set1{t} set2{t}
        r sadd set1{t} 1 2 3
        r sadd set2{t} 1 2 3 a
        r srem set2{t} a
        assert_encoding intset set1{t}
        assert_encoding listpack set2{t}
        lsort [r sinter set1{t} set2{t}]
    } {1 2 3}

    test "SINTERSTORE against non-set should throw error" {
        r del set1{t} set2{t} set3{t} key1{t}
        r set key1{t} x

        # with en empty dstkey
        assert_error "WRONGTYPE*" {r sinterstore set3{t} key1{t} noset{t}}
        assert_equal 0 [r exists set3{t}]
        assert_error "WRONGTYPE*" {r sinterstore set3{t} noset{t} key1{t}}
        assert_equal 0 [r exists set3{t}]

        # with a legal dstkey
        r sadd set1{t} a b c
        r sadd set2{t} b c d
        r sadd set3{t} e
        assert_error "WRONGTYPE*" {r sinterstore set3{t} key1{t} set2{t} noset{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]

        assert_error "WRONGTYPE*" {r sinterstore set3{t} noset{t} key1{t} set2{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]
    }

    test "SINTERSTORE against non existing keys should delete dstkey" {
        r del set1{t} set2{t} set3{t}

        r set setres{t} xxx
        assert_equal 0 [r sinterstore setres{t} foo111{t} bar222{t}]
        assert_equal 0 [r exists setres{t}]

        # with a legal dstkey
        r sadd set3{t} a b c
        assert_equal 0 [r sinterstore set3{t} set1{t} set2{t}]
        assert_equal 0 [r exists set3{t}]

        r sadd set1{t} a b c
        assert_equal 0 [r sinterstore set3{t} set1{t} set2{t}]
        assert_equal 0 [r exists set3{t}]

        assert_equal 0 [r sinterstore set3{t} set2{t} set1{t}]
        assert_equal 0 [r exists set3{t}]
    }

    test "SUNION against non-set should throw error" {
        r set key1{t} x
        assert_error "WRONGTYPE*" {r sunion key1{t} noset{t}}
        # different order
        assert_error "WRONGTYPE*" {r sunion noset{t} key1{t}}

        r del set1{t}
        r sadd set1{t} a b c
        assert_error "WRONGTYPE*" {r sunion key1{t} set1{t}}
        # different order
        assert_error "WRONGTYPE*" {r sunion set1{t} key1{t}}
    }

    test "SUNION should handle non existing key as empty" {
        r del set1{t} set2{t} set3{t}

        r sadd set1{t} a b c
        r sadd set2{t} b c d
        assert_equal {a b c d} [lsort [r sunion set1{t} set2{t} set3{t}]]
    }

    test "SUNIONSTORE against non-set should throw error" {
        r del set1{t} set2{t} set3{t} key1{t}
        r set key1{t} x

        # with en empty dstkey
        assert_error "WRONGTYPE*" {r sunionstore set3{t} key1{t} noset{t}}
        assert_equal 0 [r exists set3{t}]
        assert_error "WRONGTYPE*" {r sunionstore set3{t} noset{t} key1{t}}
        assert_equal 0 [r exists set3{t}]

        # with a legal dstkey
        r sadd set1{t} a b c
        r sadd set2{t} b c d
        r sadd set3{t} e
        assert_error "WRONGTYPE*" {r sunionstore set3{t} key1{t} key2{t} noset{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]

        assert_error "WRONGTYPE*" {r sunionstore set3{t} noset{t} key1{t} key2{t}}
        assert_equal 1 [r exists set3{t}]
        assert_equal {e} [lsort [r smembers set3{t}]]
    }

    test "SUNIONSTORE should handle non existing key as empty" {
        r del set1{t} set2{t} set3{t}

        r set setres{t} xxx
        assert_equal 0 [r sunionstore setres{t} foo111{t} bar222{t}]
        assert_equal 0 [r exists setres{t}]

        # set1 set2 both empty, should delete the dstkey
        r sadd set3{t} a b c
        assert_equal 0 [r sunionstore set3{t} set1{t} set2{t}]
        assert_equal 0 [r exists set3{t}]

        r sadd set1{t} a b c
        r sadd set3{t} e f
        assert_equal 3 [r sunionstore set3{t} set1{t} set2{t}]
        assert_equal 1 [r exists set3{t}]
        assert_equal {a b c} [lsort [r smembers set3{t}]]

        r sadd set3{t} d
        assert_equal 3 [r sunionstore set3{t} set2{t} set1{t}]
        assert_equal 1 [r exists set3{t}]
        assert_equal {a b c} [lsort [r smembers set3{t}]]
    }

    test "SUNIONSTORE against non existing keys should delete dstkey" {
        r set setres{t} xxx
        assert_equal 0 [r sunionstore setres{t} foo111{t} bar222{t}]
        assert_equal 0 [r exists setres{t}]
    }

    foreach {type contents} {listpack {a b c} intset {1 2 3}} {
        test "SPOP basics - $type" {
            create_set myset $contents
            assert_encoding $type myset
            assert_equal $contents [lsort [list [r spop myset] [r spop myset] [r spop myset]]]
            assert_equal 0 [r scard myset]
        }

        test "SPOP with <count>=1 - $type" {
            create_set myset $contents
            assert_encoding $type myset
            assert_equal $contents [lsort [list [r spop myset 1] [r spop myset 1] [r spop myset 1]]]
            assert_equal 0 [r scard myset]
        }

        test "SRANDMEMBER - $type" {
            create_set myset $contents
            unset -nocomplain myset
            array set myset {}
            for {set i 0} {$i < 100} {incr i} {
                set myset([r srandmember myset]) 1
            }
            assert_equal $contents [lsort [array names myset]]
        }
    }

    test "SPOP integer from listpack set" {
        create_set myset {a 1 2 3 4 5 6 7}
        assert_encoding listpack myset
        set a [r spop myset]
        set b [r spop myset]
        assert {[string is digit $a] || [string is digit $b]}
    }

    foreach {type contents} {
        listpack {a b c d e f g h i j k l m n o p q r s t u v w x y z}
        intset {1 10 11 12 13 14 15 16 17 18 19 2 20 21 22 23 24 25 26 3 4 5 6 7 8 9}
        hashtable {ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 b c d e f g h i j k l m n o p q r s t u v w x y z}
    } {
        test "SPOP with <count> - $type" {
            create_set myset $contents
            assert_encoding $type myset
            assert_equal $contents [lsort [concat [r spop myset 11] [r spop myset 9] [r spop myset 0] [r spop myset 4] [r spop myset 1] [r spop myset 0] [r spop myset 1] [r spop myset 0]]]
            assert_equal 0 [r scard myset]
        }
    }

    # As seen in intsetRandomMembers
    test "SPOP using integers, testing Knuth's and Floyd's algorithm" {
        create_set myset {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
        assert_encoding intset myset
        assert_equal 20 [r scard myset]
        r spop myset 1
        assert_equal 19 [r scard myset]
        r spop myset 2
        assert_equal 17 [r scard myset]
        r spop myset 3
        assert_equal 14 [r scard myset]
        r spop myset 10
        assert_equal 4 [r scard myset]
        r spop myset 10
        assert_equal 0 [r scard myset]
        r spop myset 1
        assert_equal 0 [r scard myset]
    } {}

    test "SPOP using integers with Knuth's algorithm" {
        r spop nonexisting_key 100
    } {}

    foreach {type content} {
        intset   {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
        listpack {a 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
    } {
    test "SPOP new implementation: code path #1 $type" {
        create_set myset $content
        assert_encoding $type myset
        set res [r spop myset 30]
        assert {[lsort $content] eq [lsort $res]}
        assert_equal {0} [r exists myset]
    }

    test "SPOP new implementation: code path #2 $type" {
        create_set myset $content
        assert_encoding $type myset
        set res [r spop myset 2]
        assert {[llength $res] == 2}
        assert {[r scard myset] == 18}
        set union [concat [r smembers myset] $res]
        assert {[lsort $union] eq [lsort $content]}
    }

    test "SPOP new implementation: code path #3 $type" {
        create_set myset $content
        assert_encoding $type myset
        set res [r spop myset 18]
        assert {[llength $res] == 18}
        assert {[r scard myset] == 2}
        set union [concat [r smembers myset] $res]
        assert {[lsort $union] eq [lsort $content]}
    }
    }

    test "SPOP new implementation: code path #1 propagate as DEL or UNLINK" {
        r del myset1{t} myset2{t}
        r sadd myset1{t} 1 2 3 4 5
        r sadd myset2{t} 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65

        set repl [attach_to_replication_stream]

        r config set lazyfree-lazy-server-del no
        r spop myset1{t} [r scard myset1{t}]
        r config set lazyfree-lazy-server-del yes
        r spop myset2{t} [r scard myset2{t}]
        assert_equal {0} [r exists myset1{t} myset2{t}]

        # Verify the propagate of DEL and UNLINK.
        assert_replication_stream $repl {
            {select *}
            {del myset1{t}}
            {unlink myset2{t}}
        }

        close_replication_stream $repl
    } {} {needs:repl}

    test "SRANDMEMBER count of 0 is handled correctly" {
        r srandmember myset 0
    } {}

    test "SRANDMEMBER with <count> against non existing key" {
        r srandmember nonexisting_key 100
    } {}

    test "SRANDMEMBER count overflow" {
        r sadd myset a
        assert_error {*value is out of range*} {r srandmember myset -9223372036854775808}
    } {}

    # Make sure we can distinguish between an empty array and a null response
    r readraw 1

    test "SRANDMEMBER count of 0 is handled correctly - emptyarray" {
        r srandmember myset 0
    } {*0}

    test "SRANDMEMBER with <count> against non existing key - emptyarray" {
        r srandmember nonexisting_key 100
    } {*0}

    r readraw 0

    foreach {type contents} {
        listpack {
            1 5 10 50 125 50000 33959417 4775547 65434162
            12098459 427716 483706 2726473884 72615637475
            MARY PATRICIA LINDA BARBARA ELIZABETH JENNIFER MARIA
            SUSAN MARGARET DOROTHY LISA NANCY KAREN BETTY HELEN
            SANDRA DONNA CAROL RUTH SHARON MICHELLE LAURA SARAH
            KIMBERLY DEBORAH JESSICA SHIRLEY CYNTHIA ANGELA MELISSA
            BRENDA AMY ANNA REBECCA VIRGINIA KATHLEEN
        }
        intset {
            0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
            20 21 22 23 24 25 26 27 28 29
            30 31 32 33 34 35 36 37 38 39
            40 41 42 43 44 45 46 47 48 49
        }
        hashtable {
            ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
            1 5 10 50 125 50000 33959417 4775547 65434162
            12098459 427716 483706 2726473884 72615637475
            MARY PATRICIA LINDA BARBARA ELIZABETH JENNIFER MARIA
            SUSAN MARGARET DOROTHY LISA NANCY KAREN BETTY HELEN
            SANDRA DONNA CAROL RUTH SHARON MICHELLE LAURA SARAH
            KIMBERLY DEBORAH JESSICA SHIRLEY CYNTHIA ANGELA MELISSA
            BRENDA AMY ANNA REBECCA VIRGINIA
        }
    } {
        test "SRANDMEMBER with <count> - $type" {
            create_set myset $contents
            assert_encoding $type myset
            unset -nocomplain myset
            array set myset {}
            foreach ele [r smembers myset] {
                set myset($ele) 1
            }
            assert_equal [lsort $contents] [lsort [array names myset]]

            # Make sure that a count of 0 is handled correctly.
            assert_equal [r srandmember myset 0] {}

            # We'll stress different parts of the code, see the implementation
            # of SRANDMEMBER for more information, but basically there are
            # four different code paths.
            #
            # PATH 1: Use negative count.
            #
            # 1) Check that it returns repeated elements.
            set res [r srandmember myset -100]
            assert_equal [llength $res] 100

            # 2) Check that all the elements actually belong to the
            # original set.
            foreach ele $res {
                assert {[info exists myset($ele)]}
            }

            # 3) Check that eventually all the elements are returned.
            unset -nocomplain auxset
            set iterations 1000
            while {$iterations != 0} {
                incr iterations -1
                set res [r srandmember myset -10]
                foreach ele $res {
                    set auxset($ele) 1
                }
                if {[lsort [array names myset]] eq
                    [lsort [array names auxset]]} {
                    break;
                }
            }
            assert {$iterations != 0}

            # PATH 2: positive count (unique behavior) with requested size
            # equal or greater than set size.
            foreach size {50 100} {
                set res [r srandmember myset $size]
                assert_equal [llength $res] 50
                assert_equal [lsort $res] [lsort [array names myset]]
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

            foreach size {45 5} {
                set res [r srandmember myset $size]
                assert_equal [llength $res] $size

                # 1) Check that all the elements actually belong to the
                # original set.
                foreach ele $res {
                    assert {[info exists myset($ele)]}
                }

                # 2) Check that eventually all the elements are returned.
                unset -nocomplain auxset
                set iterations 1000
                while {$iterations != 0} {
                    incr iterations -1
                    set res [r srandmember myset $size]
                    foreach ele $res {
                        set auxset($ele) 1
                    }
                    if {[lsort [array names myset]] eq
                        [lsort [array names auxset]]} {
                        break;
                    }
                }
                assert {$iterations != 0}
            }
        }
    }

    foreach {type contents} {
        listpack {
            1 5 10 50 125
            MARY PATRICIA LINDA BARBARA ELIZABETH
        }
        intset {
            0 1 2 3 4 5 6 7 8 9
        }
        hashtable {
            ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
            1 5 10 50 125
            MARY PATRICIA LINDA BARBARA
        }
    } {
        test "SRANDMEMBER histogram distribution - $type" {
            create_set myset $contents
            assert_encoding $type myset
            unset -nocomplain myset
            array set myset {}
            foreach ele [r smembers myset] {
                set myset($ele) 1
            }

            # Use negative count (PATH 1).
            # df = 9, 40 means 0.00001 probability
            set res [r srandmember myset -1000]
            assert_lessthan [chi_square_value $res] 40

            # Use positive count (both PATH 3 and PATH 4).
            foreach size {8 2} {
                unset -nocomplain allkey
                set iterations [expr {1000 / $size}]
                while {$iterations != 0} {
                    incr iterations -1
                    set res [r srandmember myset $size]
                    foreach ele $res {
                        lappend allkey $ele
                    }
                }
                # df = 9, 40 means 0.00001 probability
                assert_lessthan [chi_square_value $allkey] 40
            }
        }
    }

    proc is_rehashing {myset} {
        set htstats [r debug HTSTATS-KEY $myset]
        return [string match {*rehashing target*} $htstats]
    }

    proc rem_hash_set_top_N {myset n} {
        set cursor 0
        set members {}
        set enough 0
        while 1 {
            set res [r sscan $myset $cursor]
            set cursor [lindex $res 0]
            set k [lindex $res 1]
            foreach m $k {
                lappend members $m
                if {[llength $members] >= $n} {
                    set enough 1
                    break
                }
            }
            if {$enough || $cursor == 0} {
                break
            }
        }
        r srem $myset {*}$members
    }

    proc verify_rehashing_completed_key {myset table_size keys} {
        set htstats [r debug HTSTATS-KEY $myset]
        assert {![string match {*rehashing target*} $htstats]}
        return {[string match {*table size: $table_size*number of elements: $keys*} $htstats]}
    }

    test "SRANDMEMBER with a dict containing long chain" {
        set origin_save [config_get_set save ""]
        set origin_max_lp [config_get_set set-max-listpack-entries 0]
        set origin_save_delay [config_get_set rdb-key-save-delay 2147483647]

        # 1) Create a hash set with 100000 members.
        set members {}
        for {set i 0} {$i < 100000} {incr i} {
            lappend members [format "m:%d" $i]
        }
        create_set myset $members

        # 2) Wait for the hash set rehashing to finish.
        while {[is_rehashing myset]} {
            r srandmember myset 100
        }

        # 3) Turn off the rehashing of this set, and remove the members to 500.
        r bgsave
        rem_hash_set_top_N myset [expr {[r scard myset] - 500}]
        assert_equal [r scard myset] 500

        # 4) Kill RDB child process to restart rehashing.
        set pid1 [get_child_pid 0]
        catch {exec kill -9 $pid1}
        waitForBgsave r

        # 5) Let the set hash to start rehashing
        r spop myset 1
        assert [is_rehashing myset]

        # 6) Verify that when rdb saving is in progress, rehashing will still be performed (because
        # the ratio is extreme) by waiting for it to finish during an active bgsave.
        r bgsave

        while {[is_rehashing myset]} {
            r srandmember myset 1
        }
        if {$::verbose} {
            puts [r debug HTSTATS-KEY myset full]
        }

        set pid1 [get_child_pid 0]
        catch {exec kill -9 $pid1}
        waitForBgsave r

        # 7) Check that eventually, SRANDMEMBER returns all elements.
        array set allmyset {}
        foreach ele [r smembers myset] {
            set allmyset($ele) 1
        }
        unset -nocomplain auxset
        set iterations 1000
        while {$iterations != 0} {
            incr iterations -1
            set res [r srandmember myset -10]
            foreach ele $res {
                set auxset($ele) 1
            }
            if {[lsort [array names allmyset]] eq
                [lsort [array names auxset]]} {
                break;
            }
        }
        assert {$iterations != 0}

        # 8) Remove the members to 30 in order to calculate the value of Chi-Square Distribution,
        #    otherwise we would need more iterations.
        rem_hash_set_top_N myset [expr {[r scard myset] - 30}]
        assert_equal [r scard myset] 30
        
        # Hash set rehashing would be completed while removing members from the `myset`
        # We also check the size and members in the hash table.
        verify_rehashing_completed_key myset 64 30

        # Now that we have a hash set with only one long chain bucket.
        set htstats [r debug HTSTATS-KEY myset full]
        assert {[regexp {different slots: ([0-9]+)} $htstats - different_slots]}
        assert {[regexp {max chain length: ([0-9]+)} $htstats - max_chain_length]}
        assert {$different_slots == 1 && $max_chain_length == 30}

        # 9) Use positive count (PATH 4) to get 10 elements (out of 30) each time.
        unset -nocomplain allkey
        set iterations 1000
        while {$iterations != 0} {
            incr iterations -1
            set res [r srandmember myset 10]
            foreach ele $res {
                lappend allkey $ele
            }
        }
        # validate even distribution of random sampling (df = 29, 73 means 0.00001 probability)
        assert_lessthan [chi_square_value $allkey] 73

        r config set save $origin_save
        r config set set-max-listpack-entries $origin_max_lp
        r config set rdb-key-save-delay $origin_save_delay
    } {OK} {needs:debug slow}

    proc setup_move {} {
        r del myset3{t} myset4{t}
        create_set myset1{t} {1 a b}
        create_set myset2{t} {2 3 4}
        assert_encoding listpack myset1{t}
        assert_encoding intset myset2{t}
    }

    test "SMOVE basics - from regular set to intset" {
        # move a non-integer element to an intset should convert encoding
        setup_move
        assert_equal 1 [r smove myset1{t} myset2{t} a]
        assert_equal {1 b} [lsort [r smembers myset1{t}]]
        assert_equal {2 3 4 a} [lsort [r smembers myset2{t}]]
        assert_encoding listpack myset2{t}

        # move an integer element should not convert the encoding
        setup_move
        assert_equal 1 [r smove myset1{t} myset2{t} 1]
        assert_equal {a b} [lsort [r smembers myset1{t}]]
        assert_equal {1 2 3 4} [lsort [r smembers myset2{t}]]
        assert_encoding intset myset2{t}
    }

    test "SMOVE basics - from intset to regular set" {
        setup_move
        assert_equal 1 [r smove myset2{t} myset1{t} 2]
        assert_equal {1 2 a b} [lsort [r smembers myset1{t}]]
        assert_equal {3 4} [lsort [r smembers myset2{t}]]
    }

    test "SMOVE non existing key" {
        setup_move
        assert_equal 0 [r smove myset1{t} myset2{t} foo]
        assert_equal 0 [r smove myset1{t} myset1{t} foo]
        assert_equal {1 a b} [lsort [r smembers myset1{t}]]
        assert_equal {2 3 4} [lsort [r smembers myset2{t}]]
    }

    test "SMOVE non existing src set" {
        setup_move
        assert_equal 0 [r smove noset{t} myset2{t} foo]
        assert_equal {2 3 4} [lsort [r smembers myset2{t}]]
    }

    test "SMOVE from regular set to non existing destination set" {
        setup_move
        assert_equal 1 [r smove myset1{t} myset3{t} a]
        assert_equal {1 b} [lsort [r smembers myset1{t}]]
        assert_equal {a} [lsort [r smembers myset3{t}]]
        assert_encoding listpack myset3{t}
    }

    test "SMOVE from intset to non existing destination set" {
        setup_move
        assert_equal 1 [r smove myset2{t} myset3{t} 2]
        assert_equal {3 4} [lsort [r smembers myset2{t}]]
        assert_equal {2} [lsort [r smembers myset3{t}]]
        assert_encoding intset myset3{t}
    }

    test "SMOVE wrong src key type" {
        r set x{t} 10
        assert_error "WRONGTYPE*" {r smove x{t} myset2{t} foo}
    }

    test "SMOVE wrong dst key type" {
        r set x{t} 10
        assert_error "WRONGTYPE*" {r smove myset2{t} x{t} foo}
    }

    test "SMOVE with identical source and destination" {
        r del set{t}
        r sadd set{t} a b c
        r smove set{t} set{t} b
        lsort [r smembers set{t}]
    } {a b c}

    test "SMOVE only notify dstset when the addition is successful" {
        r del srcset{t}
        r del dstset{t}

        r sadd srcset{t} a b
        r sadd dstset{t} a

        r watch dstset{t}

        r multi
        r sadd dstset{t} c

        set r2 [redis_client]
        $r2 smove srcset{t} dstset{t} a

        # The dstset is actually unchanged, multi should success
        r exec
        set res [r scard dstset{t}]
        assert_equal $res 2
        $r2 close
    }

    tags {slow} {
        test {intsets implementation stress testing} {
            for {set j 0} {$j < 20} {incr j} {
                unset -nocomplain s
                array set s {}
                r del s
                set len [randomInt 1024]
                for {set i 0} {$i < $len} {incr i} {
                    randpath {
                        set data [randomInt 65536]
                    } {
                        set data [randomInt 4294967296]
                    } {
                        set data [randomInt 18446744073709551616]
                    }
                    set s($data) {}
                    r sadd s $data
                }
                assert_equal [lsort [r smembers s]] [lsort [array names s]]
                set len [array size s]
                for {set i 0} {$i < $len} {incr i} {
                    set e [r spop s]
                    if {![info exists s($e)]} {
                        puts "Can't find '$e' on local array"
                        puts "Local array: [lsort [r smembers s]]"
                        puts "Remote array: [lsort [array names s]]"
                        error "exception"
                    }
                    array unset s $e
                }
                assert_equal [r scard s] 0
                assert_equal [array size s] 0
            }
        }
    }
}

run_solo {set-large-memory} {
start_server [list overrides [list save ""] ] {

# test if the server supports such large configs (avoid 32 bit builds)
catch {
    r config set proto-max-bulk-len 10000000000 ;#10gb
    r config set client-query-buffer-limit 10000000000 ;#10gb
}
if {[lindex [r config get proto-max-bulk-len] 1] == 10000000000} {

    set str_length 4400000000 ;#~4.4GB

    test {SADD, SCARD, SISMEMBER - large data} {
        r flushdb
        r write "*3\r\n\$4\r\nSADD\r\n\$5\r\nmyset\r\n"
        assert_equal 1 [write_big_bulk $str_length "aaa"]
        r write "*3\r\n\$4\r\nSADD\r\n\$5\r\nmyset\r\n"
        assert_equal 1 [write_big_bulk $str_length "bbb"]
        r write "*3\r\n\$4\r\nSADD\r\n\$5\r\nmyset\r\n"
        assert_equal 0 [write_big_bulk $str_length "aaa"]
        assert_encoding hashtable myset
        set s0 [s used_memory]
        assert {$s0 > [expr $str_length * 2]}
        assert_equal 2 [r scard myset]

        r write "*3\r\n\$9\r\nSISMEMBER\r\n\$5\r\nmyset\r\n"
        assert_equal 1 [write_big_bulk $str_length "aaa"]
        r write "*3\r\n\$9\r\nSISMEMBER\r\n\$5\r\nmyset\r\n"
        assert_equal 0 [write_big_bulk $str_length "ccc"]
        r write "*3\r\n\$4\r\nSREM\r\n\$5\r\nmyset\r\n"
        assert_equal 1 [write_big_bulk $str_length "bbb"]
        assert_equal [read_big_bulk {r spop myset} yes "aaa"] $str_length
    } {} {large-memory}

    # restore defaults
    r config set proto-max-bulk-len 536870912
    r config set client-query-buffer-limit 1073741824

} ;# skip 32bit builds
}
} ;# run_solo
