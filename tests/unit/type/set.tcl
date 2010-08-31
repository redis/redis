start_server {
    tags {"set"}
    overrides {
        "set-max-intset-entries" 512
    }
} {
    proc create_set {key entries} {
        r del $key
        foreach entry $entries { r sadd $key $entry }
    }

    test {SADD, SCARD, SISMEMBER, SMEMBERS basics - regular set} {
        create_set myset {foo}
        assert_encoding hashtable myset
        assert_equal 1 [r sadd myset bar]
        assert_equal 0 [r sadd myset bar]
        assert_equal 2 [r scard myset]
        assert_equal 1 [r sismember myset foo]
        assert_equal 1 [r sismember myset bar]
        assert_equal 0 [r sismember myset bla]
        assert_equal {bar foo} [lsort [r smembers myset]]
    }

    test {SADD, SCARD, SISMEMBER, SMEMBERS basics - intset} {
        create_set myset {17}
        assert_encoding intset myset
        assert_equal 1 [r sadd myset 16]
        assert_equal 0 [r sadd myset 16]
        assert_equal 2 [r scard myset]
        assert_equal 1 [r sismember myset 16]
        assert_equal 1 [r sismember myset 17]
        assert_equal 0 [r sismember myset 18]
        assert_equal {16 17} [lsort [r smembers myset]]
    }

    test {SADD against non set} {
        r lpush mylist foo
        assert_error ERR*kind* {r sadd mylist bar}
    }

    test "SADD a non-integer against an intset" {
        create_set myset {1 2 3}
        assert_encoding intset myset
        assert_equal 1 [r sadd myset a]
        assert_encoding hashtable myset
    }

    test "SADD an integer larger than 64 bits" {
        create_set myset {213244124402402314402033402}
        assert_encoding hashtable myset
        assert_equal 1 [r sismember myset 213244124402402314402033402]
    }

    test "SADD overflows the maximum allowed integers in an intset" {
        r del myset
        for {set i 0} {$i < 512} {incr i} { r sadd myset $i }
        assert_encoding intset myset
        assert_equal 1 [r sadd myset 512]
        assert_encoding hashtable myset
    }

    test "Set encoding after DEBUG RELOAD" {
        r del myintset myhashset mylargeintset
        for {set i 0} {$i <  100} {incr i} { r sadd myintset $i }
        for {set i 0} {$i < 1280} {incr i} { r sadd mylargeintset $i }
        for {set i 0} {$i <  256} {incr i} { r sadd myhashset [format "i%03d" $i] }
        assert_encoding intset myintset
        assert_encoding hashtable mylargeintset
        assert_encoding hashtable myhashset

        r debug reload
        assert_encoding intset myintset
        assert_encoding hashtable mylargeintset
        assert_encoding hashtable myhashset
    }

    test {SREM basics - regular set} {
        create_set myset {foo bar ciao}
        assert_encoding hashtable myset
        assert_equal 0 [r srem myset qux]
        assert_equal 1 [r srem myset foo]
        assert_equal {bar ciao} [lsort [r smembers myset]]
    }

    test {SREM basics - intset} {
        create_set myset {3 4 5}
        assert_encoding intset myset
        assert_equal 0 [r srem myset 6]
        assert_equal 1 [r srem myset 4]
        assert_equal {3 5} [lsort [r smembers myset]]
    }

    foreach {type} {hashtable intset} {
        for {set i 1} {$i <= 5} {incr i} {
            r del [format "set%d" $i]
        }
        for {set i 0} {$i < 200} {incr i} {
            r sadd set1 $i
            r sadd set2 [expr $i+195]
        }
        foreach i {199 195 1000 2000} {
            r sadd set3 $i
        }
        for {set i 5} {$i < 200} {incr i} {
            r sadd set4 $i
        }
        r sadd set5 0

        # To make sure the sets are encoded as the type we are testing -- also
        # when the VM is enabled and the values may be swapped in and out
        # while the tests are running -- an extra element is added to every
        # set that determines its encoding.
        set large 200
        if {$type eq "hashtable"} {
            set large foo
        }

        for {set i 1} {$i <= 5} {incr i} {
            r sadd [format "set%d" $i] $large
        }

        test "Generated sets must be encoded as $type" {
            for {set i 1} {$i <= 5} {incr i} {
                assert_encoding $type [format "set%d" $i]
            }
        }

        test "SINTER with two sets - $type" {
            assert_equal [list 195 196 197 198 199 $large] [lsort [r sinter set1 set2]]
        }

        test "SINTERSTORE with two sets - $type" {
            r sinterstore setres set1 set2
            assert_encoding $type setres
            assert_equal [list 195 196 197 198 199 $large] [lsort [r smembers setres]]
        }

        test "SINTERSTORE with two sets, after a DEBUG RELOAD - $type" {
            r debug reload
            r sinterstore setres set1 set2
            assert_encoding $type setres
            assert_equal [list 195 196 197 198 199 $large] [lsort [r smembers setres]]
        }

        test "SUNION with two sets - $type" {
            set expected [lsort -uniq "[r smembers set1] [r smembers set2]"]
            assert_equal $expected [lsort [r sunion set1 set2]]
        }

        test "SUNIONSTORE with two sets - $type" {
            r sunionstore setres set1 set2
            assert_encoding $type setres
            set expected [lsort -uniq "[r smembers set1] [r smembers set2]"]
            assert_equal $expected [lsort [r smembers setres]]
        }

        test "SINTER against three sets - $type" {
            assert_equal [list 195 199 $large] [lsort [r sinter set1 set2 set3]]
        }

        test "SINTERSTORE with three sets - $type" {
            r sinterstore setres set1 set2 set3
            assert_equal [list 195 199 $large] [lsort [r smembers setres]]
        }

        test "SUNION with non existing keys - $type" {
            set expected [lsort -uniq "[r smembers set1] [r smembers set2]"]
            assert_equal $expected [lsort [r sunion nokey1 set1 set2 nokey2]]
        }

        test "SDIFF with two sets - $type" {
            assert_equal {0 1 2 3 4} [lsort [r sdiff set1 set4]]
        }

        test "SDIFF with three sets - $type" {
            assert_equal {1 2 3 4} [lsort [r sdiff set1 set4 set5]]
        }

        test "SDIFFSTORE with three sets - $type" {
            r sdiffstore setres set1 set4 set5
            # The type is determined by type of the first key to diff against.
            # See the implementation for more information.
            assert_encoding $type setres
            assert_equal {1 2 3 4} [lsort [r smembers setres]]
        }
    }

    test "SINTER against non-set should throw error" {
        r set key1 x
        assert_error "ERR*wrong kind*" {r sinter key1 noset}
    }

    test "SUNION against non-set should throw error" {
        r set key1 x
        assert_error "ERR*wrong kind*" {r sunion key1 noset}
    }

    test "SINTERSTORE against non existing keys should delete dstkey" {
        r set setres xxx
        assert_equal 0 [r sinterstore setres foo111 bar222]
        assert_equal 0 [r exists setres]
    }

    test "SUNIONSTORE against non existing keys should delete dstkey" {
        r set setres xxx
        assert_equal 0 [r sunionstore setres foo111 bar222]
        assert_equal 0 [r exists setres]
    }

    foreach {type contents} {hashtable {a b c} intset {1 2 3}} {
        test "SPOP basics - $type" {
            create_set myset $contents
            assert_encoding $type myset
            assert_equal $contents [lsort [list [r spop myset] [r spop myset] [r spop myset]]]
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

    proc setup_move {} {
        r del myset3 myset4
        create_set myset1 {1 a b}
        create_set myset2 {2 3 4}
        assert_encoding hashtable myset1
        assert_encoding intset myset2
    }

    test "SMOVE basics - from regular set to intset" {
        # move a non-integer element to an intset should convert encoding
        setup_move
        assert_equal 1 [r smove myset1 myset2 a]
        assert_equal {1 b} [lsort [r smembers myset1]]
        assert_equal {2 3 4 a} [lsort [r smembers myset2]]
        assert_encoding hashtable myset2

        # move an integer element should not convert the encoding
        setup_move
        assert_equal 1 [r smove myset1 myset2 1]
        assert_equal {a b} [lsort [r smembers myset1]]
        assert_equal {1 2 3 4} [lsort [r smembers myset2]]
        assert_encoding intset myset2
    }

    test "SMOVE basics - from intset to regular set" {
        setup_move
        assert_equal 1 [r smove myset2 myset1 2]
        assert_equal {1 2 a b} [lsort [r smembers myset1]]
        assert_equal {3 4} [lsort [r smembers myset2]]
    }

    test "SMOVE non existing key" {
        setup_move
        assert_equal 0 [r smove myset1 myset2 foo]
        assert_equal {1 a b} [lsort [r smembers myset1]]
        assert_equal {2 3 4} [lsort [r smembers myset2]]
    }

    test "SMOVE non existing src set" {
        setup_move
        assert_equal 0 [r smove noset myset2 foo]
        assert_equal {2 3 4} [lsort [r smembers myset2]]
    }

    test "SMOVE from regular set to non existing destination set" {
        setup_move
        assert_equal 1 [r smove myset1 myset3 a]
        assert_equal {1 b} [lsort [r smembers myset1]]
        assert_equal {a} [lsort [r smembers myset3]]
        assert_encoding hashtable myset3
    }

    test "SMOVE from intset to non existing destination set" {
        setup_move
        assert_equal 1 [r smove myset2 myset3 2]
        assert_equal {3 4} [lsort [r smembers myset2]]
        assert_equal {2} [lsort [r smembers myset3]]
        assert_encoding intset myset3
    }

    test "SMOVE wrong src key type" {
        r set x 10
        assert_error "ERR*wrong kind*" {r smove x myset2 foo}
    }

    test "SMOVE wrong dst key type" {
        r set x 10
        assert_error "ERR*wrong kind*" {r smove myset2 x foo}
    }
}
