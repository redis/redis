start_server {tags {"trie"}} {
    test {TSET/TLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r tset smallhash $key $val
            set smallhash($key) $val
        }
        list [r tlen smallhash]
    } {8}

    test {TSET/TLEN - Big hash creation} {
        array set bighash {}
        for {set i 0} {$i < 1024} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists bighash($key)]} {
                incr i -1
                continue
            }
            r tset bighash $key $val
            set bighash($key) $val
        }
        list [r tlen bighash]
    } {1024}

    test {TGET against the small hash} {
        set err {}
        foreach k [array names smallhash *] {
            if {$smallhash($k) ne [r tget smallhash $k]} {
                set err "$smallhash($k) != [r tget smallhash $k]"
                break
            }
        }
        set _ $err
    } {}

    test {TGET against the big hash} {
        set err {}
        foreach k [array names bighash *] {
            if {$bighash($k) ne [r tget bighash $k]} {
                set err "$bighash($k) != [r tget bighash $k]"
                break
            }
        }
        set _ $err
    } {}

    test {TGET against non existing key} {
        set rv {}
        lappend rv [r tget smallhash __123123123__]
        lappend rv [r tget bighash __123123123__]
        set _ $rv
    } {{} {}}

    test {TSET in update and insert mode} {
        set rv {}
        set k [lindex [array names smallhash *] 0]
        lappend rv [r tset smallhash $k newval1]
        set smallhash($k) newval1
        lappend rv [r tget smallhash $k]
        lappend rv [r tset smallhash __foobar123__ newval]
        set k [lindex [array names bighash *] 0]
        lappend rv [r tset bighash $k newval2]
        set bighash($k) newval2
        lappend rv [r tget bighash $k]
        lappend rv [r tset bighash __foobar123__ newval]
        lappend rv [r tdel smallhash __foobar123__]
        lappend rv [r tdel bighash __foobar123__]
        set _ $rv
    } {0 newval1 1 0 newval2 1 1 1}

    test {TSETNX target key missing - small hash} {
        r tsetnx smallhash __123123123__ foo
        r tget smallhash __123123123__
    } {foo}

    test {TSETNX target key exists - small hash} {
        r tsetnx smallhash __123123123__ bar
        set result [r tget smallhash __123123123__]
        r tdel smallhash __123123123__
        set _ $result
    } {foo}

    test {TSETNX target key missing - big hash} {
        r tsetnx bighash __123123123__ foo
        r tget bighash __123123123__
    } {foo}

    test {TSETNX target key exists - big hash} {
        r tsetnx bighash __123123123__ bar
        set result [r tget bighash __123123123__]
        r tdel bighash __123123123__
        set _ $result
    } {foo}

    test {TMSET wrong number of args} {
        catch {r tmset smallhash key1 val1 key2} err
        format $err
    } {*wrong number*}

    test {TMSET - small hash} {
        set args {}
        foreach {k v} [array get smallhash] {
            set newval [randstring 0 8 alpha]
            set smallhash($k) $newval
            lappend args $k $newval
        }
        r tmset smallhash {*}$args
    } {OK}

    test {TMSET - big hash} {
        set args {}
        foreach {k v} [array get bighash] {
            set newval [randstring 0 8 alpha]
            set bighash($k) $newval
            lappend args $k $newval
        }
        r tmset bighash {*}$args
    } {OK}

    test {TMGET against non existing key and fields} {
        set rv {}
        lappend rv [r tmget doesntexist __123123123__ __456456456__]
        lappend rv [r tmget smallhash __123123123__ __456456456__]
        lappend rv [r tmget bighash __123123123__ __456456456__]
        set _ $rv
    } {{{} {}} {{} {}} {{} {}}}

    test {TMGET against wrong type} {
        r set wrongtype somevalue
        assert_error "*wrong*" {r tmget wrongtype field1 field2}
    }

    test {TMGET - small hash} {
        set keys {}
        set vals {}
        foreach {k v} [array get smallhash] {
            lappend keys $k
            lappend vals $v
        }
        set err {}
        set result [r tmget smallhash {*}$keys]
        if {$vals ne $result} {
            set err "$vals != $result"
            break
        }
        set _ $err
    } {}

    test {TMGET - big hash} {
        set keys {}
        set vals {}
        foreach {k v} [array get bighash] {
            lappend keys $k
            lappend vals $v
        }
        set err {}
        set result [r tmget bighash {*}$keys]
        if {$vals ne $result} {
            set err "$vals != $result"
            break
        }
        set _ $err
    } {}

    test {TKEYS - small hash} {
        lsort [r tkeys smallhash]
    } [lsort [array names smallhash *]]

    test {TKEYS - big hash} {
        lsort [r tkeys bighash]
    } [lsort [array names bighash *]]

    test {TVALS - small hash} {
        set vals {}
        foreach {k v} [array get smallhash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    } [lsort [r tvals smallhash]]

    test {TVALS - big hash} {
        set vals {}
        foreach {k v} [array get bighash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    } [lsort [r tvals bighash]]

    test {TGETALL - small hash} {
        lsort [r tgetall smallhash]
    } [lsort [array get smallhash]]

    test {TGETALL - big hash} {
        lsort [r tgetall bighash]
    } [lsort [array get bighash]]

    test {TDEL and return value} {
        set rv {}
        lappend rv [r tdel smallhash nokey]
        lappend rv [r tdel bighash nokey]
        set k [lindex [array names smallhash *] 0]
        lappend rv [r tdel smallhash $k]
        lappend rv [r tdel smallhash $k]
        lappend rv [r tget smallhash $k]
        unset smallhash($k)
        set k [lindex [array names bighash *] 0]
        lappend rv [r tdel bighash $k]
        lappend rv [r tdel bighash $k]
        lappend rv [r tget bighash $k]
        unset bighash($k)
        set _ $rv
    } {0 0 1 0 {} 1 0 {}}

    test {TDEL - more than a single value} {
        set rv {}
        r del myhash
        r tmset myhash a 1 b 2 c 3
        assert_equal 0 [r tdel myhash x y]
        assert_equal 2 [r tdel myhash a c f]
        r tgetall myhash
    } {b 2}

    test {TDEL - hash becomes empty before deleting all specified fields} {
        r del myhash
        r tmset myhash a 1 b 2 c 3
        assert_equal 3 [r tdel myhash a b c d e]
        assert_equal 0 [r exists myhash]
    }

    test {TEXISTS} {
        set rv {}
        set k [lindex [array names smallhash *] 0]
        lappend rv [r texists smallhash $k]
        lappend rv [r texists smallhash nokey]
        set k [lindex [array names bighash *] 0]
        lappend rv [r texists bighash $k]
        lappend rv [r texists bighash nokey]
    } {1 0 1 0}

    test {TINCRBY against non existing database key} {
        r del htest
        list [r tincrby htest foo 2]
    } {2}

    test {TINCRBY against non existing hash key} {
        set rv {}
        r tdel smallhash tmp
        r tdel bighash tmp
        lappend rv [r tincrby smallhash tmp 2]
        lappend rv [r tget smallhash tmp]
        lappend rv [r tincrby bighash tmp 2]
        lappend rv [r tget bighash tmp]
    } {2 2 2 2}

    test {TINCRBY against hash key created by tincrby itself} {
        set rv {}
        lappend rv [r tincrby smallhash tmp 3]
        lappend rv [r tget smallhash tmp]
        lappend rv [r tincrby bighash tmp 3]
        lappend rv [r tget bighash tmp]
    } {5 5 5 5}

    test {TINCRBY against hash key originally set with TSET} {
        r tset smallhash tmp 100
        r tset bighash tmp 100
        list [r tincrby smallhash tmp 2] [r tincrby bighash tmp 2]
    } {102 102}

    test {TINCRBY over 32bit value} {
        r tset smallhash tmp 17179869184
        r tset bighash tmp 17179869184
        list [r tincrby smallhash tmp 1] [r tincrby bighash tmp 1]
    } {17179869185 17179869185}

    test {TINCRBY over 32bit value with over 32bit increment} {
        r tset smallhash tmp 17179869184
        r tset bighash tmp 17179869184
        list [r tincrby smallhash tmp 17179869184] [r tincrby bighash tmp 17179869184]
    } {34359738368 34359738368}

    test {TINCRBY fails against hash value with spaces (left)} {
        r tset smallhash str " 11"
        r tset bighash str " 11"
        catch {r tincrby smallhash str 1} smallerr
        catch {r tincrby smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1 1}

    test {TINCRBY fails against hash value with spaces (right)} {
        r tset smallhash str "11 "
        r tset bighash str "11 "
        catch {r tincrby smallhash str 1} smallerr
        catch {r tincrby smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1 1}

    test {TINCRBY can detect overflows} {
        set e {}
        r tset hash n -9223372036854775484
        assert {[r tincrby hash n -1] == -9223372036854775485}
        catch {r tincrby hash n -10000} e
        set e
    } {*overflow*}

    test {TINCRBYFLOAT against non existing database key} {
        r del htest
        list [r tincrbyfloat htest foo 2.5]
    } {2.5}

    test {TINCRBYFLOAT against non existing hash key} {
        set rv {}
        r tdel smallhash tmp
        r tdel bighash tmp
        lappend rv [roundFloat [r tincrbyfloat smallhash tmp 2.5]]
        lappend rv [roundFloat [r tget smallhash tmp]]
        lappend rv [roundFloat [r tincrbyfloat bighash tmp 2.5]]
        lappend rv [roundFloat [r tget bighash tmp]]
    } {2.5 2.5 2.5 2.5}

    test {TINCRBYFLOAT against hash key created by tincrby itself} {
        set rv {}
        lappend rv [roundFloat [r tincrbyfloat smallhash tmp 3.5]]
        lappend rv [roundFloat [r tget smallhash tmp]]
        lappend rv [roundFloat [r tincrbyfloat bighash tmp 3.5]]
        lappend rv [roundFloat [r tget bighash tmp]]
    } {6 6 6 6}

    test {TINCRBYFLOAT against hash key originally set with TSET} {
        r tset smallhash tmp 100
        r tset bighash tmp 100
        list [roundFloat [r tincrbyfloat smallhash tmp 2.5]] \
             [roundFloat [r tincrbyfloat bighash tmp 2.5]]
    } {102.5 102.5}

    test {TINCRBYFLOAT over 32bit value} {
        r tset smallhash tmp 17179869184
        r tset bighash tmp 17179869184
        list [r tincrbyfloat smallhash tmp 1] \
             [r tincrbyfloat bighash tmp 1]
    } {17179869185 17179869185}

    test {TINCRBYFLOAT over 32bit value with over 32bit increment} {
        r tset smallhash tmp 17179869184
        r tset bighash tmp 17179869184
        list [r tincrbyfloat smallhash tmp 17179869184] \
             [r tincrbyfloat bighash tmp 17179869184]
    } {34359738368 34359738368}

    test {TINCRBYFLOAT fails against hash value with spaces (left)} {
        r tset smallhash str " 11"
        r tset bighash str " 11"
        catch {r tincrbyfloat smallhash str 1} smallerr
        catch {r tincrbyfloat smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $smallerr]
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1 1}

    test {TINCRBYFLOAT fails against hash value with spaces (right)} {
        r tset smallhash str "11 "
        r tset bighash str "11 "
        catch {r tincrbyfloat smallhash str 1} smallerr
        catch {r tincrbyfloat smallhash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $smallerr]
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1 1}

    test {Hash ziplist regression test for large keys} {
        r tset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk a
        r tset hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk b
        r tget hash kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk
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
                    r tset hash $field $value
                    set hash($field) $value
                }

                # Verify
                foreach {k v} [array get hash] {
                    assert_equal $v [r tget hash $k]
                }
                assert_equal [array size hash] [r tlen hash]
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
                        r tset hash $field $value
                        set hash($field) $value
                    } {
                        set field [randomSignedInt 512]
                        set value [randomSignedInt 512]
                        r tset hash $field $value
                        set hash($field) $value
                    } {
                        randpath {
                            set field [randomValue]
                        } {
                            set field [randomSignedInt 512]
                        }
                        r tdel hash $field
                        unset -nocomplain hash($field)
                    }
                }

                # Verify
                foreach {k v} [array get hash] {
                    assert_equal $v [r tget hash $k]
                }
                assert_equal [array size hash] [r tlen hash]
            }
        }
    }
}
