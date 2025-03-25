set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules"}} {
    test {DataType: test loadex with invalid config} {
        catch { r module loadex $testmodule CONFIG invalid_config 1 } e
        assert_match {*ERR Error loading the extension*} $e
    }

    r module load $testmodule

    test {DataType: Test module is sane, GET/SET work.} {
        r datatype.set dtkey 100 stringval
        assert {[r datatype.get dtkey] eq {100 stringval}}
    }

    test {test blocking of datatype creation outside of OnLoad} {
        assert_equal [r block.create.datatype.outside.onload] OK
    }

    test {DataType: RM_SaveDataTypeToString(), RM_LoadDataTypeFromStringEncver() work} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]

        assert {[r datatype.restore dtkeycopy $encoded 4] eq {4}}
        assert {[r datatype.get dtkeycopy] eq {-1111 MyString}}
    }

    test {DataType: Handle truncated RM_LoadDataTypeFromStringEncver()} {
        r datatype.set dtkey -1111 MyString
        set encoded [r datatype.dump dtkey]
        set truncated [string range $encoded 0 end-1]

        catch {r datatype.restore dtkeycopy $truncated 4} e
        set e
    } {*Invalid*}

    test {DataType: ModuleTypeReplaceValue() happy path works} {
        r datatype.set key-a 1 AAA
        r datatype.set key-b 2 BBB

        assert {[r datatype.swap key-a key-b] eq {OK}}
        assert {[r datatype.get key-a] eq {2 BBB}}
        assert {[r datatype.get key-b] eq {1 AAA}}
    }

    test {DataType: ModuleTypeReplaceValue() fails on non-module keys} {
        r datatype.set key-a 1 AAA
        r set key-b RedisString

        catch {r datatype.swap key-a key-b} e
        set e
    } {*ERR*}

    test {DataType: Copy command works for modules} {
        # Test failed copies
        r datatype.set answer-to-universe 42 AAA
        catch {r copy answer-to-universe answer2} e
        assert_match {*module key failed to copy*} $e

        # Our module's data type copy function copies the int value as-is
        # but appends /<from-key>/<to-key> to the string value so we can
        # track passed arguments.
        r datatype.set sourcekey 1234 AAA
        r copy sourcekey targetkey
        r datatype.get targetkey
    } {1234 AAA/sourcekey/targetkey}

    test {DataType: Slow Loading} {
        r config set busy-reply-threshold 5000 ;# make sure we're using a high default
        # trigger slow loading
        r datatype.slow_loading 1
        set rd [redis_deferring_client]
        set start [clock clicks -milliseconds]
        $rd debug reload

        # wait till we know we're blocked inside the module
        wait_for_condition 50 100 {
            [r datatype.is_in_slow_loading] eq 1
        } else {
            fail "Failed waiting for slow loading to start"
        }

        # make sure we get LOADING error, and that we didn't get here late (not waiting for busy-reply-threshold)
        assert_error {*LOADING*} {r ping}
        assert_lessthan [expr [clock clicks -milliseconds]-$start] 2000

        # abort the blocking operation
        r datatype.slow_loading 0
        wait_for_condition 50 100 {
            [s loading] eq {0}
        } else {
            fail "Failed waiting for loading to end"
        }
        $rd read
        $rd close
    }

    test {DataType: check the type name} {
        r flushdb
        r datatype.set foo 111 bar
        assert_type test___dt foo
    }

    test {SCAN module datatype} {
        r flushdb
        populate 1000
        r datatype.set foo 111 bar
        set type [r type foo]
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type $type]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1 [llength $keys]    
    }

    test {SCAN module datatype with case sensitive} {
        r flushdb
        populate 1000
        r datatype.set foo 111 bar
        set type "tEsT___dT"
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type $type]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1 [llength $keys]
    }

    if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
        test {Reduce defrag CPU usage when module data can't be defragged} {
            r flushdb
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 25
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 100kb

            # Populate memory with interleaving field of same size.
            set n 20000
            set dummy "[string repeat x 400]"
            set rd [redis_deferring_client]
            for {set i 0} {$i < $n} {incr i} { $rd datatype.set k$i 1 $dummy }
            for {set i 0} {$i < [expr $n]} {incr i} { $rd read } ;# Discard replies

            after 120 ;# serverCron only updates the info once in 100ms
            if {$::verbose} {
                puts "used [s allocator_allocated]"
                puts "rss [s allocator_active]"
                puts "frag [s allocator_frag_ratio]"
                puts "frag_bytes [s allocator_frag_bytes]"
            }
            assert_lessthan [s allocator_frag_ratio] 1.05

            for {set i 0} {$i < $n} {incr i 2} { $rd del k$i }
            for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard del replies
            after 120 ;# serverCron only updates the info once in 100ms
            assert_morethan [s allocator_frag_ratio] 1.4

            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
                # wait for the active defrag to start working (decision once a second)
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }
                assert_morethan [s allocator_frag_ratio] 1.4

                # The cpu usage of defragment will drop to active-defrag-cycle-min
                wait_for_condition 1000 50 {
                    [s active_defrag_running] == 25
                } else {
                    fail "Unable to reduce the defragmentation speed."
                }

                # Fuzzy test to restore defragmentation speed to normal
                set end_time [expr {[clock seconds] + 10}]
                set speed_restored 0
                while {[clock seconds] < $end_time} {
                    for {set i 0} {$i < 500} {incr i} {
                    switch [expr {int(rand() * 3)}] {
                        0 {
                            # Randomly delete a key
                            set random_key [r RANDOMKEY]
                            if {$random_key != ""} {
                                r DEL $random_key
                            }
                        }
                        1 {
                            # Randomly overwrite a key
                            set random_key [r RANDOMKEY]
                            if {$random_key != ""} {
                                r datatype.set $random_key 1 $dummy
                            }
                        }
                        2 {
                            # Randomly generate a new key
                            set random_key "key_[expr {int(rand() * 10000)}]"
                            r datatype.set $random_key 1 $dummy
                        }
                    } ;# end of switch
                    } ;# end of for

                    # Wait for defragmentation speed to restore.
                    if {{[count_log_message $loglines "*Starting active defrag, frag=*%, frag_bytes=*, cpu=5?%*"]} > 1} {
                        set speed_restored 1
                        break;
                    }
                }
                # Make sure the speed is restored
                assert_equal $speed_restored 1

                # After the traffic disappears, the defragmentation speed will decrease again.
                wait_for_condition 1000 50 {
                    [s active_defrag_running] == 25
                } else {
                    fail "Unable to reduce the defragmentation speed after traffic disappears."
                } 
            }
        }
    }
}
