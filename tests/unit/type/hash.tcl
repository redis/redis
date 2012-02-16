start_server {tags {"hash"}} {
    test {HSET/HLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r hset smallhash $key $val
            set smallhash($key) $val
        }
        list [r hlen smallhash]
    } {8}

    test {Is the small hash encoded with a zipmap?} {
        assert_encoding zipmap smallhash
    }

    test {HSET/HLEN - Big hash creation} {
        array set bighash {}
        for {set i 0} {$i < 1024} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists bighash($key)]} {
                incr i -1
                continue
            }
            r hset bighash $key $val
            set bighash($key) $val
        }
        list [r hlen bighash]
    } {1024}

    test {Is the big hash encoded with a zipmap?} {
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

    test {HMSET wrong number of args} {
        catch {r hmset smallhash key1 val1 key2} err
        format $err
    } {*wrong number*}

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

    test {HMGET against wrong type} {
        r set wrongtype somevalue
        assert_error "*wrong*" {r hmget wrongtype field1 field2}
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

    test {Is a zipmap encoded Hash promoted on big payload?} {
        r hset smallhash foo [string repeat a 1024]
        r debug object smallhash
    } {*hashtable*}

    test {HINCRBY against non existing database key} {
        r del htest
        list [r hincrby htest foo 2]
    } {2}

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
        catch {r hincrby smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1 1}

    test {HINCRBY fails against hash value with spaces (right)} {
        r hset smallhash str "11 "
        r hset bighash str "11 "
        catch {r hincrby smallhash str 1} smallerr
        catch {r hincrby smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
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
        catch {r hincrbyfloat smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $smallerr]
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1 1}

    test {HINCRBYFLOAT fails against hash value with spaces (right)} {
        r hset smallhash str "11 "
        r hset bighash str "11 "
        catch {r hincrbyfloat smallhash str 1} smallerr
        catch {r hincrbyfloat smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $smallerr]
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1 1}

    test {Hash zipmap regression test for large keys} {
        r hset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk a
        r hset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk b
        r hget hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk
    } {b}
}
