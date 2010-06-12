start_server {tags {"set"}} {
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
        for {set i 0} {$i < 1000} {incr i} {
            r sadd set1 $i
            r sadd set2 [expr $i+995]
        }
        foreach i {999 995 1000 2000} {
            r sadd set3 $i
        }
        for {set i 5} {$i < 1000} {incr i} {
            r sadd set4 $i
        }
        r sadd set5 0

        # it is possible that a hashtable encoded only contains integers,
        # because it is converted from an intset to a hashtable when a
        # non-integer element is added and then removed.
        if {$type eq "hashtable"} {
            for {set i 1} {$i <= 5} {incr i} {
                r sadd [format "set%d" $i] foo
                r srem [format "set%d" $i] foo
            }
        }

        test "Generated sets must be encoded as $type" {
            for {set i 1} {$i <= 5} {incr i} {
                assert_encoding $type [format "set%d" $i]
            }
        }

        test "SINTER with two sets - $type" {
            assert_equal {995 996 997 998 999} [lsort [r sinter set1 set2]]
        }

        test "SINTERSTORE with two sets - $type" {
            r sinterstore setres set1 set2
            assert_encoding intset setres
            assert_equal {995 996 997 998 999} [lsort [r smembers setres]]
        }

        test "SINTERSTORE with two sets, after a DEBUG RELOAD - $type" {
            r debug reload
            r sinterstore setres set1 set2
            assert_encoding intset setres
            assert_equal {995 996 997 998 999} [lsort [r smembers setres]]
        }

        test "SUNION with two sets - $type" {
            set expected [lsort -uniq "[r smembers set1] [r smembers set2]"]
            assert_equal $expected [lsort [r sunion set1 set2]]
        }

        test "SUNIONSTORE with two sets - $type" {
            r sunionstore setres set1 set2
            assert_encoding intset setres
            set expected [lsort -uniq "[r smembers set1] [r smembers set2]"]
            assert_equal $expected [lsort [r smembers setres]]
        }

        test "SINTER against three sets - $type" {
            assert_equal {995 999} [lsort [r sinter set1 set2 set3]]
        }

        test "SINTERSTORE with three sets - $type" {
            r sinterstore setres set1 set2 set3
            assert_equal {995 999} [r smembers setres]
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
            assert_encoding intset setres
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

    test {SMOVE basics} {
        r sadd myset1 a
        r sadd myset1 b
        r sadd myset1 c
        r sadd myset2 x
        r sadd myset2 y
        r sadd myset2 z
        r smove myset1 myset2 a
        list [lsort [r smembers myset2]] [lsort [r smembers myset1]]
    } {{a x y z} {b c}}

    test {SMOVE non existing key} {
        list [r smove myset1 myset2 foo] [lsort [r smembers myset2]] [lsort [r smembers myset1]]
    } {0 {a x y z} {b c}}

    test {SMOVE non existing src set} {
        list [r smove noset myset2 foo] [lsort [r smembers myset2]]
    } {0 {a x y z}}

    test {SMOVE non existing dst set} {
        list [r smove myset2 myset3 y] [lsort [r smembers myset2]] [lsort [r smembers myset3]]
    } {1 {a x z} y}

    test {SMOVE wrong src key type} {
        r set x 10
        catch {r smove x myset2 foo} err
        format $err
    } {ERR*}

    test {SMOVE wrong dst key type} {
        r set x 10
        catch {r smove myset2 x foo} err
        format $err
    } {ERR*}
}
