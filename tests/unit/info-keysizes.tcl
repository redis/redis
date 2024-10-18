################################################################################
# Test the "info keysizes" command.
# The command returns a histogram of the sizes of keys in the database.
################################################################################

# Query and Strip result of "info keysizes" from header, spaces, and newlines.
proc get_stripped_info {server} {
    set infoStripped [string map {
        "# Keysizes" ""
        " " "" "\n" "" "\r" ""
    } [$server info keysizes] ]
    return $infoStripped
}

# Verify output of "info keysizes" command is as expected.
#
# Arguments:
#  cmd         -  A command that should be run before the verification.
#  expOutput   -  This is a string that represents the expected output abbreviated.
#                 Instead of the output of "strings_len_exp_distrib" write "STR". 
#                 Similarly for LIST, SET, ZSET and HASH. Spaces and newlines are 
#                 ignored.
#  waitCond    -  If set to 1, the function wait_for_condition 50x20msec for the 
#                 expOutput to match the actual output. 
# 
# (replicaMode) -  Global variable that indicates if the test is running in replica 
#                  mode. If so, run the command on leader, verify the output. Then wait
#                  for the replica to catch up and verify the output on the replica
#                  as well. Otherwise, just run the command on the leader and verify 
#                  the output.
proc run_cmd_verify_hist {cmd expOutput {waitCond 0} } {   
    uplevel 1 $cmd
    global replicaMode

    # ref the leader with `server` variable 
    if {$replicaMode eq 1} { set server [srv -1 client] } else { set server [srv 0 client] }
    
    # Replace all placeholders with the actual values. Remove spaces & newlines.
    set expStripped [string map {
        "STR" "distrib_strings_sizes" 
        "LIST" "distrib_lists_items" 
        "SET" "distrib_sets_items" 
        "ZSET" "distrib_zsets_items" 
        "HASH" "distrib_hashes_items"
        " " "" "\n" "" "\r" ""
    } $expOutput]
                     
    if {$waitCond} {
         wait_for_condition 50 20 {
            $expStripped eq [get_stripped_info $server]
         } else {
             fail "Unexpected replica KEYSIZES. Expected: `$expOutput` but got: `$infoStripped`"
         }                
    } else {
        assert_equal $expStripped [get_stripped_info $server]
    }
    
    # If we are testing `replicaMode` then need to wait for the replica to catch up    
    if {$replicaMode eq 1} {        
         wait_for_condition 50 20 {
            $expStripped eq [get_stripped_info $server]
         } else {
             fail "Unexpected replica KEYSIZES. Expected: `$expOutput` but got: `$infoStripped`"
         }
    }
}

proc test_all_keysizes { {replMode 0} } {
    # If in replica mode then update global var `replicaMode` so function 
    # `run_cmd_verify_hist` knows to run the command on the leader and then
    # wait for the replica to catch up. 
    global replicaMode
    set replicaMode $replMode    
    # ref the leader with `server` variable
    if {$replicaMode eq 1} { 
        set server [srv -1 client] 
        set suffixRepl "(replica)"
    } else { 
        set server [srv 0 client]
        set suffixRepl "" 
    }    
        
    test "KEYSIZES - Test i'th bin counts keysizes between (2^i) and (2^(i+1)-1) as expected $suffixRepl" {
        set base_string ""
        run_cmd_verify_hist {$server FLUSHALL} {}
        for {set i 1} {$i <= 10} {incr i} {
            append base_string "x"
            set log_value [expr {1 << int(log($i) / log(2))}]
            #puts "Iteration $i: $base_string (Log base 2 pattern: $log_value)"
            run_cmd_verify_hist {$server set mykey $base_string}  "db0_STR:$log_value=1"            
        }
    }

    test "KEYSIZES - Histogram of values of Bytes, Kilo and Mega $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server set x 0123456789ABCDEF} {db0_STR:16=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:32=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:64=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:128=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:256=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:512=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:1K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:2K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:4K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:8K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:16K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:32K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:64K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:128K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:256K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:512K=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:1M=1}
        run_cmd_verify_hist {$server APPEND x [$server get x]} {db0_STR:2M=1}               
    }
            
    test "KEYSIZES - Test List $suffixRepl" {
        # FLUSHALL
        run_cmd_verify_hist {$server FLUSHALL} {}
        # RPUSH
        run_cmd_verify_hist {$server RPUSH l1 1 2 3 4 5} {db0_LIST:4=1}
        run_cmd_verify_hist {$server RPUSH l1 6 7 8 9} {db0_LIST:8=1}                        
        # Test also LPUSH, RPUSH, LPUSHX, RPUSHX
        run_cmd_verify_hist {$server LPUSH l2 1} {db0_LIST:1=1,8=1}
        run_cmd_verify_hist {$server LPUSH l2 2} {db0_LIST:2=1,8=1}
        run_cmd_verify_hist {$server LPUSHX l2 3} {db0_LIST:2=1,8=1}
        run_cmd_verify_hist {$server RPUSHX l2 4} {db0_LIST:4=1,8=1}
        # RPOP
        run_cmd_verify_hist {$server RPOP l1} {db0_LIST:4=1,8=1}
        run_cmd_verify_hist {$server RPOP l1} {db0_LIST:4=2}         
         # DEL
        run_cmd_verify_hist {$server DEL l1} {db0_LIST:4=1}        
        # LINSERT, LTRIM
        run_cmd_verify_hist {$server RPUSH l3 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14} {db0_LIST:4=1,8=1}
        run_cmd_verify_hist {$server LINSERT l3 AFTER 9 10} {db0_LIST:4=1,16=1}
        run_cmd_verify_hist {$server LTRIM l3 0 8} {db0_LIST:4=1,8=1}       
        # DEL
        run_cmd_verify_hist {$server DEL l3} {db0_LIST:4=1}
        run_cmd_verify_hist {$server DEL l2} {}        
        # LMOVE, BLMOVE
        run_cmd_verify_hist {$server RPUSH l4 1 2 3 4 5 6 7 8} {db0_LIST:8=1}
        run_cmd_verify_hist {$server LMOVE l4 l5 LEFT LEFT} {db0_LIST:1=1,4=1} 
        run_cmd_verify_hist {$server LMOVE l4 l5 RIGHT RIGHT} {db0_LIST:2=1,4=1}
        run_cmd_verify_hist {$server LMOVE l4 l5 LEFT RIGHT} {db0_LIST:2=1,4=1}
        run_cmd_verify_hist {$server LMOVE l4 l5 RIGHT LEFT} {db0_LIST:4=2}
        run_cmd_verify_hist {$server BLMOVE l4 l5 RIGHT LEFT 0} {db0_LIST:2=1,4=1}        
        # DEL
        run_cmd_verify_hist {$server DEL l4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server DEL l5} {}
        # LMPOP
        run_cmd_verify_hist {$server RPUSH l6 1 2 3 4 5 6 7 8 9 10} {db0_LIST:8=1}
        run_cmd_verify_hist {$server LMPOP 1 l6 LEFT COUNT 2} {db0_LIST:8=1}
        run_cmd_verify_hist {$server LMPOP 1 l6 LEFT COUNT 1} {db0_LIST:4=1}
        run_cmd_verify_hist {$server LMPOP 1 l6 LEFT COUNT 6} {db0_LIST:1=1}
        # LPOP
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l7 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server LPOP l7} {db0_LIST:2=1}
        # LREM
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l8 1 x 3 x 5 x 7 x 9 10} {db0_LIST:8=1}
        run_cmd_verify_hist {$server LREM l8 3 x} {db0_LIST:4=1}        
        # EXPIRE 
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l9 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server PEXPIRE l9 1} {db0_LIST:4=1}
        run_cmd_verify_hist {} {} 1
        # SET overwrites
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l9 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server SET l9 1234567} {db0_STR:4=1}
        run_cmd_verify_hist {$server DEL l9} {}                            
    } {} {cluster:skip}

    test "KEYSIZES - Test SET $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        # SADD
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5} {db0_SET:4=1}
        run_cmd_verify_hist {$server SADD s1 6 7 8} {db0_SET:8=1}                        
        # Test also SADD, SREM, SMOVE, SPOP
        run_cmd_verify_hist {$server SADD s2 1} {db0_SET:1=1,8=1}
        run_cmd_verify_hist {$server SADD s2 2} {db0_SET:2=1,8=1}
        run_cmd_verify_hist {$server SREM s2 3} {db0_SET:2=1,8=1}
        run_cmd_verify_hist {$server SMOVE s2 s3 2} {db0_SET:1=2,8=1}
        run_cmd_verify_hist {$server SPOP s3} {db0_SET:1=1,8=1}
        run_cmd_verify_hist {$server SPOP s2} {db0_SET:8=1}
        run_cmd_verify_hist {$server SPOP s1} {db0_SET:4=1}
        run_cmd_verify_hist {$server del s1} {}
        
        # SDIFFSTORE
        run_cmd_verify_hist {$server flushall} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server SADD s2 6 7 8 9 A B C D} {db0_SET:8=2}
        run_cmd_verify_hist {$server SDIFFSTORE s3 s1 s2} {db0_SET:4=1,8=2}        
        #SINTERSTORE
        run_cmd_verify_hist {$server flushall} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server SADD s2 6 7 8 9 A B C D} {db0_SET:8=2}
        run_cmd_verify_hist {$server SINTERSTORE s3 s1 s2} {db0_SET:2=1,8=2}        
        #SUNIONSTORE
        run_cmd_verify_hist {$server flushall} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server SADD s2 6 7 8 9 A B C D} {db0_SET:8=2}
        run_cmd_verify_hist {$server SUNIONSTORE s3 s1 s2} {db0_SET:8=3}
        run_cmd_verify_hist {$server SADD s4 E F G H} {db0_SET:4=1,8=3}
        run_cmd_verify_hist {$server SUNIONSTORE s5 s3 s4} {db0_SET:4=1,8=3,16=1}
        # DEL
        run_cmd_verify_hist {$server flushall} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server DEL s1} {}
        # EXPIRE
        run_cmd_verify_hist {$server flushall} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server PEXPIRE s1 1} {db0_SET:8=1}
        run_cmd_verify_hist {} {} 1
        # SET overwrites
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5 6 7 8} {db0_SET:8=1}
        run_cmd_verify_hist {$server SET s1 1234567} {db0_STR:4=1}
        run_cmd_verify_hist {$server DEL s1} {}        
    } {} {cluster:skip}
     
    test "KEYSIZES - Test ZSET $suffixRepl" {
        # ZADD, ZREM
        run_cmd_verify_hist {$server FLUSHALL} {}        
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZADD z1 6 f 7 g 8 h 9 i} {db0_ZSET:8=1}
        run_cmd_verify_hist {$server ZADD z2 1 a} {db0_ZSET:1=1,8=1}
        run_cmd_verify_hist {$server ZREM z1 a} {db0_ZSET:1=1,8=1}
        run_cmd_verify_hist {$server ZREM z1 b} {db0_ZSET:1=1,4=1}        
        # ZREMRANGEBYSCORE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZREMRANGEBYSCORE z1 -inf (2} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZREMRANGEBYSCORE z1 -inf (3} {db0_ZSET:2=1}        
        # ZREMRANGEBYRANK
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e 6 f} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZREMRANGEBYRANK z1 0 1} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZREMRANGEBYRANK z1 0 0} {db0_ZSET:2=1}        
        # ZREMRANGEBYLEX
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 0 a 0 b 0 c 0 d 0 e 0 f} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZREMRANGEBYLEX z1 - (d} {db0_ZSET:2=1}
        # ZUNIONSTORE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZADD z2 6 f 7 g 8 h 9 i} {db0_ZSET:4=2}
        run_cmd_verify_hist {$server ZUNIONSTORE z3 2 z1 z2} {db0_ZSET:4=2,8=1}
        # ZINTERSTORE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZADD z2 3 c 4 d 5 e 6 f} {db0_ZSET:4=2}
        run_cmd_verify_hist {$server ZINTERSTORE z3 2 z1 z2} {db0_ZSET:2=1,4=2}
        # BZPOPMIN, BZPOPMAX
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server BZPOPMIN z1 0} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server BZPOPMAX z1 0} {db0_ZSET:2=1}
        # ZDIFFSTORE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZADD z2 3 c 4 d 5 e 6 f} {db0_ZSET:4=2}
        run_cmd_verify_hist {$server ZDIFFSTORE z3 2 z1 z2} {db0_ZSET:2=1,4=2}
        # ZINTERSTORE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server ZADD z2 3 c 4 d 5 e 6 f} {db0_ZSET:4=2}
        run_cmd_verify_hist {$server ZINTERSTORE z3 2 z1 z2} {db0_ZSET:2=1,4=2}
        # DEL
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server DEL z1} {}
        # EXPIRE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server PEXPIRE z1 1} {db0_ZSET:4=1}
        run_cmd_verify_hist {} {} 1
        # SET overwrites
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c 4 d 5 e} {db0_ZSET:4=1}
        run_cmd_verify_hist {$server SET z1 1234567} {db0_STR:4=1}
        run_cmd_verify_hist {$server DEL z1} {}
    } {} {cluster:skip}    
    
    test "KEYSIZES - Test STRING $suffixRepl" {        
        # SETRANGE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SET s2 1234567890} {db0_STR:8=1}
        run_cmd_verify_hist {$server SETRANGE s2 10 123456} {db0_STR:16=1}
        # MSET, MSETNX
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server MSET s3 1 s4 2 s5 3} {db0_STR:1=3}
        run_cmd_verify_hist {$server MSETNX s6 1 s7 2 s8 3} {db0_STR:1=6}      
        # DEL
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SET s9 1234567890} {db0_STR:8=1}
        run_cmd_verify_hist {$server DEL s9} {}
        #EXPIRE
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SET s10 1234567890} {db0_STR:8=1}
        run_cmd_verify_hist {$server PEXPIRE s10 1} {db0_STR:8=1}
        run_cmd_verify_hist {} {} 1
        # SET (+overwrite)
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SET s1 1024} {db0_STR:4=1}
        run_cmd_verify_hist {$server SET s1 842} {db0_STR:2=1}
        run_cmd_verify_hist {$server SET s1 2} {db0_STR:1=1}
        run_cmd_verify_hist {$server SET s1 1234567} {db0_STR:4=1}        
    } {} {cluster:skip}
    
    foreach type {listpackex hashtable} {
        # Test different implementations of hash tables and listpacks
        if {$type eq "hashtable"} {
            $server config set hash-max-listpack-entries 0
        } else {
            $server config set hash-max-listpack-entries 512
        }
    
        test "KEYSIZES - Test HASH ($type) $suffixRepl" {
            # HSETNX
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server HSETNX h1 1 1} {db0_HASH:1=1}
            run_cmd_verify_hist {$server HSETNX h1 2 2} {db0_HASH:2=1}
            # HSET, HDEL
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server HSET h2 1 1} {db0_HASH:1=1}
            run_cmd_verify_hist {$server HSET h2 2 2} {db0_HASH:2=1}
            run_cmd_verify_hist {$server HDEL h2 1}   {db0_HASH:1=1}
            run_cmd_verify_hist {$server HDEL h2 2}   {}
            # HMSET
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server HMSET h1 1 1 2 2 3 3} {db0_HASH:2=1}
            run_cmd_verify_hist {$server HMSET h1 1 1 2 2 3 3} {db0_HASH:2=1}
            run_cmd_verify_hist {$server HMSET h1 1 1 2 2 3 3 4 4} {db0_HASH:4=1}
                    
            # HINCRBY
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server hincrby h1 f1 10} {db0_HASH:1=1}
            run_cmd_verify_hist {$server hincrby h1 f1 10} {db0_HASH:1=1}
            run_cmd_verify_hist {$server hincrby h1 f2 20} {db0_HASH:2=1}
            # HINCRBYFLOAT
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server hincrbyfloat h1 f1 10.5} {db0_HASH:1=1}
            run_cmd_verify_hist {$server hincrbyfloat h1 f1 10.5} {db0_HASH:1=1}
            run_cmd_verify_hist {$server hincrbyfloat h1 f2 10.5} {db0_HASH:2=1}
            # HEXPIRE
            run_cmd_verify_hist {$server FLUSHALL} {}
            run_cmd_verify_hist {$server HSET h1 f1 1} {db0_HASH:1=1}
            run_cmd_verify_hist {$server HSET h1 f2 1} {db0_HASH:2=1}
            run_cmd_verify_hist {$server HPEXPIREAT h1 1 FIELDS 1 f1} {db0_HASH:1=1}            
            run_cmd_verify_hist {$server HSET h1 f3 1} {db0_HASH:2=1} 
            run_cmd_verify_hist {$server HPEXPIRE h1 1 FIELDS 1 f2} {db0_HASH:2=1}
            run_cmd_verify_hist {} {db0_HASH:1=1} 1
            run_cmd_verify_hist {$server HPEXPIRE h1 1 FIELDS 1 f3} {db0_HASH:1=1}
            run_cmd_verify_hist {} {} 1
        }
    }    
    
    test "KEYSIZES - Test STRING BITS $suffixRepl" {
        # BITOPS
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server SET b1 "x123456789"} {db0_STR:8=1}
        run_cmd_verify_hist {$server SET b2 "x12345678"} {db0_STR:8=2}
        run_cmd_verify_hist {$server BITOP AND b3 b1 b2} {db0_STR:8=3}
        run_cmd_verify_hist {$server BITOP OR b4 b1 b2} {db0_STR:8=4}
        run_cmd_verify_hist {$server BITOP XOR b5 b1 b2} {db0_STR:8=5}        
        # SETBIT
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server setbit b1 71 1} {db0_STR:8=1}
        run_cmd_verify_hist {$server setbit b1 72 1} {db0_STR:8=1}
        run_cmd_verify_hist {$server setbit b2 72 1} {db0_STR:8=2}
        run_cmd_verify_hist {$server setbit b2 640 0} {db0_STR:8=1,64=1}
        # BITFIELD
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server bitfield b3 set u8 6 255} {db0_STR:2=1}
        run_cmd_verify_hist {$server bitfield b3 set u8 65 255} {db0_STR:8=1}
        run_cmd_verify_hist {$server bitfield b4 set u8 1000 255} {db0_STR:8=1,64=1}
    } {} {cluster:skip}
       
    test "KEYSIZES - Test RESTORE $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l10 1 2 3 4} {db0_LIST:4=1}
        set encoded [$server dump l10]
        run_cmd_verify_hist {$server del l10} {}         
        run_cmd_verify_hist {$server restore l11 0 $encoded} {db0_LIST:4=1}
    }    
    
    test "KEYSIZES - Test RENAME $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l12 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server RENAME l12 l13} {db0_LIST:4=1}
    } {} {cluster:skip}
    
    test "KEYSIZES - Test MOVE $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        run_cmd_verify_hist {$server RPUSH l1 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server RPUSH l2 1} {db0_LIST:1=1,4=1}
        run_cmd_verify_hist {$server MOVE l1 1} {db0_LIST:1=1 db1_LIST:4=1}
    } {} {cluster:skip}
    
    test "KEYSIZES - Test SWAPDB $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}        
        run_cmd_verify_hist {$server RPUSH l1 1 2 3 4} {db0_LIST:4=1}
        $server select 1
        run_cmd_verify_hist {$server ZADD z1 1 A} {db0_LIST:4=1   db1_ZSET:1=1}
        run_cmd_verify_hist {$server SWAPDB 0 1}  {db0_ZSET:1=1   db1_LIST:4=1}
        $server select 0    
    } {OK} {singledb:skip}
    
    test "KEYSIZES - Test RDB $suffixRepl" {
        run_cmd_verify_hist {$server FLUSHALL} {}
        # Write list, set and zset to db0 
        run_cmd_verify_hist {$server RPUSH l1 1 2 3 4} {db0_LIST:4=1}
        run_cmd_verify_hist {$server SADD s1 1 2 3 4 5} {db0_LIST:4=1 db0_SET:4=1}
        run_cmd_verify_hist {$server ZADD z1 1 a 2 b 3 c} {db0_LIST:4=1 db0_SET:4=1 db0_ZSET:2=1}
        run_cmd_verify_hist {$server SAVE} {db0_LIST:4=1 db0_SET:4=1 db0_ZSET:2=1}
        if {$replicaMode eq 1} {        
            run_cmd_verify_hist {restart_server -1 true false} {db0_LIST:4=1 db0_SET:4=1 db0_ZSET:2=1}
        } else {
            run_cmd_verify_hist {restart_server 0 true false} {db0_LIST:4=1 db0_SET:4=1 db0_ZSET:2=1}
        }
    } {} {external:skip}
}

start_server {} {
    r select 0
    test_all_keysizes 0

    # Start another server to test replication of KEYSIZES
    start_server {tags {needs:repl external:skip}} {
        # Set the outer layer server as primary
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        # Set this inner layer server as replica
        set replica [srv 0 client]

        # Server should have role replica
        $replica replicaof $primary_host $primary_port
        wait_for_condition 50 100 { [s 0 role] eq {slave} } else { fail "Replication not started." }

        $primary select 0
        test_all_keysizes 1
    }
}
