start_server {tags {"string"}} {
    test {SET and GET an item} {
        r set x foobar
        r get x
    } {foobar}

    test {SET and GET an empty item} {
        r set x {}
        r get x
    } {}

    test {Very big payload in GET/SET} {
        set buf [string repeat "abcd" 1000000]
        r set foo $buf
        r get foo
    } [string repeat "abcd" 1000000]

    tags {"slow"} {
        test {Very big payload random access} {
            set err {}
            array set payload {}
            for {set j 0} {$j < 100} {incr j} {
                set size [expr 1+[randomInt 100000]]
                set buf [string repeat "pl-$j" $size]
                set payload($j) $buf
                r set bigpayload_$j $buf
            }
            for {set j 0} {$j < 1000} {incr j} {
                set index [randomInt 100]
                set buf [r get bigpayload_$index]
                if {$buf != $payload($index)} {
                    set err "Values differ: I set '$payload($index)' but I read back '$buf'"
                    break
                }
            }
            unset payload
            set _ $err
        } {}

        test {SET 10000 numeric keys and access all them in reverse order} {
            r flushdb
            set err {}
            for {set x 0} {$x < 10000} {incr x} {
                r set $x $x
            }
            set sum 0
            for {set x 9999} {$x >= 0} {incr x -1} {
                set val [r get $x]
                if {$val ne $x} {
                    set err "Element at position $x is $val instead of $x"
                    break
                }
            }
            set _ $err
        } {}

        test {DBSIZE should be 10000 now} {
            r dbsize
        } {10000}
    }

    test "SETNX target key missing" {
        r del novar
        assert_equal 1 [r setnx novar foobared]
        assert_equal "foobared" [r get novar]
    }

    test "SETNX target key exists" {
        r set novar foobared
        assert_equal 0 [r setnx novar blabla]
        assert_equal "foobared" [r get novar]
    }

    test "SETNX against not-expired volatile key" {
        r set x 10
        r expire x 10000
        assert_equal 0 [r setnx x 20]
        assert_equal 10 [r get x]
    }

    test "SETNX against expired volatile key" {
        # Make it very unlikely for the key this test uses to be expired by the
        # active expiry cycle. This is tightly coupled to the implementation of
        # active expiry and dbAdd() but currently the only way to test that
        # SETNX expires a key when it should have been.
        for {set x 0} {$x < 9999} {incr x} {
            r setex key-$x 3600 value
        }

        # This will be one of 10000 expiring keys. A cycle is executed every
        # 100ms, sampling 10 keys for being expired or not.  This key will be
        # expired for at most 1s when we wait 2s, resulting in a total sample
        # of 100 keys. The probability of the success of this test being a
        # false positive is therefore approx. 1%.
        r set x 10
        r expire x 1

        # Wait for the key to expire
        after 2000

        assert_equal 1 [r setnx x 20]
        assert_equal 20 [r get x]
    } {} {debug_defrag:skip}

    test "GETEX EX option" {
        r del foo
        r set foo bar
        r getex foo ex 10
        assert_range [r ttl foo] 5 10
    }

    test "GETEX PX option" {
        r del foo
        r set foo bar
        r getex foo px 10000
        assert_range [r pttl foo] 5000 10000
    }

    test "GETEX EXAT option" {
        r del foo
        r set foo bar
        r getex foo exat [expr [clock seconds] + 10]
        assert_range [r ttl foo] 5 10
    }

    test "GETEX PXAT option" {
        r del foo
        r set foo bar
        r getex foo pxat [expr [clock milliseconds] + 10000]
        assert_range [r pttl foo] 5000 10000
    }

    test "GETEX PERSIST option" {
        r del foo
        r set foo bar ex 10
        assert_range [r ttl foo] 5 10
        r getex foo persist
        assert_equal -1 [r ttl foo]
    }

    test "GETEX no option" {
        r del foo
        r set foo bar
        r getex foo
        assert_equal bar [r getex foo]
    }

    test "GETEX syntax errors" {
        set ex {}
        catch {r getex foo non-existent-option} ex
        set ex
    } {*syntax*}

    test "GETEX and GET expired key or not exist" {
        r del foo
        r set foo bar px 1
        after 2
        assert_equal {} [r getex foo]
        assert_equal {} [r get foo]
    }

    test "GETEX no arguments" {
         set ex {}
         catch {r getex} ex
         set ex
     } {*wrong number of arguments for 'getex' command}

    test "GETDEL command" {
        r del foo
        r set foo bar
        assert_equal bar [r getdel foo ]
        assert_equal {} [r getdel foo ]
    }

    test {GETDEL propagate as DEL command to replica} {
        set repl [attach_to_replication_stream]
        r set foo bar
        r getdel foo
        assert_replication_stream $repl {
            {select *}
            {set foo bar}
            {del foo}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {GETEX without argument does not propagate to replica} {
        set repl [attach_to_replication_stream]
        r set foo bar
        r getex foo
        r del foo
        assert_replication_stream $repl {
            {select *}
            {set foo bar}
            {del foo}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {MGET} {
        r flushdb
        r set foo{t} BAR
        r set bar{t} FOO
        r mget foo{t} bar{t}
    } {BAR FOO}

    test {MGET against non existing key} {
        r mget foo{t} baazz{t} bar{t}
    } {BAR {} FOO}

    test {MGET against non-string key} {
        r sadd myset{t} ciao
        r sadd myset{t} bau
        r mget foo{t} baazz{t} bar{t} myset{t}
    } {BAR {} FOO {}}

    test {GETSET (set new value)} {
        r del foo
        list [r getset foo xyz] [r get foo]
    } {{} xyz}

    test {GETSET (replace old value)} {
        r set foo bar
        list [r getset foo xyz] [r get foo]
    } {bar xyz}

    test {MSET base case} {
        r mset x{t} 10 y{t} "foo bar" z{t} "x x x x x x x\n\n\r\n"
        r mget x{t} y{t} z{t}
    } [list 10 {foo bar} "x x x x x x x\n\n\r\n"]

    test {MSET/MSETNX wrong number of args} {
        assert_error {*wrong number of arguments for 'mset' command} {r mset x{t} 10 y{t} "foo bar" z{t}}
        assert_error {*wrong number of arguments for 'msetnx' command} {r msetnx x{t} 20 y{t} "foo bar" z{t}}
    }

    test {MSET with already existing - same key twice} {
        r set x{t} x
        list [r mset x{t} xxx x{t} yyy] [r get x{t}]
    } {OK yyy}

    test {MSETNX with already existent key} {
        list [r msetnx x1{t} xxx y2{t} yyy x{t} 20] [r exists x1{t}] [r exists y2{t}]
    } {0 0 0}

    test {MSETNX with not existing keys} {
        list [r msetnx x1{t} xxx y2{t} yyy] [r get x1{t}] [r get y2{t}]
    } {1 xxx yyy}

    test {MSETNX with not existing keys - same key twice} {
        r del x1{t}
        list [r msetnx x1{t} xxx x1{t} yyy] [r get x1{t}]
    } {1 yyy}

    test {MSETNX with already existing keys - same key twice} {
        list [r msetnx x1{t} xxx x1{t} zzz] [r get x1{t}]
    } {0 yyy}

    test {MSETEX - all expiration flags} {
        # Test each expiration type separately (EX, PX, EXAT, PXAT)
        set future_sec [expr [clock seconds] + 10]
        set future_ms [expr [clock milliseconds] + 10000]

        # Test EX and PX with separate commands (each applies to all keys in that command)
        r msetex 2 ex:key1{t} val1 ex:key2{t} val2 ex 5
        r msetex 2 px:key1{t} val1 px:key2{t} val2 px 5000

        # Test EXAT and PXAT with separate commands
        r msetex 2 exat:key1{t} val3 exat:key2{t} val4 exat $future_sec
        r msetex 2 pxat:key1{t} val3 pxat:key2{t} val4 pxat $future_ms

        assert_morethan [r ttl ex:key1{t}] 0
        assert_morethan [r pttl px:key1{t}] 0
        assert_morethan [r ttl exat:key1{t}] 0
        assert_morethan [r pttl pxat:key1{t}] 0
    }

    test {MSETEX - KEEPTTL preserves existing TTL} {
        r setex keepttl:key{t} 100 oldval
        set old_ttl [r ttl keepttl:key{t}]
        r msetex 1 keepttl:key{t} newval keepttl
        assert_equal [r get keepttl:key{t}] "newval"
        assert_morethan [r ttl keepttl:key{t}] [expr $old_ttl - 5]
    }

    test {MSETEX - NX/XX conditions and return values} {
        r del nx:new{t} nx:new2{t} xx:existing{t} xx:nonexist{t}
        r set xx:existing{t} oldval

        assert_equal [r msetex 2 nx:new{t} val1 nx:new2{t} val2 nx ex 10] 1
        assert_equal [r msetex 1 xx:existing{t} newval nx ex 10] 0
        assert_equal [r msetex 1 xx:nonexist{t} newval xx ex 10] 0
        assert_equal [r msetex 1 xx:existing{t} newval xx ex 10] 1
        assert_equal [r get nx:new{t}] "val1"
        assert_equal [r get xx:existing{t}] "newval"
    }

    test {MSETEX - flexible argument parsing} {
        r del flex:1{t} flex:2{t}
        # Test flags before and after KEYS
        r msetex 2 flex:1{t} val1 flex:2{t} val2 ex 3 nx 
        r msetex 2 flex:3{t} val3 flex:4{t} val4 px 3000 xx

        assert_equal [r get flex:1{t}] "val1"
        assert_equal [r get flex:2{t}] "val2"
        assert_morethan [r ttl flex:1{t}] 0
        assert_equal [r exists flex:3{t}] 0
        assert_equal [r exists flex:4{t}] 0
    }

    test {MSETEX - error cases} {
        assert_error {*wrong number of arguments*} {r msetex}
        assert_error {*invalid numkeys value*} {r msetex key1 val1 ex 10}
        assert_error {*wrong number of key-value pairs*} {r msetex 2 key1 val1 key2}
        assert_error {*syntax error*} {r msetex 1 key1 val1 invalid_flag}
    }

    test {MSETEX - overflow protection in numkeys} {
        # Test that large numkeys values don't cause integer overflow
        # This tests the fix for potential overflow in kv_count_long * 2
        assert_error {*invalid numkeys value*} {r msetex 2147483648 key1 val1 ex 10}
        assert_error {*wrong number of key-value pairs*} {r msetex 2147483647 key1 val1 ex 10}
    }

    test {MSETEX - mutually exclusive flags} {
        # NX and XX are mutually exclusive
        assert_error {*syntax error*} {r msetex 2 key1{t} val1 key2{t} val2 nx xx ex 10}

        # Multiple expiration flags are mutually exclusive
        assert_error {*syntax error*} {r msetex 2 key1{t} val1 key2{t} val2 ex 10 px 5000}
        assert_error {*syntax error*} {r msetex 2 key1{t} val1 key2{t} val2 exat 1735689600 pxat 1735689600000}

        # KEEPTTL conflicts with expiration flags
        assert_error {*syntax error*} {r msetex 2 key1{t} val1 key2{t} val2 keepttl ex 10}
        assert_error {*syntax error*} {r msetex 2 key1{t} val1 key2{t} val2 keepttl px 5000}
    }

    test "STRLEN against non-existing key" {
        assert_equal 0 [r strlen notakey]
    }

    test "STRLEN against integer-encoded value" {
        r set myinteger -555
        assert_equal 4 [r strlen myinteger]
    }

    test "STRLEN against plain string" {
        r set mystring "foozzz0123456789 baz"
        assert_equal 20 [r strlen mystring]
    }

    test "SETBIT against non-existing key" {
        r del mykey
        assert_equal 0 [r setbit mykey 1 1]
        assert_equal [binary format B* 01000000] [r get mykey]
    }

    test "SETBIT against string-encoded key" {
        # Ascii "@" is integer 64 = 01 00 00 00
        r set mykey "@"

        assert_equal 0 [r setbit mykey 2 1]
        assert_equal [binary format B* 01100000] [r get mykey]
        assert_equal 1 [r setbit mykey 1 0]
        assert_equal [binary format B* 00100000] [r get mykey]
    }

    test "SETBIT against integer-encoded key" {
        # Ascii "1" is integer 49 = 00 11 00 01
        r set mykey 1
        assert_encoding int mykey

        assert_equal 0 [r setbit mykey 6 1]
        assert_equal [binary format B* 00110011] [r get mykey]
        assert_equal 1 [r setbit mykey 2 0]
        assert_equal [binary format B* 00010011] [r get mykey]
    }

    test "SETBIT against key with wrong type" {
        r del mykey
        r lpush mykey "foo"
        assert_error "WRONGTYPE*" {r setbit mykey 0 1}
    }

    test "SETBIT with out of range bit offset" {
        r del mykey
        assert_error "*out of range*" {r setbit mykey [expr 4*1024*1024*1024] 1}
        assert_error "*out of range*" {r setbit mykey -1 1}
    }

    test "SETBIT with non-bit argument" {
        r del mykey
        assert_error "*out of range*" {r setbit mykey 0 -1}
        assert_error "*out of range*" {r setbit mykey 0  2}
        assert_error "*out of range*" {r setbit mykey 0 10}
        assert_error "*out of range*" {r setbit mykey 0 20}
    }

    test "SETBIT fuzzing" {
        set str ""
        set len [expr 256*8]
        r del mykey

        for {set i 0} {$i < 2000} {incr i} {
            set bitnum [randomInt $len]
            set bitval [randomInt 2]
            set fmt [format "%%-%ds%%d%%-s" $bitnum]
            set head [string range $str 0 $bitnum-1]
            set tail [string range $str $bitnum+1 end]
            set str [string map {" " 0} [format $fmt $head $bitval $tail]]

            r setbit mykey $bitnum $bitval
            assert_equal [binary format B* $str] [r get mykey]
        }
    }

    test "GETBIT against non-existing key" {
        r del mykey
        assert_equal 0 [r getbit mykey 0]
    }

    test "GETBIT against string-encoded key" {
        # Single byte with 2nd and 3rd bit set
        r set mykey "`"

        # In-range
        assert_equal 0 [r getbit mykey 0]
        assert_equal 1 [r getbit mykey 1]
        assert_equal 1 [r getbit mykey 2]
        assert_equal 0 [r getbit mykey 3]

        # Out-range
        assert_equal 0 [r getbit mykey 8]
        assert_equal 0 [r getbit mykey 100]
        assert_equal 0 [r getbit mykey 10000]
    }

    test "GETBIT against integer-encoded key" {
        r set mykey 1
        assert_encoding int mykey

        # Ascii "1" is integer 49 = 00 11 00 01
        assert_equal 0 [r getbit mykey 0]
        assert_equal 0 [r getbit mykey 1]
        assert_equal 1 [r getbit mykey 2]
        assert_equal 1 [r getbit mykey 3]

        # Out-range
        assert_equal 0 [r getbit mykey 8]
        assert_equal 0 [r getbit mykey 100]
        assert_equal 0 [r getbit mykey 10000]
    }

    test "SETRANGE against non-existing key" {
        r del mykey
        assert_equal 3 [r setrange mykey 0 foo]
        assert_equal "foo" [r get mykey]

        r del mykey
        assert_equal 0 [r setrange mykey 0 ""]
        assert_equal 0 [r exists mykey]

        r del mykey
        assert_equal 4 [r setrange mykey 1 foo]
        assert_equal "\000foo" [r get mykey]
    }

    test "SETRANGE against string-encoded key" {
        r set mykey "foo"
        assert_equal 3 [r setrange mykey 0 b]
        assert_equal "boo" [r get mykey]

        r set mykey "foo"
        assert_equal 3 [r setrange mykey 0 ""]
        assert_equal "foo" [r get mykey]

        r set mykey "foo"
        assert_equal 3 [r setrange mykey 1 b]
        assert_equal "fbo" [r get mykey]

        r set mykey "foo"
        assert_equal 7 [r setrange mykey 4 bar]
        assert_equal "foo\000bar" [r get mykey]
    }

    test "SETRANGE against integer-encoded key" {
        r set mykey 1234
        assert_encoding int mykey
        assert_equal 4 [r setrange mykey 0 2]
        assert_encoding raw mykey
        assert_equal 2234 [r get mykey]

        # Shouldn't change encoding when nothing is set
        r set mykey 1234
        assert_encoding int mykey
        assert_equal 4 [r setrange mykey 0 ""]
        assert_encoding int mykey
        assert_equal 1234 [r get mykey]

        r set mykey 1234
        assert_encoding int mykey
        assert_equal 4 [r setrange mykey 1 3]
        assert_encoding raw mykey
        assert_equal 1334 [r get mykey]

        r set mykey 1234
        assert_encoding int mykey
        assert_equal 6 [r setrange mykey 5 2]
        assert_encoding raw mykey
        assert_equal "1234\0002" [r get mykey]
    }

    test "SETRANGE against key with wrong type" {
        r del mykey
        r lpush mykey "foo"
        assert_error "WRONGTYPE*" {r setrange mykey 0 bar}
    }

    test "SETRANGE with out of range offset" {
        r del mykey
        assert_error "*maximum allowed size*" {r setrange mykey [expr 512*1024*1024-4] world}

        r set mykey "hello"
        assert_error "*out of range*" {r setrange mykey -1 world}
        assert_error "*maximum allowed size*" {r setrange mykey [expr 512*1024*1024-4] world}
    }

    test "GETRANGE against non-existing key" {
        r del mykey
        assert_equal "" [r getrange mykey 0 -1]
    }

    test "GETRANGE against wrong key type" {
        r lpush lkey1 "list"
        assert_error {WRONGTYPE Operation against a key holding the wrong kind of value*} {r getrange lkey1 0 -1}
    }

    test "GETRANGE against string value" {
        r set mykey "Hello World"
        assert_equal "Hell" [r getrange mykey 0 3]
        assert_equal "Hello World" [r getrange mykey 0 -1]
        assert_equal "orld" [r getrange mykey -4 -1]
        assert_equal "" [r getrange mykey 5 3]
        assert_equal " World" [r getrange mykey 5 5000]
        assert_equal "Hello World" [r getrange mykey -5000 10000]
        assert_equal "H" [r getrange mykey 0 -100]
        assert_equal "" [r getrange mykey 1 -100]
        assert_equal "" [r getrange mykey -1 -100]
        assert_equal "H" [r getrange mykey -100 -99]
        assert_equal "H" [r getrange mykey -100 -100]
        assert_equal "" [r getrange mykey -100 -101]
    }

    test "GETRANGE against integer-encoded value" {
        r set mykey 1234
        assert_equal "123" [r getrange mykey 0 2]
        assert_equal "1234" [r getrange mykey 0 -1]
        assert_equal "234" [r getrange mykey -3 -1]
        assert_equal "" [r getrange mykey 5 3]
        assert_equal "4" [r getrange mykey 3 5000]
        assert_equal "1234" [r getrange mykey -5000 10000]
        assert_equal "1" [r getrange mykey 0 -100]
        assert_equal "" [r getrange mykey 1 -100]
        assert_equal "" [r getrange mykey -1 -100]
        assert_equal "1" [r getrange mykey -100 -99]
        assert_equal "1" [r getrange mykey -100 -100]
        assert_equal "" [r getrange mykey -100 -101]
    }

    test "GETRANGE fuzzing" {
        for {set i 0} {$i < 1000} {incr i} {
            r set bin [set bin [randstring 0 1024 binary]]
            set _start [set start [randomInt 1500]]
            set _end [set end [randomInt 1500]]
            if {$_start < 0} {set _start "end-[abs($_start)-1]"}
            if {$_end < 0} {set _end "end-[abs($_end)-1]"}
            assert_equal [string range $bin $_start $_end] [r getrange bin $start $end]
        }
    }

    test "Coverage: SUBSTR" {
        r set key abcde
        assert_equal "a" [r substr key 0 0]
        assert_equal "abcd" [r substr key 0 3]
        assert_equal "bcde" [r substr key -4 -1]
        assert_equal "" [r substr key -1 -3]
        assert_equal "" [r substr key 7 8]
        assert_equal "" [r substr nokey 0 1]
    }
    
if {[string match {*jemalloc*} [s mem_allocator]]} {
    test {trim on SET with big value} {
        # set a big value to trigger increasing the query buf
        r set key [string repeat A 100000] 
        # set a smaller value but > PROTO_MBULK_BIG_ARG (32*1024) Redis will try to save the query buf itself on the DB.
        r set key [string repeat A 33000]
        # asset the value was trimmed
        assert {[r memory usage key] < 42000}; # 42K to count for Jemalloc's additional memory overhead. 
    }
} ;# if jemalloc

    test {Extended SET can detect syntax errors} {
        set e {}
        catch {r set foo bar non-existing-option} e
        set e
    } {*syntax*}

    test {Extended SET NX option} {
        r del foo
        set v1 [r set foo 1 nx]
        set v2 [r set foo 2 nx]
        list $v1 $v2 [r get foo]
    } {OK {} 1}

    test {Extended SET XX option} {
        r del foo
        set v1 [r set foo 1 xx]
        r set foo bar
        set v2 [r set foo 2 xx]
        list $v1 $v2 [r get foo]
    } {{} OK 2}

    test {Extended SET GET option} {
        r del foo
        r set foo bar
        set old_value [r set foo bar2 GET]
        set new_value [r get foo]
        list $old_value $new_value
    } {bar bar2}

    test {Extended SET GET option with no previous value} {
        r del foo
        set old_value [r set foo bar GET]
        set new_value [r get foo]
        list $old_value $new_value
    } {{} bar}

    test {Extended SET GET option with XX} {
        r del foo
        r set foo bar
        set old_value [r set foo baz GET XX]
        set new_value [r get foo]
        list $old_value $new_value
    } {bar baz}

    test {Extended SET GET option with XX and no previous value} {
        r del foo
        set old_value [r set foo bar GET XX]
        set new_value [r get foo]
        list $old_value $new_value
    } {{} {}}

    test {Extended SET GET option with NX} {
        r del foo
        set old_value [r set foo bar GET NX]
        set new_value [r get foo]
        list $old_value $new_value
    } {{} bar}

    test {Extended SET GET option with NX and previous value} {
        r del foo
        r set foo bar
        set old_value [r set foo baz GET NX]
        set new_value [r get foo]
        list $old_value $new_value
    } {bar bar}

    test {Extended SET GET with incorrect type should result in wrong type error} {
      r del foo
      r rpush foo waffle
      catch {r set foo bar GET} err1
      assert_equal "waffle" [r rpop foo]
      set err1
    } {*WRONGTYPE*}

    test {Extended SET EX option} {
        r del foo
        r set foo bar ex 10
        set ttl [r ttl foo]
        assert {$ttl <= 10 && $ttl > 5}
    }

    test {Extended SET PX option} {
        r del foo
        r set foo bar px 10000
        set ttl [r ttl foo]
        assert {$ttl <= 10 && $ttl > 5}
    }

    test "Extended SET EXAT option" {
        r del foo
        r set foo bar exat [expr [clock seconds] + 10]
        assert_range [r ttl foo] 5 10
    }

    test "Extended SET PXAT option" {
        r del foo
        r set foo bar pxat [expr [clock milliseconds] + 10000]
        assert_range [r ttl foo] 5 10
    }
    test {Extended SET using multiple options at once} {
        r set foo val
        assert {[r set foo bar xx px 10000] eq {OK}}
        set ttl [r ttl foo]
        assert {$ttl <= 10 && $ttl > 5}
    }

    test {GETRANGE with huge ranges, Github issue #1844} {
        r set foo bar
        r getrange foo 0 4294967297
    } {bar}

    set rna1 {CACCTTCCCAGGTAACAAACCAACCAACTTTCGATCTCTTGTAGATCTGTTCTCTAAACGAACTTTAAAATCTGTGTGGCTGTCACTCGGCTGCATGCTTAGTGCACTCACGCAGTATAATTAATAACTAATTACTGTCGTTGACAGGACACGAGTAACTCGTCTATCTTCTGCAGGCTGCTTACGGTTTCGTCCGTGTTGCAGCCGATCATCAGCACATCTAGGTTTCGTCCGGGTGTG}
    set rna2 {ATTAAAGGTTTATACCTTCCCAGGTAACAAACCAACCAACTTTCGATCTCTTGTAGATCTGTTCTCTAAACGAACTTTAAAATCTGTGTGGCTGTCACTCGGCTGCATGCTTAGTGCACTCACGCAGTATAATTAATAACTAATTACTGTCGTTGACAGGACACGAGTAACTCGTCTATCTTCTGCAGGCTGCTTACGGTTTCGTCCGTGTTGCAGCCGATCATCAGCACATCTAGGTTT}
    set rnalcs {ACCTTCCCAGGTAACAAACCAACCAACTTTCGATCTCTTGTAGATCTGTTCTCTAAACGAACTTTAAAATCTGTGTGGCTGTCACTCGGCTGCATGCTTAGTGCACTCACGCAGTATAATTAATAACTAATTACTGTCGTTGACAGGACACGAGTAACTCGTCTATCTTCTGCAGGCTGCTTACGGTTTCGTCCGTGTTGCAGCCGATCATCAGCACATCTAGGTTT}

    test {LCS basic} {
        r set virus1{t} $rna1
        r set virus2{t} $rna2
        r LCS virus1{t} virus2{t}
    } $rnalcs

    test {LCS len} {
        r set virus1{t} $rna1
        r set virus2{t} $rna2
        r LCS virus1{t} virus2{t} LEN
    } [string length $rnalcs]

    test {LCS indexes} {
        dict get [r LCS virus1{t} virus2{t} IDX] matches
    } {{{238 238} {239 239}} {{236 236} {238 238}} {{229 230} {236 237}} {{224 224} {235 235}} {{1 222} {13 234}}}

    test {LCS indexes with match len} {
        dict get [r LCS virus1{t} virus2{t} IDX WITHMATCHLEN] matches
    } {{{238 238} {239 239} 1} {{236 236} {238 238} 1} {{229 230} {236 237} 2} {{224 224} {235 235} 1} {{1 222} {13 234} 222}}

    test {LCS indexes with match len and minimum match len} {
        dict get [r LCS virus1{t} virus2{t} IDX WITHMATCHLEN MINMATCHLEN 5] matches
    } {{{1 222} {13 234} 222}}

    test {SETRANGE with huge offset} {
        foreach value {9223372036854775807 2147483647} {
            catch {[r setrange K $value A]} res
            # expecting a different error on 32 and 64 bit systems
            if {![string match "*string exceeds maximum allowed size*" $res] && ![string match "*out of range*" $res]} {
                assert_equal $res "expecting an error"
           }
        }
    }

    test {APPEND modifies the encoding from int to raw} {
        r del foo
        r set foo 1
        assert_encoding "int" foo
        r append foo 2

        set res {}
        lappend res [r get foo]
        assert_encoding "raw" foo
        
        r set bar 12
        assert_encoding "int" bar
        lappend res [r get bar]
    } {12 12}
    
    # coverage for kvobjComputeSize
    test {MEMORY USAGE - STRINGS} {
        set sizes {1 5 8 15 16 17 31 32 33 63 64 65 127 128 129 255 256 257}
        set hdrsize [expr {[s arch_bits] == 32 ? 12 : 16}]
        
        foreach ksize $sizes {
            set key [string repeat "k" $ksize]
            # OBJ_ENCODING_EMBSTR, OBJ_ENCODING_RAW        
            foreach vsize $sizes {
                set value [string repeat "v" $vsize]                        
                r set $key $value
                set memory_used [r memory usage $key]
                set min [expr $hdrsize + $ksize + $vsize] 
                assert_lessthan_equal $min $memory_used
                set max [expr {32 > $min ? 64 : [expr $min * 2]}]
                assert_morethan_equal $max $memory_used
            }
            
            # OBJ_ENCODING_INT
            foreach value {1 100 10000 10000000} {
                r set $key $value
                set min [expr $hdrsize + $ksize]
                assert_lessthan_equal $min [r memory usage $key]
            }
        }
    }
    
    if {[string match {*jemalloc*} [s mem_allocator]]} {
        test {Check MEMORY USAGE for embedded key strings with jemalloc} {
        
            proc expected_mem {key val with_expire exp_mem_usage exp_debug_sdslen} {
                r del $key
                r set $key $val
                if {$with_expire} { r expire $key 5678315 }
                assert_equal $exp_mem_usage [r memory usage $key]
                assert_equal $exp_debug_sdslen [r debug sdslen $key]
            }
            
            if {[s arch_bits] == 64} {  
                # 16 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 5 (val) + 1 (\0) = 32bytes
                expected_mem x234 y2345 0 32 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 32, val_sds_len:5, val_sds_avail:0, val_zmalloc: 0"
                # 16 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 6 (val) + 1 (\0) = 33bytes
                expected_mem x234 y23456 0 40 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 40, val_sds_len:6, val_sds_avail:7, val_zmalloc: 0"
                # 16 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 13 (val) + 1 (\0) = 40bytes
                expected_mem x234 y234561234567 0 40 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 40, val_sds_len:13, val_sds_avail:0, val_zmalloc: 0"
                # 16 (kvobj) + 8 (expiry) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 13 (val) + 1 (\0) = 48bytes
                expected_mem x234 y234561234567 1 48 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 48, val_sds_len:13, val_sds_avail:0, val_zmalloc: 0"
            } else {
                # 12 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 9 (val) + 1 (\0) = 32bytes
                expected_mem x234 y23456789 0 32 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 32, val_sds_len:9, val_sds_avail:0, val_zmalloc: 0"
                # 12 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 10 (val) + 1 (\0) = 33bytes
                expected_mem x234 y234567890 0 40 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 40, val_sds_len:10, val_sds_avail:7, val_zmalloc: 0"
                # 12 (kvobj) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 17 (val) + 1 (\0) = 40bytes 
                expected_mem x234 y2345678901234567 0 40 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 40, val_sds_len:17, val_sds_avail:0, val_zmalloc: 0"
                # 12 (kvobj) + 8 (expiry) + 1 (key-hdr-size) + 1 (sdshdr5) + 4 (key) + 1 (\0) + 3 (sdshdr8) + 17 (val) + 1 (\0) = 48bytes
                expected_mem x234 y2345678901234567 1 48 "key_sds_len:4, key_sds_avail:0, key_zmalloc: 48, val_sds_len:17, val_sds_avail:0, val_zmalloc: 0"
            }
        } {} {needs:debug}
    }

    test {DIGEST basic usage with plain string} {
        r set mykey "hello world"
        set digest [r digest mykey]
        # Ensure reply is hex string
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST with empty string} {
        r set mykey ""
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST with integer-encoded value} {
        r set mykey 12345
        assert_encoding int mykey
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST with negative integer} {
        r set mykey -999
        assert_encoding int mykey
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST returns consistent hash for same value} {
        r set mykey "test string"
        set digest1 [r digest mykey]
        set digest2 [r digest mykey]
        assert_equal $digest1 $digest2
    }

    test {DIGEST returns same hash for same content in different keys} {
        r set key1 "identical"
        r set key2 "identical"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert_equal $digest1 $digest2
    }

    test {DIGEST returns different hash for different values} {
        r set key1 "value1"
        r set key2 "value2"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert {$digest1 != $digest2}
    }

    test {DIGEST with binary data} {
        r set mykey "\x00\x01\x02\x03\xff\xfe"
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST with unicode characters} {
        r set mykey "Hello 世界"
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST with very long string} {
        set longstring [string repeat "Lorem ipsum dolor sit amet. " 1000]
        r set mykey $longstring
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST against non-existing key} {
        r del nonexistent
        assert_equal {} [r digest nonexistent]
    }

    test {DIGEST against wrong type (list)} {
        r del mylist
        r lpush mylist "element"
        assert_error "*WRONGTYPE*" {r digest mylist}
    }

    test {DIGEST against wrong type (hash)} {
        r del myhash
        r hset myhash field value
        assert_error "*WRONGTYPE*" {r digest myhash}
    }

    test {DIGEST against wrong type (set)} {
        r del myset
        r sadd myset member
        assert_error "*WRONGTYPE*" {r digest myset}
    }

    test {DIGEST against wrong type (zset)} {
        r del myzset
        r zadd myzset 1 member
        assert_error "*WRONGTYPE*" {r digest myzset}
    }

    test {DIGEST wrong number of arguments} {
        assert_error "*wrong number of arguments*" {r digest}
        assert_error "*wrong number of arguments*" {r digest key1 key2}
    }

    test {DIGEST with special characters and whitespace} {
        r set mykey "  spaces  \t\n\r"
        set digest [r digest mykey]
        assert {[string is wideinteger -strict "0x$digest"]}
    }

    test {DIGEST consistency across SET operations} {
        r set mykey "original"
        set digest1 [r digest mykey]

        r set mykey "changed"
        set digest2 [r digest mykey]
        assert {$digest1 != $digest2}

        r set mykey "original"
        set digest3 [r digest mykey]
        assert_equal $digest1 $digest3
    }

    test {DELEX basic usage without conditions} {
        r set mykey "hello"
        assert_equal 1 [r delex mykey]

        r hset myhash f v
        assert_equal 1 [r delex myhash]

        r zadd mystr 1 m
        assert_equal 1 [r delex mystr]
    }

    test {DELEX basic usage with IFEQ} {
        r set mykey "hello"
        assert_equal 1 [r delex mykey IFEQ "hello"]
        assert_equal 0 [r exists mykey]

        r set mykey "hello"
        assert_equal 0 [r delex mykey IFEQ "world"]
        assert_equal 1 [r exists mykey]
        assert_equal "hello" [r get mykey]
    }

    test {DELEX basic usage with IFNE} {
        r set mykey "hello"
        assert_equal 1 [r delex mykey IFNE "world"]
        assert_equal 0 [r exists mykey]

        r set mykey "hello"
        assert_equal 0 [r delex mykey IFNE "hello"]
        assert_equal 1 [r exists mykey]
        assert_equal "hello" [r get mykey]
    }

    test {DELEX basic usage with IFDEQ} {
        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal 1 [r delex mykey IFDEQ $digest]
        assert_equal 0 [r exists mykey]

        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal 0 [r delex mykey IFDEQ $wrong_digest]
        assert_equal 1 [r exists mykey]
        assert_equal "hello" [r get mykey]
    }

    test {DELEX basic usage with IFDNE} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal 1 [r delex mykey IFDNE $wrong_digest]
        assert_equal 0 [r exists mykey]

        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal 0 [r delex mykey IFDNE $digest]
        assert_equal 1 [r exists mykey]
        assert_equal "hello" [r get mykey]
    }

    test {DELEX with non-existing key} {
        r del nonexistent
        assert_equal 0 [r delex nonexistent IFEQ "hello"]
        assert_equal 0 [r delex nonexistent IFNE "hello"]
        assert_equal 0 [r delex nonexistent IFDEQ 1234567890]
        assert_equal 0 [r delex nonexistent IFDNE 1234567890]
    }

    test {DELEX with empty string value} {
        r set mykey ""
        assert_equal 1 [r delex mykey IFEQ ""]
        assert_equal 0 [r exists mykey]

        r set mykey ""
        assert_equal 0 [r delex mykey IFEQ "notempty"]
        assert_equal 1 [r exists mykey]
    }

    test {DELEX with integer-encoded value} {
        r set mykey 12345
        assert_encoding int mykey
        assert_equal 1 [r delex mykey IFEQ "12345"]
        assert_equal 0 [r exists mykey]

        r set mykey 12345
        assert_encoding int mykey
        assert_equal 0 [r delex mykey IFEQ "54321"]
        assert_equal 1 [r exists mykey]
    }

    test {DELEX with negative integer} {
        r set mykey -999
        assert_encoding int mykey
        assert_equal 1 [r delex mykey IFEQ "-999"]
        assert_equal 0 [r exists mykey]
    }

    test {DELEX with binary data} {
        r set mykey "\x00\x01\x02\x03\xff\xfe"
        assert_equal 1 [r delex mykey IFEQ "\x00\x01\x02\x03\xff\xfe"]
        assert_equal 0 [r exists mykey]

        r set mykey "\x00\x01\x02\x03\xff\xfe"
        assert_equal 0 [r delex mykey IFEQ "\x00\x01\x02\x03\xff\xff"]
        assert_equal 1 [r exists mykey]
    }

    test {DELEX with unicode characters} {
        r set mykey "Hello 世界"
        assert_equal 1 [r delex mykey IFEQ "Hello 世界"]
        assert_equal 0 [r exists mykey]

        r set mykey "Hello 世界"
        assert_equal 0 [r delex mykey IFEQ "Hello World"]
        assert_equal 1 [r exists mykey]
    }

    test {DELEX with very long string} {
        set longstring [string repeat "Lorem ipsum dolor sit amet. " 1000]
        r set mykey $longstring
        assert_equal 1 [r delex mykey IFEQ $longstring]
        assert_equal 0 [r exists mykey]
    }

    test {DELEX against wrong type} {
        r del mylist
        r lpush mylist "element"
        assert_error "*ERR*" {r delex mylist IFEQ "element"}

        r del myhash
        r hset myhash field value
        assert_error "*ERR*" {r delex myhash IFEQ "value"}

        r del myset
        r sadd myset member
        assert_error "*ERR*" {r delex myset IFEQ "member"}

        r del myzset
        r zadd myzset 1 member
        assert_error "*ERR*" {r delex myzset IFEQ "member"}
    }

    test {DELEX wrong number of arguments} {
        r del key1
        assert_error "*wrong number of arguments*" {r delex key1 IFEQ}
   
        r set key1 x
        assert_error "*wrong number of arguments*" {r delex key1 IFEQ}
        assert_error "*wrong number of arguments*" {r delex key1 IFEQ value1 extra}
    }

    test {DELEX invalid condition} {
        r set mykey "hello"
        assert_error "*Invalid condition*" {r delex mykey INVALID "hello"}
        assert_error "*Invalid condition*" {r delex mykey IF "hello"}
        assert_error "*Invalid condition*" {r delex mykey EQ "hello"}
    }

    test {DELEX with special characters and whitespace} {
        r set mykey "  spaces  \t\n\r"
        assert_equal 1 [r delex mykey IFEQ "  spaces  \t\n\r"]
        assert_equal 0 [r exists mykey]
    }

    test {DELEX digest consistency with same content} {
        r set key1 "identical"
        r set key2 "identical"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert_equal $digest1 $digest2

        # Both should be deletable with the same digest
        assert_equal 1 [r delex key1 IFDEQ $digest2]
        assert_equal 1 [r delex key2 IFDEQ $digest1]
    }

    test {DELEX digest with different content} {
        r set key1 "value1"
        r set key2 "value2"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert {$digest1 != $digest2}

        # Should not be able to delete with wrong digest
        assert_equal 0 [r delex key1 IFDEQ $digest2]
        assert_equal 0 [r delex key2 IFDEQ $digest1]

        # Should be able to delete with correct digest
        assert_equal 1 [r delex key1 IFDEQ $digest1]
        assert_equal 1 [r delex key2 IFDEQ $digest2]
    }

    test {DELEX propagate as DEL command to replica} {
        r flushall
        set repl [attach_to_replication_stream]
        r set foo bar
        r delex foo IFEQ bar
        assert_replication_stream $repl {
            {select *}
            {set foo bar}
            {del foo}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {DELEX does not propagate when condition not met} {
        r flushall
        set repl [attach_to_replication_stream]
        r set foo bar
        r delex foo IFEQ baz
        r set foo bar2
        assert_replication_stream $repl {
            {select *}
            {set foo bar}
            {set foo bar2}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {DELEX with integer that looks like string} {
        # Set as integer
        r set key1 123
        assert_encoding int key1
        assert_equal 1 [r delex key1 IFEQ "123"]
        assert_equal 0 [r exists key1]

        # Set as string
        r set key2 "123"
        assert_equal 1 [r delex key2 IFEQ "123"]
        assert_equal 0 [r exists key2]
    }

    test {Extended SET with IFEQ - key exists and matches} {
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IFEQ "hello"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFEQ - key exists but doesn't match} {
        r set mykey "hello"
        assert_equal {} [r set mykey "world" IFEQ "different"]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFEQ - key doesn't exist} {
        r del mykey
        assert_equal {} [r set mykey "world" IFEQ "hello"]
        assert_equal 0 [r exists mykey]
    }

    test {Extended SET with IFNE - key exists and doesn't match} {
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IFNE "different"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFNE - key exists and matches} {
        r set mykey "hello"
        assert_equal {} [r set mykey "world" IFNE "hello"]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFNE - key doesn't exist} {
        r del mykey
        assert_equal "OK" [r set mykey "world" IFNE "hello"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDEQ - key exists and digest matches} {
        r set mykey "hello"
        set digest [r digest mykey]
        puts $digest
        assert_equal "OK" [r set mykey "world" IFDEQ $digest]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDEQ - key exists but digest doesn't match} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal {} [r set mykey "world" IFDEQ $wrong_digest]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFDEQ - key doesn't exist} {
        r del mykey
        set digest 1234567890
        assert_equal {} [r set mykey "world" IFDEQ $digest]
        assert_equal 0 [r exists mykey]
    }

    test {Extended SET with IFDNE - key exists and digest doesn't match} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal "OK" [r set mykey "world" IFDNE $wrong_digest]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDNE - key exists and digest matches} {
        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal {} [r set mykey "world" IFDNE $digest]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFDNE - key doesn't exist} {
        r del mykey
        set digest 1234567890
        assert_equal "OK" [r set mykey "world" IFDNE $digest]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFEQ and GET - key exists and matches} {
        r set mykey "hello"
        assert_equal "hello" [r set mykey "world" IFEQ "hello" GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFEQ and GET - key exists but doesn't match} {
        r set mykey "hello"
        assert_equal "hello" [r set mykey "world" IFEQ "different" GET]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFEQ and GET - key doesn't exist} {
        r del mykey
        assert_equal {} [r set mykey "world" IFEQ "hello" GET]
        assert_equal 0 [r exists mykey]
    }

    test {Extended SET with IFNE and GET - key exists and doesn't match} {
        r set mykey "hello"
        assert_equal "hello" [r set mykey "world" IFNE "different" GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFNE and GET - key exists and matches} {
        r set mykey "hello"
        assert_equal "hello" [r set mykey "world" IFNE "hello" GET]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFNE and GET - key doesn't exist} {
        r del mykey
        assert_equal {} [r set mykey "world" IFNE "hello" GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDEQ and GET - key exists and digest matches} {
        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal "hello" [r set mykey "world" IFDEQ $digest GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDEQ and GET - key exists but digest doesn't match} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal "hello" [r set mykey "world" IFDEQ $wrong_digest GET]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFDEQ and GET - key doesn't exist} {
        r del mykey
        set digest 1234567890
        assert_equal {} [r set mykey "world" IFDEQ $digest GET]
        assert_equal 0 [r exists mykey]
    }

    test {Extended SET with IFDNE and GET - key exists and digest doesn't match} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal "hello" [r set mykey "world" IFDNE $wrong_digest GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFDNE and GET - key exists and digest matches} {
        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal "hello" [r set mykey "world" IFDNE $digest GET]
        assert_equal "hello" [r get mykey]
    }

    test {Extended SET with IFDNE and GET - key doesn't exist} {
        r del mykey
        set digest 1234567890
        assert_equal {} [r set mykey "world" IFDNE $digest GET]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with IFEQ and expiration} {
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IFEQ "hello" EX 10]
        assert_equal "world" [r get mykey]
        assert_range [r ttl mykey] 5 10
    }

    test {Extended SET with IFNE and expiration} {
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IFNE "different" EX 10]
        assert_equal "world" [r get mykey]
        assert_range [r ttl mykey] 5 10
    }

    test {Extended SET with IFDEQ and expiration} {
        r set mykey "hello"
        set digest [r digest mykey]
        assert_equal "OK" [r set mykey "world" IFDEQ $digest EX 10]
        assert_equal "world" [r get mykey]
        assert_range [r ttl mykey] 5 10
    }

    test {Extended SET with IFDNE and expiration} {
        r set mykey "hello"
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal "OK" [r set mykey "world" IFDNE $wrong_digest EX 10]
        assert_equal "world" [r get mykey]
        assert_range [r ttl mykey] 5 10
    }

    test {Extended SET with IFEQ against wrong type} {
        r del mylist
        r lpush mylist "element"
        assert_error "*WRONGTYPE*" {r set mylist "value" IFEQ "element"}
    }

    test {Extended SET with IFNE against wrong type} {
        r del myhash
        r hset myhash field value
        assert_error "*WRONGTYPE*" {r set myhash "value" IFNE "value"}
    }

    test {Extended SET with IFDEQ against wrong type} {
        r del myset
        r sadd myset member
        assert_error "*WRONGTYPE*" {r set myset "value" IFDEQ 1234567890}
    }

    test {Extended SET with IFDNE against wrong type} {
        r del myzset
        r zadd myzset 1 member
        assert_error "*WRONGTYPE*" {r set myzset "value" IFDNE 1234567890}
    }

    test {Extended SET with integer-encoded value and IFEQ} {
        r set mykey 12345
        assert_encoding int mykey
        assert_equal "OK" [r set mykey "world" IFEQ "12345"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with integer-encoded value and IFNE} {
        r set mykey 12345
        assert_encoding int mykey
        assert_equal "OK" [r set mykey "world" IFNE "54321"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with binary data and IFEQ} {
        r set mykey "\x00\x01\x02\x03\xff\xfe"
        assert_equal "OK" [r set mykey "world" IFEQ "\x00\x01\x02\x03\xff\xfe"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with unicode characters and IFEQ} {
        r set mykey "Hello 世界"
        assert_equal "OK" [r set mykey "world" IFEQ "Hello 世界"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with empty string and IFEQ} {
        r set mykey ""
        assert_equal "OK" [r set mykey "world" IFEQ ""]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with empty string and IFNE} {
        r set mykey ""
        assert_equal {} [r set mykey "world" IFNE ""]
        assert_equal "" [r get mykey]
    }

    test {Extended SET case insensitive conditions} {
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" ifeq "hello"]
        assert_equal "world" [r get mykey]
        
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IfEq "hello"]
        assert_equal "world" [r get mykey]
        
        r set mykey "hello"
        assert_equal "OK" [r set mykey "world" IFEQ "hello"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with special characters and IFEQ} {
        r set mykey "  spaces  \t\n\r"
        assert_equal "OK" [r set mykey "world" IFEQ "  spaces  \t\n\r"]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET digest consistency with same content} {
        r set key1 "identical"
        r set key2 "identical"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert_equal $digest1 $digest2
        
        # Both should be settable with the same digest
        assert_equal "OK" [r set key1 "new1" IFDEQ $digest1]
        assert_equal "OK" [r set key2 "new2" IFDEQ $digest2]
        assert_equal "new1" [r get key1]
        assert_equal "new2" [r get key2]
    }

    test {Extended SET digest with different content} {
        r set key1 "value1"
        r set key2 "value2"
        set digest1 [r digest key1]
        set digest2 [r digest key2]
        assert {$digest1 != $digest2}
        
        # Should not be able to set with wrong digest
        assert_equal {} [r set key1 "new1" IFDEQ $digest2]
        assert_equal {} [r set key2 "new2" IFDEQ $digest1]
        assert_equal "value1" [r get key1]
        assert_equal "value2" [r get key2]
        
        # Should be able to set with correct digest
        assert_equal "OK" [r set key1 "new1" IFDEQ $digest1]
        assert_equal "OK" [r set key2 "new2" IFDEQ $digest2]
        assert_equal "new1" [r get key1]
        assert_equal "new2" [r get key2]
    }

    test {Extended SET with very long string and IFEQ} {
        set longstring [string repeat "Lorem ipsum dolor sit amet. " 1000]
        r set mykey $longstring
        assert_equal "OK" [r set mykey "world" IFEQ $longstring]
        assert_equal "world" [r get mykey]
    }

    test {Extended SET with negative digest} {
        r set mykey "test"
        set digest [r digest mykey]
        set wrong_digest [format %x [expr [scan [r digest mykey] %x] + 1]]
        assert_equal "OK" [r set mykey "world" IFDNE $wrong_digest]
        assert_equal "world" [r get mykey]
    }

    test {DIGEST always returns exactly 16 hex characters with leading zeros} {
        # Test with a value that produces a digest with leading zeros
        r set foo "v8lf0c11xh8ymlqztfd3eeq16kfn4sspw7fqmnuuq3k3t75em5wdizgcdw7uc26nnf961u2jkfzkjytls2kwlj7626sd"
        # Verify it matches the expected value with leading zeros
        assert_equal "00006c38adf31777" [r digest foo]
    }

    test {IFDEQ/IFDNE reject digest with incorrect format} {
        r set mykey "test"
        set digest [r digest mykey]

        # Test with too short digest (15 chars)
        set short_digest [string range $digest 1 end]
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDEQ $short_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDNE $short_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDEQ $short_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDNE $short_digest}

        # Test with too long digest (17 chars)
        set long_digest "0${digest}"
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDEQ $long_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDNE $long_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDEQ $long_digest}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDNE $long_digest}

        # Test with empty digest
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDEQ ""}
        assert_error "*must be exactly 16 hexadecimal characters*" {r set mykey "new" IFDNE ""}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDEQ ""}
        assert_error "*must be exactly 16 hexadecimal characters*" {r delex mykey IFDNE ""}
    }

    test {IFDEQ/IFDNE accepts uppercase hex digits (case-insensitive)} {
        # Test SET IFDEQ with uppercase
        r set mykey "hello"
        set digest [r digest mykey]
        set upper_digest [string toupper $digest]
        assert_equal "OK" [r set mykey "world" IFDEQ $upper_digest]
        assert_equal "world" [r get mykey]

        # Test SET IFDEQ with uppercase
        r set mykey "hello"
        set digest [r digest mykey]
        set upper_digest [string toupper $digest]
        assert_equal "" [r set mykey "world" IFDNE $upper_digest]
        assert_equal "hello" [r get mykey]

        # Test DELEX IFDEQ with uppercase
        r set mykey "hello"
        set upper_digest [string toupper [r digest mykey]]
        assert_equal 1 [r delex mykey IFDEQ $upper_digest]
        assert_equal 0 [r exists mykey]

        # Test DELEX IFDNE with uppercase
        r set mykey "hello"
        set upper_digest [string toupper [r digest mykey]]
        assert_equal 0 [r delex mykey IFDNE $upper_digest]
        assert_equal 1 [r exists mykey]
    }
}
