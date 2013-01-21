start_server {tags {"trie"}} {
    test {TSET/TLEN} {
        array set trie {}
        for {set i 0} {$i < 1024} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists trie($key)]} {
                incr i -1
                continue
            }
            r tset trie $key $val
            set trie($key) $val
        }
        list [r tlen trie]
    } {1024}

    test {TGET} {
        set err {}
        foreach k [array names trie *] {
            if {$trie($k) ne [r tget trie $k]} {
                set err "$trie($k) != [r tget trie $k]"
                break
            }
        }
        set _ $err
    } {}

    test {TGET against non existing key} {
        set rv {}
        lappend rv [r tget trie __123123123__]
        set _ $rv
    } {{}}

    test {TSET in update and insert mode} {
        set rv {}
        set k [lindex [array names trie *] 0]
        lappend rv [r tset trie $k newval2]
        set trie($k) newval2
        lappend rv [r tget trie $k]
        lappend rv [r tset trie __foobar123__ newval]
        lappend rv [r tdel trie __foobar123__]
        set _ $rv
    } {0 newval2 1 1}

    test {TSETNX target key missing} {
        r tsetnx trie __123123123__ foo
        r tget trie __123123123__
    } {foo}

    test {TSETNX target key exists} {
        r tsetnx trie __123123123__ bar
        set result [r tget trie __123123123__]
        r tdel trie __123123123__
        set _ $result
    } {foo}

    test {TMSET wrong number of args} {
        catch {r tmset trie key1 val1 key2} err
        format $err
    } {*wrong number*}

    test {TMSET} {
        set args {}
        foreach {k v} [array get trie] {
            set newval [randstring 0 8 alpha]
            set trie($k) $newval
            lappend args $k $newval
        }
        r tmset trie {*}$args
    } {OK}

    test {TMGET against non existing key and fields} {
        set rv {}
        lappend rv [r tmget doesntexist __123123123__ __456456456__]
        lappend rv [r tmget trie __123123123__ __456456456__]
        set _ $rv
    } {{{} {}} {{} {}}}

    test {TMGET against wrong type} {
        r set wrongtype somevalue
        assert_error "*wrong*" {r tmget wrongtype field1 field2}
    }

    test {TMGET} {
        set keys {}
        set vals {}
        foreach {k v} [array get trie] {
            lappend keys $k
            lappend vals $v
        }
        set err {}
        set result [r tmget trie {*}$keys]
        if {$vals ne $result} {
            set err "$vals != $result"
            break
        }
        set _ $err
    } {}

    test {TKEYS} {
        lsort [r tkeys trie]
    } [lsort [array names trie *]]

    test {TVALS} {
        set vals {}
        foreach {k v} [array get trie] {
            lappend vals $v
        }
        set _ [lsort $vals]
    } [lsort [r tvals trie]]

    test {TGETALL} {
        lsort [r tgetall trie]
    } [lsort [array get trie]]

    test {TKEYS with prefix} {
        set rv {}

        r del smallhash
        r tset smallhash prefix1_a 1
        r tset smallhash prefix1_b 2
        r tset smallhash prefix2_a 3
        r tset smallhash prefix2_b 4

        lappend rv [lsort [r tkeys smallhash]]
        lappend rv [lsort [r tkeys smallhash prefix]]
        lappend rv [lsort [r tkeys smallhash prefix1]]
        lappend rv [lsort [r tkeys smallhash prefix2]]
        lappend rv [lsort [r tkeys smallhash prefix_doesnotexist]]
    } {{prefix1_a prefix1_b prefix2_a prefix2_b} {prefix1_a prefix1_b prefix2_a prefix2_b} {prefix1_a prefix1_b} {prefix2_a prefix2_b} {}}

    test {TVALS with prefix} {
        set rv {}

        lappend rv [lsort [r tvals smallhash]]
        lappend rv [lsort [r tvals smallhash prefix]]
        lappend rv [lsort [r tvals smallhash prefix1]]
        lappend rv [lsort [r tvals smallhash prefix2]]
        lappend rv [lsort [r tvals smallhash prefix_doesnotexist]]
    } {{1 2 3 4} {1 2 3 4} {1 2} {3 4} {}}

    test {TGETALL with prefix} {
        set rv {}

        lappend rv [lsort [r tgetall smallhash]]
        lappend rv [lsort [r tgetall smallhash prefix]]
        lappend rv [lsort [r tgetall smallhash prefix1]]
        lappend rv [lsort [r tgetall smallhash prefix2]]
        lappend rv [lsort [r tgetall smallhash prefix_doesnotexist]]
    } {{1 2 3 4 prefix1_a prefix1_b prefix2_a prefix2_b} {1 2 3 4 prefix1_a prefix1_b prefix2_a prefix2_b} {1 2 prefix1_a prefix1_b} {3 4 prefix2_a prefix2_b} {}}

    test {TDEL and return value} {
        set rv {}
        lappend rv [r tdel trie nokey]
        set k [lindex [array names trie *] 0]
        lappend rv [r tdel trie $k]
        lappend rv [r tdel trie $k]
        lappend rv [r tget trie $k]
        unset trie($k)
        set _ $rv
    } {0 1 0 {}}

    test {TDEL - more than a single value} {
        set rv {}
        r del mytrie
        r tmset mytrie a 1 b 2 c 3
        assert_equal 0 [r tdel mytrie x y]
        assert_equal 2 [r tdel mytrie a c f]
        r tgetall mytrie
    } {b 2}

    test {TDEL - trie becomes empty before deleting all specified fields} {
        r del mytrie
        r tmset mytrie a 1 b 2 c 3
        assert_equal 3 [r tdel mytrie a b c d e]
        assert_equal 0 [r exists mytrie]
    }

    test {TEXISTS} {
        set rv {}
        set k [lindex [array names trie *] 0]
        lappend rv [r texists trie $k]
        lappend rv [r texists trie nokey]
    } {1 0}

    test {TINCRBY against non existing database key} {
        r del htest
        list [r tincrby htest foo 2]
    } {2}

    test {TINCRBY against non existing trie key} {
        set rv {}
        r tdel trie tmp
        lappend rv [r tincrby trie tmp 2]
        lappend rv [r tget trie tmp]
    } {2 2}

    test {TINCRBY against trie key created by tincrby itself} {
        set rv {}
        lappend rv [r tincrby trie tmp 3]
        lappend rv [r tget trie tmp]
    } {5 5}

    test {TINCRBY against trie key originally set with TSET} {
        r tset trie tmp 100
        list [r tincrby trie tmp 2]
    } {102}

    test {TINCRBY over 32bit value} {
        r tset trie tmp 17179869184
        list [r tincrby trie tmp 1]
    } {17179869185}

    test {TINCRBY over 32bit value with over 32bit increment} {
        r tset trie tmp 17179869184
        list [r tincrby trie tmp 17179869184]
    } {34359738368}

    test {TINCRBY fails against trie value with spaces (left)} {
        r tset trie str " 11"
        catch {r tincrby trie str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1}

    test {TINCRBY fails against trie value with spaces (right)} {
        r tset trie str "11 "
        catch {r tincrby trie str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1}

    test {TINCRBY can detect overflows} {
        set e {}
        r tset trie n -9223372036854775484
        assert {[r tincrby trie n -1] == -9223372036854775485}
        catch {r tincrby trie n -10000} e
        set e
    } {*overflow*}

    test {TINCRBYFLOAT against non existing database key} {
        r del htest
        list [r tincrbyfloat htest foo 2.5]
    } {2.5}

    test {TINCRBYFLOAT against non existing trie key} {
        set rv {}
        r tdel trie tmp
        lappend rv [roundFloat [r tincrbyfloat trie tmp 2.5]]
        lappend rv [roundFloat [r tget trie tmp]]
    } {2.5 2.5}

    test {TINCRBYFLOAT against trie key created by tincrby itself} {
        set rv {}
        lappend rv [roundFloat [r tincrbyfloat trie tmp 3.5]]
        lappend rv [roundFloat [r tget trie tmp]]
    } {6 6}

    test {TINCRBYFLOAT against trie key originally set with TSET} {
        r tset trie tmp 100
        list [roundFloat [r tincrbyfloat trie tmp 2.5]]
    } {102.5}

    test {TINCRBYFLOAT over 32bit value} {
        r tset trie tmp 17179869184
        list [r tincrbyfloat trie tmp 1]
    } {17179869185}

    test {TINCRBYFLOAT over 32bit value with over 32bit increment} {
        r tset trie tmp 17179869184
        list [r tincrbyfloat trie tmp 17179869184]
    } {34359738368}

    test {TINCRBYFLOAT fails against trie value with spaces (left)} {
        r tset trie str " 11"
        catch {r tincrbyfloat trie str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1}

    test {TINCRBYFLOAT fails against trie value with spaces (right)} {
        r tset trie str "11 "
        catch {r tincrbyfloat trie str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not*float*" $bigerr]
    } {1}

    test {Trie test for large keys} {
        r tset trie kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk a
        r tset trie kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk b
        r tget trie kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk
    } {b}

    foreach size {10 512} {
        test "Trie fuzzing #1 - $size fields" {
            for {set times 0} {$times < 10} {incr times} {
                catch {unset trie}
                array set trie {}
                r del trie

                # Create
                for {set j 0} {$j < $size} {incr j} {
                    set field [randomValue]
                    set value [randomValue]
                    r tset trie $field $value
                    set trie($field) $value
                }

                # Verify
                foreach {k v} [array get trie] {
                    assert_equal $v [r tget trie $k]
                }
                assert_equal [array size trie] [r tlen trie]
            }
        }

        test "Trie fuzzing #2 - $size fields" {
            for {set times 0} {$times < 10} {incr times} {
                catch {unset trie}
                array set trie {}
                r del trie

                # Create
                for {set j 0} {$j < $size} {incr j} {
                    randpath {
                        set field [randomValue]
                        set value [randomValue]
                        r tset trie $field $value
                        set trie($field) $value
                    } {
                        set field [randomSignedInt 512]
                        set value [randomSignedInt 512]
                        r tset trie $field $value
                        set trie($field) $value
                    } {
                        randpath {
                            set field [randomValue]
                        } {
                            set field [randomSignedInt 512]
                        }
                        r tdel trie $field
                        unset -nocomplain trie($field)
                    }
                }

                # Verify
                foreach {k v} [array get trie] {
                    assert_equal $v [r tget trie $k]
                }
                assert_equal [array size trie] [r tlen trie]
            }
        }
    }
}
