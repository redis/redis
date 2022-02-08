set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {DataType: Test module is sane, GET/SET work.} {
        r datatype.set dtkey 100 stringval
        assert {[r datatype.get dtkey] eq {100 stringval}}
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
}
