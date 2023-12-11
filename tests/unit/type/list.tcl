# check functionality compression of plain and zipped nodes
start_server [list overrides [list save ""] ] {
    r config set list-compress-depth 2
    r config set list-max-ziplist-size 1

    # 3 test to check compression with regular ziplist nodes
    # 1. using push + insert
    # 2. using push + insert + trim
    # 3. using push + insert + set

    test {reg node check compression with insert and pop} {
        r lpush list1 [string repeat a 500]
        r lpush list1 [string repeat b 500]
        r lpush list1 [string repeat c 500]
        r lpush list1 [string repeat d 500]
        r linsert list1 after [string repeat d 500] [string repeat e 500]
        r linsert list1 after [string repeat d 500] [string repeat f 500]
        r linsert list1 after [string repeat d 500] [string repeat g 500]
        r linsert list1 after [string repeat d 500] [string repeat j 500]
        assert_equal [r lpop list1] [string repeat d 500]
        assert_equal [r lpop list1] [string repeat j 500]
        assert_equal [r lpop list1] [string repeat g 500]
        assert_equal [r lpop list1] [string repeat f 500]
        assert_equal [r lpop list1] [string repeat e 500]
        assert_equal [r lpop list1] [string repeat c 500]
        assert_equal [r lpop list1] [string repeat b 500]
        assert_equal [r lpop list1] [string repeat a 500]
    };

    test {reg node check compression combined with trim} {
        r lpush list2 [string repeat a 500]
        r linsert list2 after  [string repeat a 500] [string repeat b 500]
        r rpush list2 [string repeat c 500]
        assert_equal [string repeat b 500] [r lindex list2 1]
        r LTRIM list2 1 -1
        r llen list2
    } {2}

    test {reg node check compression with lset} {
        r lpush list3 [string repeat a 500]
        r LSET list3 0 [string repeat b 500]
        assert_equal [string repeat b 500] [r lindex list3 0]
        r lpush list3 [string repeat c 500]
        r LSET list3 0 [string repeat d 500]
        assert_equal [string repeat d 500] [r lindex list3 0]
    }

    # repeating the 3 tests with plain nodes
    # (by adjusting quicklist-packed-threshold)

    test {plain node check compression} {
        r debug quicklist-packed-threshold 1b
        r lpush list4 [string repeat a 500]
        r lpush list4 [string repeat b 500]
        r lpush list4 [string repeat c 500]
        r lpush list4 [string repeat d 500]
        r linsert list4 after [string repeat d 500] [string repeat e 500]
        r linsert list4 after [string repeat d 500] [string repeat f 500]
        r linsert list4 after [string repeat d 500] [string repeat g 500]
        r linsert list4 after [string repeat d 500] [string repeat j 500]
        assert_equal [r lpop list4] [string repeat d 500]
        assert_equal [r lpop list4] [string repeat j 500]
        assert_equal [r lpop list4] [string repeat g 500]
        assert_equal [r lpop list4] [string repeat f 500]
        assert_equal [r lpop list4] [string repeat e 500]
        assert_equal [r lpop list4] [string repeat c 500]
        assert_equal [r lpop list4] [string repeat b 500]
        assert_equal [r lpop list4] [string repeat a 500]
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    test {plain node check compression with ltrim} {
        r debug quicklist-packed-threshold 1b
        r lpush list5 [string repeat a 500]
        r linsert list5 after  [string repeat a 500] [string repeat b 500]
        r rpush list5 [string repeat c 500]
        assert_equal [string repeat b 500] [r lindex list5 1]
        r LTRIM list5 1 -1
        assert_equal [r llen list5] 2
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    test {plain node check compression using lset} {
        r debug quicklist-packed-threshold 1b
        r lpush list6 [string repeat a 500]
        r LSET list6 0 [string repeat b 500]
        assert_equal [string repeat b 500] [r lindex list6 0]
        r lpush list6 [string repeat c 500]
        r LSET list6 0 [string repeat d 500]
        assert_equal [string repeat d 500] [r lindex list6 0]
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # revert config for external mode tests.
    r config set list-compress-depth 0
}

# check functionality of plain nodes using low packed-threshold
start_server [list overrides [list save ""] ] {
    # basic command check for plain nodes - "LPUSH & LPOP"
    test {Test LPUSH and LPOP on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r lpush lst 9
        r lpush lst xxxxxxxxxx
        r lpush lst xxxxxxxxxx
        set s0 [s used_memory]
        assert {$s0 > 10}
        assert {[r llen lst] == 3}
        set s0 [r rpop lst]
        set s1 [r rpop lst]
        assert {$s0 eq "9"}
        assert {[r llen lst] == 1}
        r lpop lst
        assert {[string length $s1] == 10}
        # check rdb
        r lpush lst xxxxxxxxxx
        r lpush lst bb
        r debug reload
        assert_equal [r rpop lst] "xxxxxxxxxx"
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # basic command check for plain nodes - "LINDEX & LINSERT"
    test {Test LINDEX and LINSERT on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r lpush lst xxxxxxxxxxx
        r lpush lst 9
        r lpush lst xxxxxxxxxxx
        r linsert lst before "9" "8"
        assert {[r lindex lst 1] eq "8"}
        r linsert lst BEFORE "9" "7"
        r linsert lst BEFORE "9" "xxxxxxxxxxx"
        assert {[r lindex lst 3] eq "xxxxxxxxxxx"}
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # basic command check for plain nodes - "LTRIM"
    test {Test LTRIM on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r lpush lst1 9
        r lpush lst1 xxxxxxxxxxx
        r lpush lst1 9
        r LTRIM lst1 1 -1
        assert_equal [r llen lst1] 2
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # basic command check for plain nodes - "LREM"
    test {Test LREM on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r lpush lst one
        r lpush lst xxxxxxxxxxx
        set s0 [s used_memory]
        assert {$s0 > 10}
        r lpush lst 9
        r LREM lst -2 "one"
        assert_equal [r llen lst] 2
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # basic command check for plain nodes - "LPOS"
    test {Test LPOS on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r RPUSH lst "aa"
        r RPUSH lst "bb"
        r RPUSH lst "cc"
        r LSET lst 0 "xxxxxxxxxxx"
        assert_equal [r LPOS lst "xxxxxxxxxxx"] 0
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # basic command check for plain nodes - "LMOVE"
    test {Test LMOVE on plain nodes} {
        r flushdb
        r debug quicklist-packed-threshold 1b
        r RPUSH lst2{t} "aa"
        r RPUSH lst2{t} "bb"
        r LSET lst2{t} 0 xxxxxxxxxxx
        r RPUSH lst2{t} "cc"
        r RPUSH lst2{t} "dd"
        r LMOVE lst2{t} lst{t} RIGHT LEFT
        r LMOVE lst2{t} lst{t} LEFT RIGHT
        assert_equal [r llen lst{t}] 2
        assert_equal [r llen lst2{t}] 2
        assert_equal [r lpop lst2{t}] "bb"
        assert_equal [r lpop lst2{t}] "cc"
        assert_equal [r lpop lst{t}] "dd"
        assert_equal [r lpop lst{t}] "xxxxxxxxxxx"
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # testing LSET with combinations of node types
    # plain->packed , packed->plain, plain->plain, packed->packed
    test {Test LSET with packed / plain combinations} {
        r debug quicklist-packed-threshold 5b
        r RPUSH lst "aa"
        r RPUSH lst "bb"
        r lset lst 0 [string repeat d 50001]
        set s1 [r lpop lst]
        assert_equal $s1 [string repeat d 50001]
        r RPUSH lst [string repeat f 50001]
        r lset lst 0 [string repeat e 50001]
        set s1 [r lpop lst]
        assert_equal $s1 [string repeat e 50001]
        r RPUSH lst [string repeat m 50001]
        r lset lst 0 "bb"
        set s1 [r lpop lst]
        assert_equal $s1 "bb"
        r RPUSH lst "bb"
        r lset lst 0 "cc"
        set s1 [r lpop lst]
        assert_equal $s1 "cc"
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}

    # checking LSET in case ziplist needs to be split
    test {Test LSET with packed is split in the middle} {
        r flushdb
        r debug quicklist-packed-threshold 5b
        r RPUSH lst "aa"
        r RPUSH lst "bb"
        r RPUSH lst "cc"
        r RPUSH lst "dd"
        r RPUSH lst "ee"
        r lset lst 2 [string repeat e 10]
        assert_equal [r lpop lst] "aa"
        assert_equal [r lpop lst] "bb"
        assert_equal [r lpop lst] [string repeat e 10]
        assert_equal [r lpop lst] "dd"
        assert_equal [r lpop lst] "ee"
        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}


    # repeating "plain check LSET with combinations"
    # but now with single item in each ziplist
    test {Test LSET with packed consist only one item} {
        r flushdb
        set original_config [config_get_set list-max-ziplist-size 1]
        r debug quicklist-packed-threshold 1b
        r RPUSH lst "aa"
        r RPUSH lst "bb"
        r lset lst 0 [string repeat d 50001]
        set s1 [r lpop lst]
        assert_equal $s1 [string repeat d 50001]
        r RPUSH lst [string repeat f 50001]
        r lset lst 0 [string repeat e 50001]
        set s1 [r lpop lst]
        assert_equal $s1 [string repeat e 50001]
        r RPUSH lst [string repeat m 50001]
        r lset lst 0 "bb"
        set s1 [r lpop lst]
        assert_equal $s1 "bb"
        r RPUSH lst "bb"
        r lset lst 0 "cc"
        set s1 [r lpop lst]
        assert_equal $s1 "cc"
        r debug quicklist-packed-threshold 0
        r config set list-max-ziplist-size $original_config
    } {OK} {needs:debug}

    test {Crash due to delete entry from a compress quicklist node} {
        r flushdb
        r debug quicklist-packed-threshold 100b
        set original_config [config_get_set list-compress-depth 1]

        set small_ele [string repeat x 32]
        set large_ele [string repeat x 100]

        # Push a large element
        r RPUSH lst $large_ele

        # Insert two elements and keep them in the same node
        r RPUSH lst $small_ele
        r RPUSH lst $small_ele

        # When setting the position of -1 to a large element, we first insert
        # a large element at the end and then delete its previous element.
        r LSET lst -1 $large_ele
        assert_equal "$large_ele $small_ele $large_ele" [r LRANGE lst 0 -1]

        r debug quicklist-packed-threshold 0
        r config set list-compress-depth $original_config
    } {OK} {needs:debug}

    test {Crash due to split quicklist node wrongly} {
        r flushdb
        r debug quicklist-packed-threshold 10b

        r LPUSH lst "aa"
        r LPUSH lst "bb"
        r LSET lst -2 [string repeat x 10]
        r RPOP lst
        assert_equal [string repeat x 10] [r LRANGE lst 0 -1]

        r debug quicklist-packed-threshold 0
    } {OK} {needs:debug}
}

run_solo {list-large-memory} {
start_server [list overrides [list save ""] ] {

# test if the server supports such large configs (avoid 32 bit builds)
catch {
    r config set proto-max-bulk-len 10000000000 ;#10gb
    r config set client-query-buffer-limit 10000000000 ;#10gb
}
if {[lindex [r config get proto-max-bulk-len] 1] == 10000000000} {

    set str_length 5000000000

    # repeating all the plain nodes basic checks with 5gb values
    test {Test LPUSH and LPOP on plain nodes over 4GB} {
        r flushdb
        r lpush lst 9
        r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
        write_big_bulk $str_length;
        r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
        write_big_bulk $str_length;
        set s0 [s used_memory]
        assert {$s0 > $str_length}
        assert {[r llen lst] == 3}
        assert_equal [r rpop lst] "9"
        assert_equal [read_big_bulk {r rpop lst}] $str_length
        assert {[r llen lst] == 1}
        assert_equal [read_big_bulk {r rpop lst}] $str_length
   } {} {large-memory}

   test {Test LINDEX and LINSERT on plain nodes over 4GB} {
       r flushdb
       r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
       write_big_bulk $str_length;
       r lpush lst 9
       r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
       write_big_bulk $str_length;
       r linsert lst before "9" "8"
       assert_equal [r lindex lst 1] "8"
       r LINSERT lst BEFORE "9" "7"
       r write "*5\r\n\$7\r\nLINSERT\r\n\$3\r\nlst\r\n\$6\r\nBEFORE\r\n\$3\r\n\"9\"\r\n"
       write_big_bulk 10;
       assert_equal [read_big_bulk {r rpop lst}] $str_length
   } {} {large-memory}

   test {Test LTRIM on plain nodes over 4GB} {
       r flushdb
       r lpush lst 9
       r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
       write_big_bulk $str_length;
       r lpush lst 9
       r LTRIM lst 1 -1
       assert_equal [r llen lst] 2
       assert_equal [r rpop lst] 9
       assert_equal [read_big_bulk {r rpop lst}] $str_length
   } {} {large-memory}

   test {Test LREM on plain nodes over 4GB} {
       r flushdb
       r lpush lst one
       r write "*3\r\n\$5\r\nLPUSH\r\n\$3\r\nlst\r\n"
       write_big_bulk $str_length;
       r lpush lst 9
       r LREM lst -2 "one"
       assert_equal [read_big_bulk {r rpop lst}] $str_length
       r llen lst
   } {1} {large-memory}

   test {Test LSET on plain nodes over 4GB} {
       r flushdb
       r RPUSH lst "aa"
       r RPUSH lst "bb"
       r RPUSH lst "cc"
       r write "*4\r\n\$4\r\nLSET\r\n\$3\r\nlst\r\n\$1\r\n0\r\n"
       write_big_bulk $str_length;
       assert_equal [r rpop lst] "cc"
       assert_equal [r rpop lst] "bb"
       assert_equal [read_big_bulk {r rpop lst}] $str_length
   } {} {large-memory}

   test {Test LMOVE on plain nodes over 4GB} {
       r flushdb
       r RPUSH lst2{t} "aa"
       r RPUSH lst2{t} "bb"
       r write "*4\r\n\$4\r\nLSET\r\n\$7\r\nlst2{t}\r\n\$1\r\n0\r\n"
       write_big_bulk $str_length;
       r RPUSH lst2{t} "cc"
       r RPUSH lst2{t} "dd"
       r LMOVE lst2{t} lst{t} RIGHT LEFT
       assert_equal [read_big_bulk {r LMOVE lst2{t} lst{t} LEFT RIGHT}] $str_length
       assert_equal [r llen lst{t}] 2
       assert_equal [r llen lst2{t}] 2
       assert_equal [r lpop lst2{t}] "bb"
       assert_equal [r lpop lst2{t}] "cc"
       assert_equal [r lpop lst{t}] "dd"
       assert_equal [read_big_bulk {r rpop lst{t}}] $str_length
   } {} {large-memory}

    # restore defaults
    r config set proto-max-bulk-len 536870912
    r config set client-query-buffer-limit 1073741824

} ;# skip 32bit builds
}
} ;# run_solo

start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-size" -1
    }
} {
    source "tests/unit/type/list-common.tcl"

    # A helper function to execute either B*POP or BLMPOP* with one input key.
    proc bpop_command {rd pop key timeout} {
        if {$pop == "BLMPOP_LEFT"} {
            $rd blmpop $timeout 1 $key left count 1
        } elseif {$pop == "BLMPOP_RIGHT"} {
            $rd blmpop $timeout 1 $key right count 1
        } else {
            $rd $pop $key $timeout
        }
    }

    # A helper function to execute either B*POP or BLMPOP* with two input keys.
    proc bpop_command_two_key {rd pop key key2 timeout} {
        if {$pop == "BLMPOP_LEFT"} {
            $rd blmpop $timeout 2 $key $key2 left count 1
        } elseif {$pop == "BLMPOP_RIGHT"} {
            $rd blmpop $timeout 2 $key $key2 right count 1
        } else {
            $rd $pop $key $key2 $timeout
        }
    }

    proc create_listpack {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding listpack $key
    }

    proc create_quicklist {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding quicklist $key
    }

foreach {type large} [array get largevalue] {
    test "LPOS basic usage - $type" {
        r DEL mylist
        r RPUSH mylist a b c $large 2 3 c c
        assert {[r LPOS mylist a] == 0}
        assert {[r LPOS mylist c] == 2}
    }

    test {LPOS RANK (positive, negative and zero rank) option} {
        assert {[r LPOS mylist c RANK 1] == 2}
        assert {[r LPOS mylist c RANK 2] == 6}
        assert {[r LPOS mylist c RANK 4] eq ""}
        assert {[r LPOS mylist c RANK -1] == 7}
        assert {[r LPOS mylist c RANK -2] == 6}
        assert_error "*RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to start*" {r LPOS mylist c RANK 0}
        assert_error "*value is out of range*" {r LPOS mylist c RANK -9223372036854775808}
    }

    test {LPOS COUNT option} {
        assert {[r LPOS mylist c COUNT 0] == {2 6 7}}
        assert {[r LPOS mylist c COUNT 1] == {2}}
        assert {[r LPOS mylist c COUNT 2] == {2 6}}
        assert {[r LPOS mylist c COUNT 100] == {2 6 7}}
    }

    test {LPOS COUNT + RANK option} {
        assert {[r LPOS mylist c COUNT 0 RANK 2] == {6 7}}
        assert {[r LPOS mylist c COUNT 2 RANK -1] == {7 6}}
    }

    test {LPOS non existing key} {
        assert {[r LPOS mylistxxx c COUNT 0 RANK 2] eq {}}
    }

    test {LPOS no match} {
        assert {[r LPOS mylist x COUNT 2 RANK -1] eq {}}
        assert {[r LPOS mylist x RANK -1] eq {}}
    }

    test {LPOS MAXLEN} {
        assert {[r LPOS mylist a COUNT 0 MAXLEN 1] == {0}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 1] == {}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 3] == {2}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 3 RANK -1] == {7 6}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 7 RANK 2] == {6}}
    }

    test {LPOS when RANK is greater than matches} {
        r DEL mylist
        r LPUSH mylist a
        assert {[r LPOS mylist b COUNT 10 RANK 5] eq {}}
    }

    test "LPUSH, RPUSH, LLENGTH, LINDEX, LPOP - $type" {
        # first lpush then rpush
        r del mylist1
        assert_equal 1 [r lpush mylist1 $large]
        assert_encoding $type mylist1
        assert_equal 2 [r rpush mylist1 b]
        assert_equal 3 [r rpush mylist1 c]
        assert_equal 3 [r llen mylist1]
        assert_equal $large [r lindex mylist1 0]
        assert_equal b [r lindex mylist1 1]
        assert_equal c [r lindex mylist1 2]
        assert_equal {} [r lindex mylist1 3]
        assert_equal c [r rpop mylist1]
        assert_equal $large [r lpop mylist1]

        # first rpush then lpush
        r del mylist2
        assert_equal 1 [r rpush mylist2 $large]
        assert_equal 2 [r lpush mylist2 b]
        assert_equal 3 [r lpush mylist2 c]
        assert_encoding $type mylist2
        assert_equal 3 [r llen mylist2]
        assert_equal c [r lindex mylist2 0]
        assert_equal b [r lindex mylist2 1]
        assert_equal $large [r lindex mylist2 2]
        assert_equal {} [r lindex mylist2 3]
        assert_equal $large [r rpop mylist2]
        assert_equal c [r lpop mylist2]
    }

    test "LPOP/RPOP with wrong number of arguments" {
        assert_error {*wrong number of arguments for 'lpop' command} {r lpop key 1 1}
        assert_error {*wrong number of arguments for 'rpop' command} {r rpop key 2 2}
    }

    test "RPOP/LPOP with the optional count argument - $type" {
        assert_equal 7 [r lpush listcount aa $large cc dd ee ff gg]
        assert_equal {gg} [r lpop listcount 1]
        assert_equal {ff ee} [r lpop listcount 2]
        assert_equal "aa $large" [r rpop listcount 2]
        assert_equal {cc} [r rpop listcount 1]
        assert_equal {dd} [r rpop listcount 123]
        assert_error "*ERR*range*" {r lpop forbarqaz -123}
    }
}

    proc verify_resp_response {resp response resp2_response resp3_response} {
        if {$resp == 2} {
            assert_equal $response $resp2_response
        } elseif {$resp == 3} {
            assert_equal $response $resp3_response
        }
    }

    foreach resp {3 2} {
        if {[lsearch $::denytags "resp3"] >= 0} {
            if {$resp == 3} {continue}
        } elseif {$::force_resp3} {
            if {$resp == 2} {continue}
        }
        r hello $resp

        # Make sure we can distinguish between an empty array and a null response
        r readraw 1

        test "LPOP/RPOP with the count 0 returns an empty array in RESP$resp" {
            r lpush listcount zero
            assert_equal {*0} [r lpop listcount 0]
            assert_equal {*0} [r rpop listcount 0]
        }

        test "LPOP/RPOP against non existing key in RESP$resp" {
            r del non_existing_key

            verify_resp_response $resp [r lpop non_existing_key] {$-1} {_}
            verify_resp_response $resp [r rpop non_existing_key] {$-1} {_}
        }

        test "LPOP/RPOP with <count> against non existing key in RESP$resp" {
            r del non_existing_key

            verify_resp_response $resp [r lpop non_existing_key 0] {*-1} {_}
            verify_resp_response $resp [r lpop non_existing_key 1] {*-1} {_}

            verify_resp_response $resp [r rpop non_existing_key 0] {*-1} {_}
            verify_resp_response $resp [r rpop non_existing_key 1] {*-1} {_}
        }

        r readraw 0
        r hello 2
    }

    test {Variadic RPUSH/LPUSH} {
        r del mylist
        assert_equal 4 [r lpush mylist a b c d]
        assert_equal 8 [r rpush mylist 0 1 2 3]
        assert_equal {d c b a 0 1 2 3} [r lrange mylist 0 -1]
    }

    test {DEL a list} {
        assert_equal 1 [r del mylist2]
        assert_equal 0 [r exists mylist2]
        assert_equal 0 [r llen mylist2]
    }

    foreach {type large} [array get largevalue] {
    foreach {pop} {BLPOP BLMPOP_LEFT} {
        test "$pop: single existing list - $type" {
            set rd [redis_deferring_client]
            create_$type blist "a b $large c d"

            bpop_command $rd $pop blist 1
            assert_equal {blist a} [$rd read]
            if {$pop == "BLPOP"} {
                bpop_command $rd BRPOP blist 1
            } else {
                bpop_command $rd BLMPOP_RIGHT blist 1
            }
            assert_equal {blist d} [$rd read]

            bpop_command $rd $pop blist 1
            assert_equal {blist b} [$rd read]
            if {$pop == "BLPOP"} {
                bpop_command $rd BRPOP blist 1
            } else {
                bpop_command $rd BLMPOP_RIGHT blist 1
            }
            assert_equal {blist c} [$rd read]

            assert_equal 1 [r llen blist]
            $rd close
        }

        test "$pop: multiple existing lists - $type" {
            set rd [redis_deferring_client]
            create_$type blist1{t} "a $large c"
            create_$type blist2{t} "d $large f"

            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            assert_equal {blist1{t} a} [$rd read]
            if {$pop == "BLPOP"} {
                bpop_command_two_key $rd BRPOP blist1{t} blist2{t} 1
            } else {
                bpop_command_two_key $rd BLMPOP_RIGHT blist1{t} blist2{t} 1
            }
            assert_equal {blist1{t} c} [$rd read]
            assert_equal 1 [r llen blist1{t}]
            assert_equal 3 [r llen blist2{t}]

            bpop_command_two_key $rd $pop blist2{t} blist1{t} 1
            assert_equal {blist2{t} d} [$rd read]
            if {$pop == "BLPOP"} {
                bpop_command_two_key $rd BRPOP blist2{t} blist1{t} 1
            } else {
                bpop_command_two_key $rd BLMPOP_RIGHT blist2{t} blist1{t} 1
            }
            assert_equal {blist2{t} f} [$rd read]
            assert_equal 1 [r llen blist1{t}]
            assert_equal 1 [r llen blist2{t}]
            $rd close
        }

        test "$pop: second list has an entry - $type" {
            set rd [redis_deferring_client]
            r del blist1{t}
            create_$type blist2{t} "d $large f"

            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            assert_equal {blist2{t} d} [$rd read]
            if {$pop == "BLPOP"} {
                bpop_command_two_key $rd BRPOP blist1{t} blist2{t} 1
            } else {
                bpop_command_two_key $rd BLMPOP_RIGHT blist1{t} blist2{t} 1
            }
            assert_equal {blist2{t} f} [$rd read]
            assert_equal 0 [r llen blist1{t}]
            assert_equal 1 [r llen blist2{t}]
            $rd close
        }
    }

        test "BRPOPLPUSH - $type" {
            r del target{t}
            r rpush target{t} bar

            set rd [redis_deferring_client]
            create_$type blist{t} "a b $large c d"

            $rd brpoplpush blist{t} target{t} 1
            assert_equal d [$rd read]

            assert_equal d [r lpop target{t}]
            assert_equal "a b $large c" [r lrange blist{t} 0 -1]
            $rd close
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "BLMOVE $wherefrom $whereto - $type" {
                    r del target{t}
                    r rpush target{t} bar

                    set rd [redis_deferring_client]
                    create_$type blist{t} "a b $large c d"

                    $rd blmove blist{t} target{t} $wherefrom $whereto 1
                    set poppedelement [$rd read]

                    if {$wherefrom eq "right"} {
                        assert_equal d $poppedelement
                        assert_equal "a b $large c" [r lrange blist{t} 0 -1]
                    } else {
                        assert_equal a $poppedelement
                        assert_equal "b $large c d" [r lrange blist{t} 0 -1]
                    }

                    if {$whereto eq "right"} {
                        assert_equal $poppedelement [r rpop target{t}]
                    } else {
                        assert_equal $poppedelement [r lpop target{t}]
                    }
                    $rd close
                }
            }
        }
    }

foreach {pop} {BLPOP BLMPOP_LEFT} {
    test "$pop, LPUSH + DEL should not awake blocked client" {
        set rd [redis_deferring_client]
        r del list

        bpop_command $rd $pop list 0
        wait_for_blocked_client

        r multi
        r lpush list a
        r del list
        r exec
        r del list
        r lpush list b
        assert_equal {list b} [$rd read]
        $rd close
    }

    test "$pop, LPUSH + DEL + SET should not awake blocked client" {
        set rd [redis_deferring_client]
        r del list

        bpop_command $rd $pop list 0
        wait_for_blocked_client

        r multi
        r lpush list a
        r del list
        r set list foo
        r exec
        r del list
        r lpush list b
        assert_equal {list b} [$rd read]
        $rd close
    }
}

    test "BLPOP with same key multiple times should work (issue #801)" {
        set rd [redis_deferring_client]
        r del list1{t} list2{t}

        # Data arriving after the BLPOP.
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        wait_for_blocked_client
        r lpush list1{t} a
        assert_equal [$rd read] {list1{t} a}
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        wait_for_blocked_client
        r lpush list2{t} b
        assert_equal [$rd read] {list2{t} b}

        # Data already there.
        r lpush list1{t} a
        r lpush list2{t} b
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        assert_equal [$rd read] {list1{t} a}
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        assert_equal [$rd read] {list2{t} b}
        $rd close
    }

foreach {pop} {BLPOP BLMPOP_LEFT} {
    test "MULTI/EXEC is isolated from the point of view of $pop" {
        set rd [redis_deferring_client]
        r del list

        bpop_command $rd $pop list 0
        wait_for_blocked_client

        r multi
        r lpush list a
        r lpush list b
        r lpush list c
        r exec
        assert_equal {list c} [$rd read]
        $rd close
    }

    test "$pop with variadic LPUSH" {
        set rd [redis_deferring_client]
        r del blist
        bpop_command $rd $pop blist 0
        wait_for_blocked_client
        assert_equal 2 [r lpush blist foo bar]
        assert_equal {blist bar} [$rd read]
        assert_equal foo [lindex [r lrange blist 0 -1] 0]
        $rd close
    }
}

    test "BRPOPLPUSH with zero timeout should block indefinitely" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r rpush target{t} bar
        $rd brpoplpush blist{t} target{t} 0
        wait_for_blocked_clients_count 1
        r rpush blist{t} foo
        assert_equal foo [$rd read]
        assert_equal {foo bar} [r lrange target{t} 0 -1]
        $rd close
    }

    foreach wherefrom {left right} {
        foreach whereto {left right} {
            test "BLMOVE $wherefrom $whereto with zero timeout should block indefinitely" {
                set rd [redis_deferring_client]
                r del blist{t} target{t}
                r rpush target{t} bar
                $rd blmove blist{t} target{t} $wherefrom $whereto 0
                wait_for_blocked_clients_count 1
                r rpush blist{t} foo
                assert_equal foo [$rd read]
                if {$whereto eq "right"} {
                    assert_equal {bar foo} [r lrange target{t} 0 -1]
                } else {
                    assert_equal {foo bar} [r lrange target{t} 0 -1]
                }
                $rd close
            }
        }
    }

    foreach wherefrom {left right} {
        foreach whereto {left right} {
            test "BLMOVE ($wherefrom, $whereto) with a client BLPOPing the target list" {
                set rd [redis_deferring_client]
                set rd2 [redis_deferring_client]
                r del blist{t} target{t}
                $rd2 blpop target{t} 0
                wait_for_blocked_clients_count 1
                $rd blmove blist{t} target{t} $wherefrom $whereto 0
                wait_for_blocked_clients_count 2
                r rpush blist{t} foo
                assert_equal foo [$rd read]
                assert_equal {target{t} foo} [$rd2 read]
                assert_equal 0 [r exists target{t}]
                $rd close
                $rd2 close
            }
        }
    }

    test "BRPOPLPUSH with wrong source type" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set blist{t} nolist
        $rd brpoplpush blist{t} target{t} 1
        assert_error "WRONGTYPE*" {$rd read}
        $rd close
    }

    test "BRPOPLPUSH with wrong destination type" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        r lpush blist{t} foo
        $rd brpoplpush blist{t} target{t} 1
        assert_error "WRONGTYPE*" {$rd read}
        $rd close

        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        $rd brpoplpush blist{t} target{t} 0
        wait_for_blocked_clients_count 1
        r rpush blist{t} foo
        assert_error "WRONGTYPE*" {$rd read}
        assert_equal {foo} [r lrange blist{t} 0 -1]
        $rd close
    }

    test "BRPOPLPUSH maintains order of elements after failure" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        $rd brpoplpush blist{t} target{t} 0
        wait_for_blocked_client
        r rpush blist{t} a b c
        assert_error "WRONGTYPE*" {$rd read}
        $rd close
        r lrange blist{t} 0 -1
    } {a b c}

    test "BRPOPLPUSH with multiple blocked clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        r del blist{t} target1{t} target2{t}
        r set target1{t} nolist
        $rd1 brpoplpush blist{t} target1{t} 0
        wait_for_blocked_clients_count 1
        $rd2 brpoplpush blist{t} target2{t} 0
        wait_for_blocked_clients_count 2
        r lpush blist{t} foo

        assert_error "WRONGTYPE*" {$rd1 read}
        assert_equal {foo} [$rd2 read]
        assert_equal {foo} [r lrange target2{t} 0 -1]
        $rd1 close
        $rd2 close
    }

    test "BLMPOP with multiple blocked clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set rd3 [redis_deferring_client]
        set rd4 [redis_deferring_client]
        r del blist{t} blist2{t}

        $rd1 blmpop 0 2 blist{t} blist2{t} left count 1
        wait_for_blocked_clients_count 1
        $rd2 blmpop 0 2 blist{t} blist2{t} right count 10
        wait_for_blocked_clients_count 2
        $rd3 blmpop 0 2 blist{t} blist2{t} left count 10
        wait_for_blocked_clients_count 3
        $rd4 blmpop 0 2 blist{t} blist2{t} right count 1
        wait_for_blocked_clients_count 4

        r multi
        r lpush blist{t} a b c d e
        r lpush blist2{t} 1 2 3 4 5
        r exec

        assert_equal {blist{t} e} [$rd1 read]
        assert_equal {blist{t} {a b c d}} [$rd2 read]
        assert_equal {blist2{t} {5 4 3 2 1}} [$rd3 read]

        r lpush blist2{t} 1 2 3
        assert_equal {blist2{t} 1} [$rd4 read]
        $rd1 close
        $rd2 close
        $rd3 close
        $rd4 close
    }

    test "Linked LMOVEs" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1{t} list2{t} list3{t}

      $rd1 blmove list1{t} list2{t} right left 0
      wait_for_blocked_clients_count 1
      $rd2 blmove list2{t} list3{t} left right 0
      wait_for_blocked_clients_count 2

      r rpush list1{t} foo

      assert_equal {} [r lrange list1{t} 0 -1]
      assert_equal {} [r lrange list2{t} 0 -1]
      assert_equal {foo} [r lrange list3{t} 0 -1]
      $rd1 close
      $rd2 close
    }

    test "Circular BRPOPLPUSH" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1{t} list2{t}

      $rd1 brpoplpush list1{t} list2{t} 0
      wait_for_blocked_clients_count 1
      $rd2 brpoplpush list2{t} list1{t} 0
      wait_for_blocked_clients_count 2

      r rpush list1{t} foo

      assert_equal {foo} [r lrange list1{t} 0 -1]
      assert_equal {} [r lrange list2{t} 0 -1]
      $rd1 close
      $rd2 close
    }

    test "Self-referential BRPOPLPUSH" {
      set rd [redis_deferring_client]

      r del blist{t}

      $rd brpoplpush blist{t} blist{t} 0
      wait_for_blocked_client

      r rpush blist{t} foo

      assert_equal {foo} [r lrange blist{t} 0 -1]
      $rd close
    }

    test "BRPOPLPUSH inside a transaction" {
        r del xlist{t} target{t}
        r lpush xlist{t} foo
        r lpush xlist{t} bar

        r multi
        r brpoplpush xlist{t} target{t} 0
        r brpoplpush xlist{t} target{t} 0
        r brpoplpush xlist{t} target{t} 0
        r lrange xlist{t} 0 -1
        r lrange target{t} 0 -1
        r exec
    } {foo bar {} {} {bar foo}}

    test "PUSH resulting from BRPOPLPUSH affect WATCH" {
        set blocked_client [redis_deferring_client]
        set watching_client [redis_deferring_client]
        r del srclist{t} dstlist{t} somekey{t}
        r set somekey{t} somevalue
        $blocked_client brpoplpush srclist{t} dstlist{t} 0
        wait_for_blocked_client
        $watching_client watch dstlist{t}
        $watching_client read
        $watching_client multi
        $watching_client read
        $watching_client get somekey{t}
        $watching_client read
        r lpush srclist{t} element
        $watching_client exec
        set res [$watching_client read]
        $blocked_client close
        $watching_client close
        set _ $res
    } {}

    test "BRPOPLPUSH does not affect WATCH while still blocked" {
        set blocked_client [redis_deferring_client]
        set watching_client [redis_deferring_client]
        r del srclist{t} dstlist{t} somekey{t}
        r set somekey{t} somevalue
        $blocked_client brpoplpush srclist{t} dstlist{t} 0
        wait_for_blocked_client
        $watching_client watch dstlist{t}
        $watching_client read
        $watching_client multi
        $watching_client read
        $watching_client get somekey{t}
        $watching_client read
        $watching_client exec
        # Blocked BLPOPLPUSH may create problems, unblock it.
        r lpush srclist{t} element
        set res [$watching_client read]
        $blocked_client close
        $watching_client close
        set _ $res
    } {somevalue}

    test {BRPOPLPUSH timeout} {
      set rd [redis_deferring_client]

      $rd brpoplpush foo_list{t} bar_list{t} 1
      wait_for_blocked_clients_count 1
      wait_for_blocked_clients_count 0 500 10
      set res [$rd read]
      $rd close
      set _ $res
    } {}

    test {SWAPDB awakes blocked client} {
        r flushall
        r select 1
        r rpush k hello
        r select 9
        set rd [redis_deferring_client]
        $rd brpop k 5
        wait_for_blocked_clients_count 1
        r swapdb 1 9
        $rd read
    } {k hello} {singledb:skip}

    test {SWAPDB wants to wake blocked client, but the key already expired} {
        set repl [attach_to_replication_stream]
        r flushall
        r debug set-active-expire 0
        r select 1
        r rpush k hello
        r pexpire k 100
        set rd [redis_deferring_client]
        $rd deferred 0
        $rd select 9
        set id [$rd client id]
        $rd deferred 1
        $rd brpop k 1
        wait_for_blocked_clients_count 1
        after 101
        r swapdb 1 9
        # The SWAPDB command tries to awake the blocked client, but it remains
        # blocked because the key is expired. Check that the deferred client is
        # still blocked. Then unblock it.
        assert_match "*flags=b*" [r client list id $id]
        r client unblock $id
        assert_equal {} [$rd read]
        $rd deferred 0
        # We want to force key deletion to be propagated to the replica 
        # in order to verify it was expired on the replication stream.
        $rd set somekey1 someval1
        $rd exists k
        r set somekey2 someval2
        
        assert_replication_stream $repl {
            {select *}
            {flushall}
            {select 1}
            {rpush k hello}
            {pexpireat k *}
            {swapdb 1 9}
            {select 9}
            {set somekey1 someval1}
            {del k}
            {select 1}
            {set somekey2 someval2}
        }
        close_replication_stream $repl
        r debug set-active-expire 1
        # Restore server and client state
        r select 9
    } {OK} {singledb:skip needs:debug}

    test {MULTI + LPUSH + EXPIRE + DEBUG SLEEP on blocked client, key already expired} {
        set repl [attach_to_replication_stream]
        r flushall
        r debug set-active-expire 0

        set rd [redis_deferring_client]
        $rd client id
        set id [$rd read]
        $rd brpop k 0
        wait_for_blocked_clients_count 1

        r multi
        r rpush k hello
        r pexpire k 100
        r debug sleep 0.2
        r exec

        # The EXEC command tries to awake the blocked client, but it remains
        # blocked because the key is expired. Check that the deferred client is
        # still blocked. Then unblock it.
        assert_match "*flags=b*" [r client list id $id]
        r client unblock $id
        assert_equal {} [$rd read]
        # We want to force key deletion to be propagated to the replica 
        # in order to verify it was expired on the replication stream.
        $rd exists k
        assert_equal {0} [$rd read]
        assert_replication_stream $repl {
            {select *}
            {flushall}
            {multi}
            {rpush k hello}
            {pexpireat k *}
            {exec}
            {del k}
        }
        close_replication_stream $repl
        # Restore server and client state
        r debug set-active-expire 1
        r select 9
    } {OK} {singledb:skip needs:debug}

foreach {pop} {BLPOP BLMPOP_LEFT} {
    test "$pop when new key is moved into place" {
        set rd [redis_deferring_client]
        r del foo{t}

        bpop_command $rd $pop foo{t} 0
        wait_for_blocked_client
        r lpush bob{t} abc def hij
        r rename bob{t} foo{t}
        set res [$rd read]
        $rd close
        set _ $res
    } {foo{t} hij}

    test "$pop when result key is created by SORT..STORE" {
        set rd [redis_deferring_client]

        # zero out list from previous test without explicit delete
        r lpop foo{t}
        r lpop foo{t}
        r lpop foo{t}

        bpop_command $rd $pop foo{t} 5
        wait_for_blocked_client
        r lpush notfoo{t} hello hola aguacate konichiwa zanzibar
        r sort notfoo{t} ALPHA store foo{t}
        set res [$rd read]
        $rd close
        set _ $res
    } {foo{t} aguacate}
}

    test "BLPOP: timeout value out of range" {
        # Timeout is parsed as float and multiplied by 1000, added mstime()
        # and stored in long-long which might lead to out-of-range value.
        # (Even though given timeout is smaller than LLONG_MAX, the result
        # will be bigger)            
        assert_error "ERR *is out of range*" {r BLPOP blist1 0x7FFFFFFFFFFFFF}
    }  
        
    foreach {pop} {BLPOP BRPOP BLMPOP_LEFT BLMPOP_RIGHT} {
        test "$pop: with single empty list argument" {
            set rd [redis_deferring_client]
            r del blist1
            bpop_command $rd $pop blist1 1
            wait_for_blocked_client
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
            $rd close
        }

        test "$pop: with negative timeout" {
            set rd [redis_deferring_client]
            bpop_command $rd $pop blist1 -1
            assert_error "ERR *is negative*" {$rd read}
            $rd close
        }

        test "$pop: with non-integer timeout" {
            set rd [redis_deferring_client]
            r del blist1
            bpop_command $rd $pop blist1 0.1
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
            $rd close
        }

        test "$pop: with zero timeout should block indefinitely" {
            # To test this, use a timeout of 0 and wait a second.
            # The blocking pop should still be waiting for a push.
            set rd [redis_deferring_client]
            bpop_command $rd $pop blist1 0
            wait_for_blocked_client
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            $rd close
        }

        test "$pop: with 0.001 timeout should not block indefinitely" {
            # Use a timeout of 0.001 and wait for the number of blocked clients to equal 0.
            # Validate the empty read from the deferring client.
            set rd [redis_deferring_client]
            bpop_command $rd $pop blist1 0.001
            wait_for_blocked_clients_count 0
            assert_equal {} [$rd read]
            $rd close
        }

        test "$pop: second argument is not a list" {
            set rd [redis_deferring_client]
            r del blist1{t} blist2{t}
            r set blist2{t} nolist{t}
            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            assert_error "WRONGTYPE*" {$rd read}
            $rd close
        }

        test "$pop: timeout" {
            set rd [redis_deferring_client]
            r del blist1{t} blist2{t}
            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            wait_for_blocked_client
            assert_equal {} [$rd read]
            $rd close
        }

        test "$pop: arguments are empty" {
            set rd [redis_deferring_client]
            r del blist1{t} blist2{t}

            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            wait_for_blocked_client
            r rpush blist1{t} foo
            assert_equal {blist1{t} foo} [$rd read]
            assert_equal 0 [r exists blist1{t}]
            assert_equal 0 [r exists blist2{t}]

            bpop_command_two_key $rd $pop blist1{t} blist2{t} 1
            wait_for_blocked_client
            r rpush blist2{t} foo
            assert_equal {blist2{t} foo} [$rd read]
            assert_equal 0 [r exists blist1{t}]
            assert_equal 0 [r exists blist2{t}]
            $rd close
        }
    }

foreach {pop} {BLPOP BLMPOP_LEFT} {
    test "$pop inside a transaction" {
        r del xlist
        r lpush xlist foo
        r lpush xlist bar
        r multi

        bpop_command r $pop xlist 0
        bpop_command r $pop xlist 0
        bpop_command r $pop xlist 0
        r exec
    } {{xlist bar} {xlist foo} {}}
}

    test {BLMPOP propagate as pop with count command to replica} {
        set rd [redis_deferring_client]
        set repl [attach_to_replication_stream]

        # BLMPOP without being blocked.
        r lpush mylist{t} a b c
        r rpush mylist2{t} 1 2 3
        r blmpop 0 1 mylist{t} left count 1
        r blmpop 0 2 mylist{t} mylist2{t} right count 10
        r blmpop 0 2 mylist{t} mylist2{t} right count 10

        # BLMPOP that gets blocked.
        $rd blmpop 0 1 mylist{t} left count 1
        wait_for_blocked_client
        r lpush mylist{t} a
        $rd blmpop 0 2 mylist{t} mylist2{t} left count 5
        wait_for_blocked_client
        r lpush mylist{t} a b c
        $rd blmpop 0 2 mylist{t} mylist2{t} right count 10
        wait_for_blocked_client
        r rpush mylist2{t} a b c

        # Released on timeout.
        assert_equal {} [r blmpop 0.01 1 mylist{t} left count 10]
        r set foo{t} bar ;# something else to propagate after, so we can make sure the above pop didn't.

        $rd close

        assert_replication_stream $repl {
            {select *}
            {lpush mylist{t} a b c}
            {rpush mylist2{t} 1 2 3}
            {lpop mylist{t} 1}
            {rpop mylist{t} 2}
            {rpop mylist2{t} 3}
            {lpush mylist{t} a}
            {lpop mylist{t} 1}
            {lpush mylist{t} a b c}
            {lpop mylist{t} 3}
            {rpush mylist2{t} a b c}
            {rpop mylist2{t} 3}
            {set foo{t} bar}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {LPUSHX, RPUSHX - generic} {
        r del xlist
        assert_equal 0 [r lpushx xlist a]
        assert_equal 0 [r llen xlist]
        assert_equal 0 [r rpushx xlist a]
        assert_equal 0 [r llen xlist]
    }

    foreach {type large} [array get largevalue] {
        test "LPUSHX, RPUSHX - $type" {
            create_$type xlist "$large c"
            assert_equal 3 [r rpushx xlist d]
            assert_equal 4 [r lpushx xlist a]
            assert_equal 6 [r rpushx xlist 42 x]
            assert_equal 9 [r lpushx xlist y3 y2 y1]
            assert_equal "y1 y2 y3 a $large c d 42 x" [r lrange xlist 0 -1]
        }

        test "LINSERT - $type" {
            create_$type xlist "a $large c d"
            assert_equal 5 [r linsert xlist before c zz] "before c"
            assert_equal "a $large zz c d" [r lrange xlist 0 10] "lrangeA"
            assert_equal 6 [r linsert xlist after c yy] "after c"
            assert_equal "a $large zz c yy d" [r lrange xlist 0 10] "lrangeB"
            assert_equal 7 [r linsert xlist after d dd] "after d"
            assert_equal -1 [r linsert xlist after bad ddd] "after bad"
            assert_equal "a $large zz c yy d dd" [r lrange xlist 0 10] "lrangeC"
            assert_equal 8 [r linsert xlist before a aa] "before a"
            assert_equal -1 [r linsert xlist before bad aaa] "before bad"
            assert_equal "aa a $large zz c yy d dd" [r lrange xlist 0 10] "lrangeD"

            # check inserting integer encoded value
            assert_equal 9 [r linsert xlist before aa 42] "before aa"
            assert_equal 42 [r lrange xlist 0 0] "lrangeE"
        }
    }

    test {LINSERT raise error on bad syntax} {
        catch {[r linsert xlist aft3r aa 42]} e
        set e
    } {*ERR*syntax*error*}

    test {LINSERT against non-list value error} {
        r set k1 v1
        assert_error {WRONGTYPE Operation against a key holding the wrong kind of value*} {r linsert k1 after 0 0}
    }

    test {LINSERT against non existing key} {
        assert_equal 0 [r linsert not-a-key before 0 0]
    }

foreach type {listpack quicklist} {
    foreach {num} {250 500} {
        if {$type == "quicklist"} {
            set origin_config [config_get_set list-max-listpack-size 5]
        } else {
            set origin_config [config_get_set list-max-listpack-size -1]
        }

        proc check_numbered_list_consistency {key} {
            set len [r llen $key]
            for {set i 0} {$i < $len} {incr i} {
                assert_equal $i [r lindex $key $i]
                assert_equal [expr $len-1-$i] [r lindex $key [expr (-$i)-1]]
            }
        }

        proc check_random_access_consistency {key} {
            set len [r llen $key]
            for {set i 0} {$i < $len} {incr i} {
                set rint [expr int(rand()*$len)]
                assert_equal $rint [r lindex $key $rint]
                assert_equal [expr $len-1-$rint] [r lindex $key [expr (-$rint)-1]]
            }
        }

        test "LINDEX consistency test - $type" {
            r del mylist
            for {set i 0} {$i < $num} {incr i} {
                r rpush mylist $i
            }
            assert_encoding $type mylist
            check_numbered_list_consistency mylist
        }

        test "LINDEX random access - $type" {
            assert_encoding $type mylist
            check_random_access_consistency mylist
        }

        test "Check if list is still ok after a DEBUG RELOAD - $type" {
            r debug reload
            assert_encoding $type mylist
            check_numbered_list_consistency mylist
            check_random_access_consistency mylist
        } {} {needs:debug}

        config_set list-max-listpack-size $origin_config
    }
}

    test {LLEN against non-list value error} {
        r del mylist
        r set mylist foobar
        assert_error WRONGTYPE* {r llen mylist}
    }

    test {LLEN against non existing key} {
        assert_equal 0 [r llen not-a-key]
    }

    test {LINDEX against non-list value error} {
        assert_error WRONGTYPE* {r lindex mylist 0}
    }

    test {LINDEX against non existing key} {
        assert_equal "" [r lindex not-a-key 10]
    }

    test {LPUSH against non-list value error} {
        assert_error WRONGTYPE* {r lpush mylist 0}
    }

    test {RPUSH against non-list value error} {
        assert_error WRONGTYPE* {r rpush mylist 0}
    }

    foreach {type large} [array get largevalue] {
        test "RPOPLPUSH base case - $type" {
            r del mylist1{t} mylist2{t}
            create_$type mylist1{t} "a $large c d"
            assert_equal d [r rpoplpush mylist1{t} mylist2{t}]
            assert_equal c [r rpoplpush mylist1{t} mylist2{t}]
            assert_equal $large [r rpoplpush mylist1{t} mylist2{t}]
            assert_equal "a" [r lrange mylist1{t} 0 -1]
            assert_equal "$large c d" [r lrange mylist2{t} 0 -1]
            assert_encoding listpack mylist1{t} ;# converted to listpack after shrinking
            assert_encoding $type mylist2{t}
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "LMOVE $wherefrom $whereto base case - $type" {
                    r del mylist1{t} mylist2{t}

                    if {$wherefrom eq "right"} {
                        create_$type mylist1{t} "c d $large a"
                    } else {
                        create_$type mylist1{t} "a $large c d"
                    }
                    assert_equal a [r lmove mylist1{t} mylist2{t} $wherefrom $whereto]
                    assert_equal $large [r lmove mylist1{t} mylist2{t} $wherefrom $whereto]
                    assert_equal "c d" [r lrange mylist1{t} 0 -1]
                    if {$whereto eq "right"} {
                        assert_equal "a $large" [r lrange mylist2{t} 0 -1]
                    } else {
                        assert_equal "$large a" [r lrange mylist2{t} 0 -1]
                    }
                    assert_encoding $type mylist2{t}
                }
            }
        }

        test "RPOPLPUSH with the same list as src and dst - $type" {
            create_$type mylist{t} "a $large c"
            assert_equal "a $large c" [r lrange mylist{t} 0 -1]
            assert_equal c [r rpoplpush mylist{t} mylist{t}]
            assert_equal "c a $large" [r lrange mylist{t} 0 -1]
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "LMOVE $wherefrom $whereto with the same list as src and dst - $type" {
                    if {$wherefrom eq "right"} {
                        create_$type mylist{t} "a $large c"
                        assert_equal "a $large c" [r lrange mylist{t} 0 -1]
                    } else {
                        create_$type mylist{t} "c a $large"
                        assert_equal "c a $large" [r lrange mylist{t} 0 -1]
                    }
                    assert_equal c [r lmove mylist{t} mylist{t} $wherefrom $whereto]
                    if {$whereto eq "right"} {
                        assert_equal "a $large c" [r lrange mylist{t} 0 -1]
                    } else {
                        assert_equal "c a $large" [r lrange mylist{t} 0 -1]
                    }
                }
            }
        }

        foreach {othertype otherlarge} [array get largevalue] {
            test "RPOPLPUSH with $type source and existing target $othertype" {
                create_$type srclist{t} "a b c $large"
                create_$othertype dstlist{t} "$otherlarge"
                assert_equal $large [r rpoplpush srclist{t} dstlist{t}]
                assert_equal c [r rpoplpush srclist{t} dstlist{t}]
                assert_equal "a b" [r lrange srclist{t} 0 -1]
                assert_equal "c $large $otherlarge" [r lrange dstlist{t} 0 -1]

                # When we rpoplpush'ed a large value, dstlist should be
                # converted to the same encoding as srclist.
                if {$type eq "quicklist"} {
                    assert_encoding quicklist dstlist{t}
                }
            }

            foreach wherefrom {left right} {
                foreach whereto {left right} {
                    test "LMOVE $wherefrom $whereto with $type source and existing target $othertype" {
                        create_$othertype dstlist{t} "$otherlarge"

                        if {$wherefrom eq "right"} {
                            create_$type srclist{t} "a b c $large"
                        } else {
                            create_$type srclist{t} "$large c a b"
                        }
                        assert_equal $large [r lmove srclist{t} dstlist{t} $wherefrom $whereto]
                        assert_equal c [r lmove srclist{t} dstlist{t} $wherefrom $whereto]
                        assert_equal "a b" [r lrange srclist{t} 0 -1]

                        if {$whereto eq "right"} {
                            assert_equal "$otherlarge $large c" [r lrange dstlist{t} 0 -1]
                        } else {
                            assert_equal "c $large $otherlarge" [r lrange dstlist{t} 0 -1]
                        }

                        # When we lmoved a large value, dstlist should be
                        # converted to the same encoding as srclist.
                        if {$type eq "quicklist"} {
                            assert_encoding quicklist dstlist{t}
                        }
                    }
                }
            }
        }
    }

    test {RPOPLPUSH against non existing key} {
        r del srclist{t} dstlist{t}
        assert_equal {} [r rpoplpush srclist{t} dstlist{t}]
        assert_equal 0 [r exists srclist{t}]
        assert_equal 0 [r exists dstlist{t}]
    }

    test {RPOPLPUSH against non list src key} {
        r del srclist{t} dstlist{t}
        r set srclist{t} x
        assert_error WRONGTYPE* {r rpoplpush srclist{t} dstlist{t}}
        assert_type string srclist{t}
        assert_equal 0 [r exists newlist{t}]
    }

foreach {type large} [array get largevalue] {
    test "RPOPLPUSH against non list dst key - $type" {
        create_$type srclist{t} "a $large c d"
        r set dstlist{t} x
        assert_error WRONGTYPE* {r rpoplpush srclist{t} dstlist{t}}
        assert_type string dstlist{t}
        assert_equal "a $large c d" [r lrange srclist{t} 0 -1]
    }
}

    test {RPOPLPUSH against non existing src key} {
        r del srclist{t} dstlist{t}
        assert_equal {} [r rpoplpush srclist{t} dstlist{t}]
    } {}

    foreach {type large} [array get largevalue] { 
        test "Basic LPOP/RPOP/LMPOP - $type" {
            create_$type mylist "$large 1 2"
            assert_equal $large [r lpop mylist]
            assert_equal 2 [r rpop mylist]
            assert_equal 1 [r lpop mylist]
            assert_equal 0 [r llen mylist]

            create_$type mylist "$large 1 2"
            assert_equal "mylist $large" [r lmpop 1 mylist left count 1]
            assert_equal {mylist {2 1}} [r lmpop 2 mylist mylist right count 2]
        }
    }

    test {LPOP/RPOP/LMPOP against empty list} {
        r del non-existing-list{t} non-existing-list2{t}

        assert_equal {} [r lpop non-existing-list{t}]
        assert_equal {} [r rpop non-existing-list2{t}]

        assert_equal {} [r lmpop 1 non-existing-list{t} left count 1]
        assert_equal {} [r lmpop 1 non-existing-list{t} left count 10]
        assert_equal {} [r lmpop 2 non-existing-list{t} non-existing-list2{t} right count 1]
        assert_equal {} [r lmpop 2 non-existing-list{t} non-existing-list2{t} right count 10]
    }

    test {LPOP/RPOP/LMPOP NON-BLOCK or BLOCK against non list value} {
        r set notalist{t} foo
        assert_error WRONGTYPE* {r lpop notalist{t}}
        assert_error WRONGTYPE* {r blpop notalist{t} 0}
        assert_error WRONGTYPE* {r rpop notalist{t}}
        assert_error WRONGTYPE* {r brpop notalist{t} 0}

        r del notalist2{t}
        assert_error "WRONGTYPE*" {r lmpop 2 notalist{t} notalist2{t} left count 1}
        assert_error "WRONGTYPE*" {r blmpop 0 2 notalist{t} notalist2{t} left count 1}

        r del notalist{t}
        r set notalist2{t} nolist
        assert_error "WRONGTYPE*" {r lmpop 2 notalist{t} notalist2{t} right count 10}
        assert_error "WRONGTYPE*" {r blmpop 0 2 notalist{t} notalist2{t} left count 1}
    }

    foreach {num} {250 500} {
        test "Mass RPOP/LPOP - $type" {
            r del mylist
            set sum1 0
            for {set i 0} {$i < $num} {incr i} {
                if {$i == [expr $num/2]} {
                    r lpush mylist $large
                }
                r lpush mylist $i
                incr sum1 $i
            }
            assert_encoding $type mylist
            set sum2 0
            for {set i 0} {$i < [expr $num/2]} {incr i} {
                incr sum2 [r lpop mylist]
                incr sum2 [r rpop mylist]
            }
            assert_equal $sum1 $sum2
        }
    }

    test {LMPOP with illegal argument} {
        assert_error "ERR wrong number of arguments for 'lmpop' command" {r lmpop}
        assert_error "ERR wrong number of arguments for 'lmpop' command" {r lmpop 1}
        assert_error "ERR wrong number of arguments for 'lmpop' command" {r lmpop 1 mylist{t}}

        assert_error "ERR numkeys*" {r lmpop 0 mylist{t} LEFT}
        assert_error "ERR numkeys*" {r lmpop a mylist{t} LEFT}
        assert_error "ERR numkeys*" {r lmpop -1 mylist{t} RIGHT}

        assert_error "ERR syntax error*" {r lmpop 1 mylist{t} bad_where}
        assert_error "ERR syntax error*" {r lmpop 1 mylist{t} LEFT bar_arg}
        assert_error "ERR syntax error*" {r lmpop 1 mylist{t} RIGHT LEFT}
        assert_error "ERR syntax error*" {r lmpop 1 mylist{t} COUNT}
        assert_error "ERR syntax error*" {r lmpop 1 mylist{t} LEFT COUNT 1 COUNT 2}
        assert_error "ERR syntax error*" {r lmpop 2 mylist{t} mylist2{t} bad_arg}

        assert_error "ERR count*" {r lmpop 1 mylist{t} LEFT COUNT 0}
        assert_error "ERR count*" {r lmpop 1 mylist{t} RIGHT COUNT a}
        assert_error "ERR count*" {r lmpop 1 mylist{t} LEFT COUNT -1}
        assert_error "ERR count*" {r lmpop 2 mylist{t} mylist2{t} RIGHT COUNT -1}
    }

foreach {type large} [array get largevalue] {
    test "LMPOP single existing list - $type" {
        # Same key multiple times.
        create_$type mylist{t} "a b $large d e f"
        assert_equal {mylist{t} {a b}} [r lmpop 2 mylist{t} mylist{t} left count 2]
        assert_equal {mylist{t} {f e}} [r lmpop 2 mylist{t} mylist{t} right count 2]
        assert_equal 2 [r llen mylist{t}]

        # First one exists, second one does not exist.
        create_$type mylist{t} "a b $large d e"
        r del mylist2{t}
        assert_equal {mylist{t} a} [r lmpop 2 mylist{t} mylist2{t} left count 1]
        assert_equal 4 [r llen mylist{t}]
        assert_equal "mylist{t} {e d $large b}" [r lmpop 2 mylist{t} mylist2{t} right count 10]
        assert_equal {} [r lmpop 2 mylist{t} mylist2{t} right count 1]

        # First one does not exist, second one exists.
        r del mylist{t}
        create_$type mylist2{t} "1 2 $large 4 5"
        assert_equal {mylist2{t} 5} [r lmpop 2 mylist{t} mylist2{t} right count 1]
        assert_equal 4 [r llen mylist2{t}]
        assert_equal "mylist2{t} {1 2 $large 4}" [r lmpop 2 mylist{t} mylist2{t} left count 10]

        assert_equal 0 [r exists mylist{t} mylist2{t}]
    }

    test "LMPOP multiple existing lists - $type" {
        create_$type mylist{t} "a b $large d e"
        create_$type mylist2{t} "1 2 $large 4 5"

        # Pop up from the first key.
        assert_equal {mylist{t} {a b}} [r lmpop 2 mylist{t} mylist2{t} left count 2]
        assert_equal 3 [r llen mylist{t}]
        assert_equal "mylist{t} {e d $large}" [r lmpop 2 mylist{t} mylist2{t} right count 3]
        assert_equal 0 [r exists mylist{t}]

        # Pop up from the second key.
        assert_equal "mylist2{t} {1 2 $large}" [r lmpop 2 mylist{t} mylist2{t} left count 3]
        assert_equal 2 [r llen mylist2{t}]
        assert_equal {mylist2{t} {5 4}} [r lmpop 2 mylist{t} mylist2{t} right count 2]
        assert_equal 0 [r exists mylist{t}]

        # Pop up all elements.
        create_$type mylist{t} "a $large c"
        create_$type mylist2{t} "1 $large 3"
        assert_equal "mylist{t} {a $large c}" [r lmpop 2 mylist{t} mylist2{t} left count 10]
        assert_equal 0 [r llen mylist{t}]
        assert_equal "mylist2{t} {3 $large 1}" [r lmpop 2 mylist{t} mylist2{t} right count 10]
        assert_equal 0 [r llen mylist2{t}]
        assert_equal 0 [r exists mylist{t} mylist2{t}]
    }
}

    test {LMPOP propagate as pop with count command to replica} {
        set repl [attach_to_replication_stream]

        # left/right propagate as lpop/rpop with count
        r lpush mylist{t} a b c

        # Pop elements from one list.
        r lmpop 1 mylist{t} left count 1
        r lmpop 1 mylist{t} right count 1

        # Now the list have only one element
        r lmpop 2 mylist{t} mylist2{t} left count 10

        # No elements so we don't propagate.
        r lmpop 2 mylist{t} mylist2{t} left count 10

        # Pop elements from the second list.
        r rpush mylist2{t} 1 2 3
        r lmpop 2 mylist{t} mylist2{t} left count 2
        r lmpop 2 mylist{t} mylist2{t} right count 1

        # Pop all elements.
        r rpush mylist{t} a b c
        r rpush mylist2{t} 1 2 3
        r lmpop 2 mylist{t} mylist2{t} left count 10
        r lmpop 2 mylist{t} mylist2{t} right count 10

        assert_replication_stream $repl {
            {select *}
            {lpush mylist{t} a b c}
            {lpop mylist{t} 1}
            {rpop mylist{t} 1}
            {lpop mylist{t} 1}
            {rpush mylist2{t} 1 2 3}
            {lpop mylist2{t} 2}
            {rpop mylist2{t} 1}
            {rpush mylist{t} a b c}
            {rpush mylist2{t} 1 2 3}
            {lpop mylist{t} 3}
            {rpop mylist2{t} 3}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    foreach {type large} [array get largevalue] {
        test "LRANGE basics - $type" {
            create_$type mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {1 2 3 4 5 6 7 8} [r lrange mylist 1 -2]
            assert_equal {7 8 9} [r lrange mylist -3 -1]
            assert_equal {4} [r lrange mylist 4 4]
        }

        test "LRANGE inverted indexes - $type" {
            create_$type mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {} [r lrange mylist 6 2]
        }

        test "LRANGE out of range indexes including the full list - $type" {
            create_$type mylist "$large 1 2 3"
            assert_equal "$large 1 2 3" [r lrange mylist -1000 1000]
        }

        test "LRANGE out of range negative end index - $type" {
            create_$type mylist "$large 1 2 3"
            assert_equal $large [r lrange mylist 0 -4]
            assert_equal {} [r lrange mylist 0 -5]
        }
    }

    test {LRANGE against non existing key} {
        assert_equal {} [r lrange nosuchkey 0 1]
    }

    test {LRANGE with start > end yields an empty array for backward compatibility} {
        create_$type mylist "1 $large 3"
        assert_equal {} [r lrange mylist 1 0]
        assert_equal {} [r lrange mylist -1 -2]
    }

    foreach {type large} [array get largevalue] {
        proc trim_list {type min max} {
            upvar 1 large large
            r del mylist
            create_$type mylist "1 2 3 4 $large"
            r ltrim mylist $min $max
            r lrange mylist 0 -1
        }

        test "LTRIM basics - $type" {
            assert_equal "1" [trim_list $type 0 0]
            assert_equal "1 2" [trim_list $type 0 1]
            assert_equal "1 2 3" [trim_list $type 0 2]
            assert_equal "2 3" [trim_list $type 1 2]
            assert_equal "2 3 4 $large" [trim_list $type 1 -1]
            assert_equal "2 3 4" [trim_list $type 1 -2]
            assert_equal "4 $large" [trim_list $type -2 -1]
            assert_equal "$large" [trim_list $type -1 -1]
            assert_equal "1 2 3 4 $large" [trim_list $type -5 -1]
            assert_equal "1 2 3 4 $large" [trim_list $type -10 10]
            assert_equal "1 2 3 4 $large" [trim_list $type 0 5]
            assert_equal "1 2 3 4 $large" [trim_list $type 0 10]
        }

        test "LTRIM out of range negative end index - $type" {
            assert_equal {1} [trim_list $type 0 -5]
            assert_equal {} [trim_list $type 0 -6]
        }

        test "LSET - $type" {
            create_$type mylist "99 98 $large 96 95"
            r lset mylist 1 foo
            r lset mylist -1 bar
            assert_equal "99 foo $large 96 bar" [r lrange mylist 0 -1]
        }

        test "LSET out of range index - $type" {
            assert_error ERR*range* {r lset mylist 10 foo}
        }
    }

    test {LSET against non existing key} {
        assert_error ERR*key* {r lset nosuchkey 10 foo}
    }

    test {LSET against non list value} {
        r set nolist foobar
        assert_error WRONGTYPE* {r lset nolist 0 foo}
    }

    foreach {type e} [array get largevalue] {
        test "LREM remove all the occurrences - $type" {
            create_$type mylist "$e foo bar foobar foobared zap bar test foo"
            assert_equal 2 [r lrem mylist 0 bar]
            assert_equal "$e foo foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM remove the first occurrence - $type" {
            assert_equal 1 [r lrem mylist 1 foo]
            assert_equal "$e foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM remove non existing element - $type" {
            assert_equal 0 [r lrem mylist 1 nosuchelement]
            assert_equal "$e foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count - $type" {
            create_$type mylist "$e foo bar foobar foobared zap bar test foo foo"
            assert_equal 1 [r lrem mylist -1 bar]
            assert_equal "$e foo bar foobar foobared zap test foo foo" [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count (2) - $type" {
            assert_equal 2 [r lrem mylist -2 foo]
            assert_equal "$e foo bar foobar foobared zap test" [r lrange mylist 0 -1]
        }

        test "LREM deleting objects that may be int encoded - $type" {
            create_$type myotherlist "$e 1 2 3"
            assert_equal 1 [r lrem myotherlist 1 2]
            assert_equal 3 [r llen myotherlist]
        }
    }

    test "Regression for bug 593 - chaining BRPOPLPUSH with other blocking cmds" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 brpoplpush a{t} b{t} 0
        $rd1 brpoplpush a{t} b{t} 0
        wait_for_blocked_clients_count 1
        $rd2 brpoplpush b{t} c{t} 0
        wait_for_blocked_clients_count 2
        r lpush a{t} data
        $rd1 close
        $rd2 close
        r ping
    } {PONG}

    test "BLPOP/BLMOVE should increase dirty" {
        r del lst{t} lst1{t}
        set rd [redis_deferring_client]

        set dirty [s rdb_changes_since_last_save]
        $rd blpop lst{t} 0
        wait_for_blocked_client
        r lpush lst{t} a
        assert_equal {lst{t} a} [$rd read]
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 2}

        set dirty [s rdb_changes_since_last_save]
        $rd blmove lst{t} lst1{t} left left 0
        wait_for_blocked_client
        r lpush lst{t} a
        assert_equal {a} [$rd read]
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 2}

        $rd close
    }

foreach {pop} {BLPOP BLMPOP_RIGHT} {
    test "client unblock tests" {
        r del l
        set rd [redis_deferring_client]
        $rd client id
        set id [$rd read]

        # test default args
        bpop_command $rd $pop l 0
        wait_for_blocked_client
        r client unblock $id
        assert_equal {} [$rd read]

        # test with timeout
        bpop_command $rd $pop l 0
        wait_for_blocked_client
        r client unblock $id TIMEOUT
        assert_equal {} [$rd read]

        # test with error
        bpop_command $rd $pop l 0
        wait_for_blocked_client
        r client unblock $id ERROR
        catch {[$rd read]} e
        assert_equal $e "UNBLOCKED client unblocked via CLIENT UNBLOCK"

        # test with invalid client id
        catch {[r client unblock asd]} e
        assert_equal $e "ERR value is not an integer or out of range"

        # test with non blocked client
        set myid [r client id]
        catch {[r client unblock $myid]} e
        assert_equal $e {invalid command name "0"}

        # finally, see the this client and list are still functional
        bpop_command $rd $pop l 0
        wait_for_blocked_client
        r lpush l foo
        assert_equal {l foo} [$rd read]
        $rd close
    }
}

    foreach {max_lp_size large} "3 $largevalue(listpack) -1 $largevalue(quicklist)" {
        test "List listpack -> quicklist encoding conversion" {
            set origin_conf [config_get_set list-max-listpack-size $max_lp_size]

            # RPUSH
            create_listpack lst "a b c"
            r RPUSH lst $large
            assert_encoding quicklist lst

            # LINSERT
            create_listpack lst "a b c"
            r LINSERT lst after b $large
            assert_encoding quicklist lst

            # LSET
            create_listpack lst "a b c"
            r LSET lst 0 $large
            assert_encoding quicklist lst

            # LMOVE
            create_quicklist lsrc{t} "a b c $large"
            create_listpack ldes{t} "d e f"
            r LMOVE lsrc{t} ldes{t} right right
            assert_encoding quicklist ldes{t}

            r config set list-max-listpack-size $origin_conf
        }
    }

    test "List quicklist -> listpack encoding conversion" {
        set origin_conf [config_get_set list-max-listpack-size 3]

        # RPOP
        create_quicklist lst "a b c d"
        r RPOP lst 3
        assert_encoding listpack lst

        # LREM
        create_quicklist lst "a a a d"
        r LREM lst 3 a
        assert_encoding listpack lst

        # LTRIM
        create_quicklist lst "a b c d"
        r LTRIM lst 1 1
        assert_encoding listpack lst

        r config set list-max-listpack-size -1

        # RPOP
        create_quicklist lst "a b c $largevalue(quicklist)"
        r RPOP lst 1
        assert_encoding listpack lst

        # LREM
        create_quicklist lst "a $largevalue(quicklist)"
        r LREM lst 1 $largevalue(quicklist)
        assert_encoding listpack lst

        # LTRIM
        create_quicklist lst "a b $largevalue(quicklist)"
        r LTRIM lst 0 1
        assert_encoding listpack lst

        # LSET
        create_quicklist lst "$largevalue(quicklist) a b"
        r RPOP lst 2
        assert_encoding quicklist lst
        r LSET lst -1 c
        assert_encoding listpack lst

        r config set list-max-listpack-size $origin_conf
    }

    test "List encoding conversion when RDB loading" {
        set origin_conf [config_get_set list-max-listpack-size 3]
        create_listpack lst "a b c"

        # list is still a listpack after DEBUG RELOAD
        r DEBUG RELOAD
        assert_encoding listpack lst

        # list is still a quicklist after DEBUG RELOAD
        r RPUSH lst d
        r DEBUG RELOAD
        assert_encoding quicklist lst

        # when a quicklist has only one packed node, it will be
        # converted to listpack during rdb loading
        r RPOP lst
        assert_encoding quicklist lst 
        r DEBUG RELOAD
        assert_encoding listpack lst

        r config set list-max-listpack-size $origin_conf
    } {OK} {needs:debug}

    test "List invalid list-max-listpack-size config" {
        # ​When list-max-listpack-size is 0 we treat it as 1 and it'll
        # still be listpack if there's a single element in the list.
        r config set list-max-listpack-size 0
        r DEL lst
        r RPUSH lst a
        assert_encoding listpack lst
        r RPUSH lst b
        assert_encoding quicklist lst

        # When list-max-listpack-size < -5 we treat it as -5.
        r config set list-max-listpack-size -6
        r DEL lst
        r RPUSH lst [string repeat "x" 60000]
        assert_encoding listpack lst
        # Converted to quicklist when the size of listpack exceed 65536
        r RPUSH lst [string repeat "x" 5536]
        assert_encoding quicklist lst
    }

    test "List of various encodings" {
        r del k
        r lpush k 127 ;# ZIP_INT_8B
        r lpush k 32767 ;# ZIP_INT_16B
        r lpush k 2147483647 ;# ZIP_INT_32B
        r lpush k 9223372036854775808 ;# ZIP_INT_64B
        r lpush k 0 ;# ZIP_INT_IMM_MIN
        r lpush k 12 ;# ZIP_INT_IMM_MAX
        r lpush k [string repeat x 31] ;# ZIP_STR_06B
        r lpush k [string repeat x 8191] ;# ZIP_STR_14B
        r lpush k [string repeat x 65535] ;# ZIP_STR_32B
        assert_encoding quicklist k ;# exceeds the size limit of quicklist node
        set k [r lrange k 0 -1]
        set dump [r dump k]

        # coverage for objectComputeSize
        assert_morethan [memory_usage k] 0

        config_set sanitize-dump-payload no mayfail
        r restore kk 0 $dump replace
        assert_encoding quicklist kk
        set kk [r lrange kk 0 -1]

        # try some forward and backward searches to make sure all encodings
        # can be traversed
        assert_equal [r lindex kk 5] {9223372036854775808}
        assert_equal [r lindex kk -5] {0}
        assert_equal [r lpos kk foo rank 1] {}
        assert_equal [r lpos kk foo rank -1] {}

        # make sure the values are right
        assert_equal $k $kk
        assert_equal [lpop k] [string repeat x 65535]
        assert_equal [lpop k] [string repeat x 8191]
        assert_equal [lpop k] [string repeat x 31]
        set _ $k
    } {12 0 9223372036854775808 2147483647 32767 127}

    test "List of various encodings - sanitize dump" {
        config_set sanitize-dump-payload yes mayfail
        r restore kk 0 $dump replace
        assert_encoding quicklist kk
        set k [r lrange k 0 -1]
        set kk [r lrange kk 0 -1]

        # make sure the values are right
        assert_equal $k $kk
        assert_equal [lpop k] [string repeat x 65535]
        assert_equal [lpop k] [string repeat x 8191]
        assert_equal [lpop k] [string repeat x 31]
        set _ $k
    } {12 0 9223372036854775808 2147483647 32767 127}
    
    test "Unblock fairness is kept while pipelining" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        
        # delete the list in case already exists
        r del mylist
        
        # block a client on the list
        $rd1 BLPOP mylist 0
        wait_for_blocked_clients_count 1
        
        # pipeline on other client a list push and a blocking pop
        # we should expect the fairness to be kept and have $rd1
        # being unblocked
        set buf ""
        append buf "LPUSH mylist 1\r\n"
        append buf "BLPOP mylist 0\r\n"
        $rd2 write $buf
        $rd2 flush
        
        # we check that we still have 1 blocked client
        # and that the first blocked client has been served
        assert_equal [$rd1 read] {mylist 1}
        assert_equal [$rd2 read] {1}
        wait_for_blocked_clients_count 1
        
        # We no unblock the last client and verify it was served last 
        r LPUSH mylist 2
        wait_for_blocked_clients_count 0
        assert_equal [$rd2 read] {mylist 2}
        
        $rd1 close
        $rd2 close
    }
    
    test "Unblock fairness is kept during nested unblock" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set rd3 [redis_deferring_client]
        
        # delete the list in case already exists
        r del l1{t} l2{t} l3{t}
        
        # block a client on the list
        $rd1 BRPOPLPUSH l1{t} l3{t} 0
        wait_for_blocked_clients_count 1
        
        $rd2 BLPOP l2{t} 0
        wait_for_blocked_clients_count 2
        
        $rd3 BLMPOP 0 2 l2{t} l3{t} LEFT COUNT 1
        wait_for_blocked_clients_count 3
        
        r multi
        r lpush l1{t} 1
        r lpush l2{t} 2
        r exec
        
        wait_for_blocked_clients_count 0
        
        assert_equal [$rd1 read] {1}
        assert_equal [$rd2 read] {l2{t} 2}
        assert_equal [$rd3 read] {l3{t} 1}
        
        $rd1 close
        $rd2 close
        $rd3 close
    }
    
    test "Blocking command accounted only once in commandstats" {
        # cleanup first
        r del mylist
        
        # create a test client
        set rd [redis_deferring_client]
        
        # reset the server stats
        r config resetstat
        
        # block a client on the list
        $rd BLPOP mylist 0
        wait_for_blocked_clients_count 1
        
        # unblock the list
        r LPUSH mylist 1
        wait_for_blocked_clients_count 0
        
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdrstat blpop r]
        
        $rd close
    }
    
    test "Blocking command accounted only once in commandstats after timeout" {
        # cleanup first
        r del mylist
        
        # create a test client
        set rd [redis_deferring_client]
        $rd client id
        set id [$rd read]

        # reset the server stats
        r config resetstat
        
        # block a client on the list
        $rd BLPOP mylist 0
        wait_for_blocked_clients_count 1
        
        # unblock the client on timeout
        r client unblock $id timeout
        
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdrstat blpop r]
        
        $rd close
    }

    test {Command being unblocked cause another command to get unblocked execution order test} {
        r del src{t} dst{t} key1{t} key2{t} key3{t}
        set repl [attach_to_replication_stream]

        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set rd3 [redis_deferring_client]

        $rd1 blmove src{t} dst{t} left right 0
        wait_for_blocked_clients_count 1

        $rd2 blmove dst{t} src{t} right left 0
        wait_for_blocked_clients_count 2

        # Create a pipeline of commands that will be processed in one socket read.
        # Insert two set commands before and after lpush to observe the execution order.
        set buf ""
        append buf "set key1{t} value1\r\n"
        append buf "lpush src{t} dummy\r\n"
        append buf "set key2{t} value2\r\n"
        $rd3 write $buf
        $rd3 flush

        wait_for_blocked_clients_count 0

        r set key3{t} value3

        # If a command being unblocked causes another command to get unblocked, like a BLMOVE would do,
        # then the new unblocked command will get processed right away rather than wait for later.
        # If the set command occurs between two lmove commands, the results are not as expected.
        assert_replication_stream $repl {
            {select *}
            {set key1{t} value1}
            {lpush src{t} dummy}
            {lmove src{t} dst{t} left right}
            {lmove dst{t} src{t} right left}
            {set key2{t} value2}
            {set key3{t} value3}
        }

        $rd1 close
        $rd2 close
        $rd3 close

        close_replication_stream $repl
    } {} {needs:repl}

} ;# stop servers
