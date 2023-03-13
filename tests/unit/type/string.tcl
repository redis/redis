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
    }

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

    test {MSETNX with already existent key} {
        list [r msetnx x1{t} xxx y2{t} yyy x{t} 20] [r exists x1{t}] [r exists y2{t}]
    } {0 0 0}

    test {MSETNX with not existing keys} {
        list [r msetnx x1{t} xxx y2{t} yyy] [r get x1{t}] [r get y2{t}]
    } {1 xxx yyy}

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

    test "GETRANGE against string value" {
        r set mykey "Hello World"
        assert_equal "Hell" [r getrange mykey 0 3]
        assert_equal "Hello World" [r getrange mykey 0 -1]
        assert_equal "orld" [r getrange mykey -4 -1]
        assert_equal "" [r getrange mykey 5 3]
        assert_equal " World" [r getrange mykey 5 5000]
        assert_equal "Hello World" [r getrange mykey -5000 10000]
    }

    test "GETRANGE against integer-encoded value" {
        r set mykey 1234
        assert_equal "123" [r getrange mykey 0 2]
        assert_equal "1234" [r getrange mykey 0 -1]
        assert_equal "234" [r getrange mykey -3 -1]
        assert_equal "" [r getrange mykey 5 3]
        assert_equal "4" [r getrange mykey 3 5000]
        assert_equal "1234" [r getrange mykey -5000 10000]
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
}
