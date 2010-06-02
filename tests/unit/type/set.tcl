start_server {} {
    test {SADD, SCARD, SISMEMBER, SMEMBERS basics} {
        r sadd myset foo
        r sadd myset bar
        list [r scard myset] [r sismember myset foo] \
            [r sismember myset bar] [r sismember myset bla] \
            [lsort [r smembers myset]]
    } {2 1 1 0 {bar foo}}

    test {SADD adding the same element multiple times} {
        r sadd myset foo
        r sadd myset foo
        r sadd myset foo
        r scard myset
    } {2}

    test {SADD against non set} {
        r lpush mylist foo
        catch {r sadd mylist bar} err
        format $err
    } {ERR*kind*}

    test {SREM basics} {
        r sadd myset ciao
        r srem myset foo
        lsort [r smembers myset]
    } {bar ciao}

    test {Mass SADD and SINTER with two sets} {
        for {set i 0} {$i < 1000} {incr i} {
            r sadd set1 $i
            r sadd set2 [expr $i+995]
        }
        lsort [r sinter set1 set2]
    } {995 996 997 998 999}

    test {SUNION with two sets} {
        lsort [r sunion set1 set2]
    } [lsort -uniq "[r smembers set1] [r smembers set2]"]

    test {SINTERSTORE with two sets} {
        r sinterstore setres set1 set2
        lsort [r smembers setres]
    } {995 996 997 998 999}

    test {SINTERSTORE with two sets, after a DEBUG RELOAD} {
        r debug reload
        r sinterstore setres set1 set2
        lsort [r smembers setres]
    } {995 996 997 998 999}

    test {SUNIONSTORE with two sets} {
        r sunionstore setres set1 set2
        lsort [r smembers setres]
    } [lsort -uniq "[r smembers set1] [r smembers set2]"]

    test {SUNIONSTORE against non existing keys} {
        r set setres xxx
        list [r sunionstore setres foo111 bar222] [r exists xxx]
    } {0 0}

    test {SINTER against three sets} {
        r sadd set3 999
        r sadd set3 995
        r sadd set3 1000
        r sadd set3 2000
        lsort [r sinter set1 set2 set3]
    } {995 999}

    test {SINTERSTORE with three sets} {
        r sinterstore setres set1 set2 set3
        lsort [r smembers setres]
    } {995 999}

    test {SUNION with non existing keys} {
        lsort [r sunion nokey1 set1 set2 nokey2]
    } [lsort -uniq "[r smembers set1] [r smembers set2]"]

    test {SDIFF with two sets} {
        for {set i 5} {$i < 1000} {incr i} {
            r sadd set4 $i
        }
        lsort [r sdiff set1 set4]
    } {0 1 2 3 4}

    test {SDIFF with three sets} {
        r sadd set5 0
        lsort [r sdiff set1 set4 set5]
    } {1 2 3 4}

    test {SDIFFSTORE with three sets} {
        r sdiffstore sres set1 set4 set5
        lsort [r smembers sres]
    } {1 2 3 4}

    test {SPOP basics} {
        r del myset
        r sadd myset 1
        r sadd myset 2
        r sadd myset 3
        list [lsort [list [r spop myset] [r spop myset] [r spop myset]]] [r scard myset]
    } {{1 2 3} 0}
    
    test {SRANDMEMBER} {
        r del myset
        r sadd myset a
        r sadd myset b
        r sadd myset c
        unset -nocomplain myset
        array set myset {}
        for {set i 0} {$i < 100} {incr i} {
            set myset([r srandmember myset]) 1
        }
        lsort [array names myset]
    } {a b c}
    
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
