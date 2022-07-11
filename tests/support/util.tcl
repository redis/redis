proc randstring {min max {type binary}} {
    set len [expr {$min+int(rand()*($max-$min+1))}]
    set output {}
    if {$type eq {binary}} {
        set minval 0
        set maxval 255
    } elseif {$type eq {alpha} || $type eq {simplealpha}} {
        set minval 48
        set maxval 122
    } elseif {$type eq {compr}} {
        set minval 48
        set maxval 52
    }
    while {$len} {
        set num [expr {$minval+int(rand()*($maxval-$minval+1))}]
        set rr [format "%c" $num]
        if {$type eq {simplealpha} && ![string is alnum $rr]} {continue}
        if {$type eq {alpha} && $num eq 92} {continue} ;# avoid putting '\' char in the string, it can mess up TCL processing
        append output $rr
        incr len -1
    }
    return $output
}

# Useful for some test
proc zlistAlikeSort {a b} {
    if {[lindex $a 0] > [lindex $b 0]} {return 1}
    if {[lindex $a 0] < [lindex $b 0]} {return -1}
    string compare [lindex $a 1] [lindex $b 1]
}

# Return all log lines starting with the first line that contains a warning.
# Generally, this will be an assertion error with a stack trace.
proc crashlog_from_file {filename} {
    set lines [split [exec cat $filename] "\n"]
    set matched 0
    set logall 0
    set result {}
    foreach line $lines {
        if {[string match {*REDIS BUG REPORT START*} $line]} {
            set logall 1
        }
        if {[regexp {^\[\d+\]\s+\d+\s+\w+\s+\d{2}:\d{2}:\d{2} \#} $line]} {
            set matched 1
        }
        if {$logall || $matched} {
            lappend result $line
        }
    }
    join $result "\n"
}

# Return sanitizer log lines
proc sanitizer_errors_from_file {filename} {
    set log [exec cat $filename]
    set lines [split [exec cat $filename] "\n"]

    foreach line $lines {
        # Ignore huge allocation warnings
        if ([string match {*WARNING: AddressSanitizer failed to allocate*} $line]) {
            continue
        }

        # GCC UBSAN output does not contain 'Sanitizer' but 'runtime error'.
        if {[string match {*runtime error*} $log] ||
            [string match {*Sanitizer*} $log]} {
            return $log
        }
    }

    return ""
}

proc getInfoProperty {infostr property} {
    if {[regexp -lineanchor "^$property:(.*?)\r\n" $infostr _ value]} {
        return $value
    }
}

proc cluster_info {r field} {
    if {[regexp "^$field:(.*?)\r\n" [$r cluster info] _ value]} {
        set _ $value
    }
}

# Return value for INFO property
proc status {r property} {
    set _ [getInfoProperty [{*}$r info] $property]
}

proc waitForBgsave r {
    while 1 {
        if {[status $r rdb_bgsave_in_progress] eq 1} {
            if {$::verbose} {
                puts -nonewline "\nWaiting for background save to finish... "
                flush stdout
            }
            after 50
        } else {
            break
        }
    }
}

proc waitForBgrewriteaof r {
    while 1 {
        if {[status $r aof_rewrite_in_progress] eq 1} {
            if {$::verbose} {
                puts -nonewline "\nWaiting for background AOF rewrite to finish... "
                flush stdout
            }
            after 50
        } else {
            break
        }
    }
}

proc wait_for_sync r {
    wait_for_condition 50 100 {
        [status $r master_link_status] eq "up"
    } else {
        fail "replica didn't sync in time"
    }
}

proc wait_replica_online r {
    wait_for_condition 50 100 {
        [string match "*slave0:*,state=online*" [$r info replication]]
    } else {
        fail "replica didn't online in time"
    }
}

proc wait_for_ofs_sync {r1 r2} {
    wait_for_condition 50 100 {
        [status $r1 master_repl_offset] eq [status $r2 master_repl_offset]
    } else {
        fail "replica offset didn't match in time"
    }
}

proc wait_done_loading r {
    wait_for_condition 50 100 {
        [catch {$r ping} e] == 0
    } else {
        fail "Loading DB is taking too much time."
    }
}

proc wait_lazyfree_done r {
    wait_for_condition 50 100 {
        [status $r lazyfree_pending_objects] == 0
    } else {
        fail "lazyfree isn't done"
    }
}

# count current log lines in server's stdout
proc count_log_lines {srv_idx} {
    set _ [string trim [exec wc -l < [srv $srv_idx stdout]]]
}

# returns the number of times a line with that pattern appears in a file
proc count_message_lines {file pattern} {
    set res 0
    # exec fails when grep exists with status other than 0 (when the patter wasn't found)
    catch {
        set res [string trim [exec grep $pattern $file 2> /dev/null | wc -l]]
    }
    return $res
}

# returns the number of times a line with that pattern appears in the log
proc count_log_message {srv_idx pattern} {
    set stdout [srv $srv_idx stdout]
    return [count_message_lines $stdout $pattern]
}

# verify pattern exists in server's sdtout after a certain line number
proc verify_log_message {srv_idx pattern from_line} {
    incr from_line
    set result [exec tail -n +$from_line < [srv $srv_idx stdout]]
    if {![string match $pattern $result]} {
        error "assertion:expected message not found in log file: $pattern"
    }
}

# wait for pattern to be found in server's stdout after certain line number
# return value is a list containing the line that matched the pattern and the line number
proc wait_for_log_messages {srv_idx patterns from_line maxtries delay} {
    set retry $maxtries
    set next_line [expr $from_line + 1] ;# searching form the line after
    set stdout [srv $srv_idx stdout]
    while {$retry} {
        # re-read the last line (unless it's before to our first), last time we read it, it might have been incomplete
        set next_line [expr $next_line - 1 > $from_line + 1 ? $next_line - 1 : $from_line + 1]
        set result [exec tail -n +$next_line < $stdout]
        set result [split $result "\n"]
        foreach line $result {
            foreach pattern $patterns {
                if {[string match $pattern $line]} {
                    return [list $line $next_line]
                }
            }
            incr next_line
        }
        incr retry -1
        after $delay
    }
    if {$retry == 0} {
        if {$::verbose} {
            puts "content of $stdout from line: $from_line:"
            puts [exec tail -n +$from_line < $stdout]
        }
        fail "log message of '$patterns' not found in $stdout after line: $from_line till line: [expr $next_line -1]"
    }
}

# write line to server log file
proc write_log_line {srv_idx msg} {
    set logfile [srv $srv_idx stdout]
    set fd [open $logfile "a+"]
    puts $fd "### $msg"
    close $fd
}

# Random integer between 0 and max (excluded).
proc randomInt {max} {
    expr {int(rand()*$max)}
}

# Random integer between min and max (excluded).
proc randomRange {min max} {
    expr {int(rand()*[expr $max - $min]) + $min}
}

# Random signed integer between -max and max (both extremes excluded).
proc randomSignedInt {max} {
    set i [randomInt $max]
    if {rand() > 0.5} {
        set i -$i
    }
    return $i
}

proc randpath args {
    set path [expr {int(rand()*[llength $args])}]
    uplevel 1 [lindex $args $path]
}

proc randomValue {} {
    randpath {
        # Small enough to likely collide
        randomSignedInt 1000
    } {
        # 32 bit compressible signed/unsigned
        randpath {randomSignedInt 2000000000} {randomSignedInt 4000000000}
    } {
        # 64 bit
        randpath {randomSignedInt 1000000000000}
    } {
        # Random string
        randpath {randstring 0 256 alpha} \
                {randstring 0 256 compr} \
                {randstring 0 256 binary}
    }
}

proc randomKey {} {
    randpath {
        # Small enough to likely collide
        randomInt 1000
    } {
        # 32 bit compressible signed/unsigned
        randpath {randomInt 2000000000} {randomInt 4000000000}
    } {
        # 64 bit
        randpath {randomInt 1000000000000}
    } {
        # Random string
        randpath {randstring 1 256 alpha} \
                {randstring 1 256 compr}
    }
}

proc findKeyWithType {r type} {
    for {set j 0} {$j < 20} {incr j} {
        set k [{*}$r randomkey]
        if {$k eq {}} {
            return {}
        }
        if {[{*}$r type $k] eq $type} {
            return $k
        }
    }
    return {}
}

proc createComplexDataset {r ops {opt {}}} {
    set useexpire [expr {[lsearch -exact $opt useexpire] != -1}]
    if {[lsearch -exact $opt usetag] != -1} {
        set tag "{t}"
    } else {
        set tag ""
    }
    for {set j 0} {$j < $ops} {incr j} {
        set k [randomKey]$tag
        set k2 [randomKey]$tag
        set f [randomValue]
        set v [randomValue]

        if {$useexpire} {
            if {rand() < 0.1} {
                {*}$r expire [randomKey] [randomInt 2]
            }
        }

        randpath {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            randpath {set d +inf} {set d -inf}
        }
        set t [{*}$r type $k]

        if {$t eq {none}} {
            randpath {
                {*}$r set $k $v
            } {
                {*}$r lpush $k $v
            } {
                {*}$r sadd $k $v
            } {
                {*}$r zadd $k $d $v
            } {
                {*}$r hset $k $f $v
            } {
                {*}$r del $k
            }
            set t [{*}$r type $k]
        }

        switch $t {
            {string} {
                # Nothing to do
            }
            {list} {
                randpath {{*}$r lpush $k $v} \
                        {{*}$r rpush $k $v} \
                        {{*}$r lrem $k 0 $v} \
                        {{*}$r rpop $k} \
                        {{*}$r lpop $k}
            }
            {set} {
                randpath {{*}$r sadd $k $v} \
                        {{*}$r srem $k $v} \
                        {
                            set otherset [findKeyWithType {*}$r set]
                            if {$otherset ne {}} {
                                randpath {
                                    {*}$r sunionstore $k2 $k $otherset
                                } {
                                    {*}$r sinterstore $k2 $k $otherset
                                } {
                                    {*}$r sdiffstore $k2 $k $otherset
                                }
                            }
                        }
            }
            {zset} {
                randpath {{*}$r zadd $k $d $v} \
                        {{*}$r zrem $k $v} \
                        {
                            set otherzset [findKeyWithType {*}$r zset]
                            if {$otherzset ne {}} {
                                randpath {
                                    {*}$r zunionstore $k2 2 $k $otherzset
                                } {
                                    {*}$r zinterstore $k2 2 $k $otherzset
                                }
                            }
                        }
            }
            {hash} {
                randpath {{*}$r hset $k $f $v} \
                        {{*}$r hdel $k $f}
            }
        }
    }
}

proc formatCommand {args} {
    set cmd "*[llength $args]\r\n"
    foreach a $args {
        append cmd "$[string length $a]\r\n$a\r\n"
    }
    set _ $cmd
}

proc csvdump r {
    set o {}
    if {$::singledb} {
        set maxdb 1
    } else {
        set maxdb 16
    }
    for {set db 0} {$db < $maxdb} {incr db} {
        if {!$::singledb} {
            {*}$r select $db
        }
        foreach k [lsort [{*}$r keys *]] {
            set type [{*}$r type $k]
            append o [csvstring $db] , [csvstring $k] , [csvstring $type] ,
            switch $type {
                string {
                    append o [csvstring [{*}$r get $k]] "\n"
                }
                list {
                    foreach e [{*}$r lrange $k 0 -1] {
                        append o [csvstring $e] ,
                    }
                    append o "\n"
                }
                set {
                    foreach e [lsort [{*}$r smembers $k]] {
                        append o [csvstring $e] ,
                    }
                    append o "\n"
                }
                zset {
                    foreach e [{*}$r zrange $k 0 -1 withscores] {
                        append o [csvstring $e] ,
                    }
                    append o "\n"
                }
                hash {
                    set fields [{*}$r hgetall $k]
                    set newfields {}
                    foreach {k v} $fields {
                        lappend newfields [list $k $v]
                    }
                    set fields [lsort -index 0 $newfields]
                    foreach kv $fields {
                        append o [csvstring [lindex $kv 0]] ,
                        append o [csvstring [lindex $kv 1]] ,
                    }
                    append o "\n"
                }
            }
        }
    }
    if {!$::singledb} {
        {*}$r select 9
    }
    return $o
}

proc csvstring s {
    return "\"$s\""
}

proc roundFloat f {
    format "%.10g" $f
}

set ::last_port_attempted 0
proc find_available_port {start count} {
    set port [expr $::last_port_attempted + 1]
    for {set attempts 0} {$attempts < $count} {incr attempts} {
        if {$port < $start || $port >= $start+$count} {
            set port $start
        }
        set fd1 -1
        if {[catch {set fd1 [socket -server 127.0.0.1 $port]}] ||
            [catch {set fd2 [socket -server 127.0.0.1 [expr $port+10000]]}]} {
            if {$fd1 != -1} {
                close $fd1
            }
        } else {
            close $fd1
            close $fd2
            set ::last_port_attempted $port
            return $port
        }
        incr port
    }
    error "Can't find a non busy port in the $start-[expr {$start+$count-1}] range."
}

# Test if TERM looks like to support colors
proc color_term {} {
    expr {[info exists ::env(TERM)] && [string match *xterm* $::env(TERM)]}
}

proc colorstr {color str} {
    if {[color_term]} {
        set b 0
        if {[string range $color 0 4] eq {bold-}} {
            set b 1
            set color [string range $color 5 end]
        }
        switch $color {
            red {set colorcode {31}}
            green {set colorcode {32}}
            yellow {set colorcode {33}}
            blue {set colorcode {34}}
            magenta {set colorcode {35}}
            cyan {set colorcode {36}}
            white {set colorcode {37}}
            default {set colorcode {37}}
        }
        if {$colorcode ne {}} {
            return "\033\[$b;${colorcode};49m$str\033\[0m"
        }
    } else {
        return $str
    }
}

proc find_valgrind_errors {stderr on_termination} {
    set fd [open $stderr]
    set buf [read $fd]
    close $fd

    # Look for stack trace (" at 0x") and other errors (Invalid, Mismatched, etc).
    # Look for "Warnings", but not the "set address range perms". These don't indicate any real concern.
    # corrupt-dump unit, not sure why but it seems they don't indicate any real concern.
    if {[regexp -- { at 0x} $buf] ||
        [regexp -- {^(?=.*Warning)(?:(?!set address range perms).)*$} $buf] ||
        [regexp -- {Invalid} $buf] ||
        [regexp -- {Mismatched} $buf] ||
        [regexp -- {uninitialized} $buf] ||
        [regexp -- {has a fishy} $buf] ||
        [regexp -- {overlap} $buf]} {
        return $buf
    }

    # If the process didn't terminate yet, we can't look for the summary report
    if {!$on_termination} {
        return ""
    }

    # Look for the absence of a leak free summary (happens when redis isn't terminated properly).
    if {(![regexp -- {definitely lost: 0 bytes} $buf] &&
         ![regexp -- {no leaks are possible} $buf])} {
        return $buf
    }

    return ""
}

# Execute a background process writing random data for the specified number
# of seconds to the specified Redis instance.
proc start_write_load {host port seconds} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/helpers/gen_write_load.tcl $host $port $seconds $::tls &
}

# Stop a process generating write load executed with start_write_load.
proc stop_write_load {handle} {
    catch {exec /bin/kill -9 $handle}
}

proc wait_load_handlers_disconnected {{level 0}} {
    wait_for_condition 50 100 {
        ![string match {*name=LOAD_HANDLER*} [r $level client list]]
    } else {
        fail "load_handler(s) still connected after too long time."
    }
}

proc K { x y } { set x } 

# Shuffle a list with Fisher-Yates algorithm.
proc lshuffle {list} {
    set n [llength $list]
    while {$n>1} {
        set j [expr {int(rand()*$n)}]
        incr n -1
        if {$n==$j} continue
        set v [lindex $list $j]
        lset list $j [lindex $list $n]
        lset list $n $v
    }
    return $list
}

# Execute a background process writing complex data for the specified number
# of ops to the specified Redis instance.
proc start_bg_complex_data {host port db ops} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/helpers/bg_complex_data.tcl $host $port $db $ops $::tls &
}

# Stop a process generating write load executed with start_bg_complex_data.
proc stop_bg_complex_data {handle} {
    catch {exec /bin/kill -9 $handle}
}

# Write num keys with the given key prefix and value size (in bytes). If idx is
# given, it's the index (AKA level) used with the srv procedure and it specifies
# to which Redis instance to write the keys.
proc populate {num {prefix key:} {size 3} {idx 0}} {
    set rd [redis_deferring_client $idx]
    for {set j 0} {$j < $num} {incr j} {
        $rd set $prefix$j [string repeat A $size]
    }
    for {set j 0} {$j < $num} {incr j} {
        $rd read
    }
    $rd close
}

proc get_child_pid {idx} {
    set pid [srv $idx pid]
    if {[file exists "/usr/bin/pgrep"]} {
        set fd [open "|pgrep -P $pid" "r"]
        set child_pid [string trim [lindex [split [read $fd] \n] 0]]
    } else {
        set fd [open "|ps --ppid $pid -o pid" "r"]
        set child_pid [string trim [lindex [split [read $fd] \n] 1]]
    }
    close $fd

    return $child_pid
}

proc cmdrstat {cmd r} {
    if {[regexp "\r\ncmdstat_$cmd:(.*?)\r\n" [$r info commandstats] _ value]} {
        set _ $value
    }
}

proc errorrstat {cmd r} {
    if {[regexp "\r\nerrorstat_$cmd:(.*?)\r\n" [$r info errorstats] _ value]} {
        set _ $value
    }
}

proc latencyrstat_percentiles {cmd r} {
    if {[regexp "\r\nlatency_percentiles_usec_$cmd:(.*?)\r\n" [$r info latencystats] _ value]} {
        set _ $value
    }
}

proc generate_fuzzy_traffic_on_key {key duration} {
    # Commands per type, blocking commands removed
    # TODO: extract these from COMMAND DOCS, and improve to include other types
    set string_commands {APPEND BITCOUNT BITFIELD BITOP BITPOS DECR DECRBY GET GETBIT GETRANGE GETSET INCR INCRBY INCRBYFLOAT MGET MSET MSETNX PSETEX SET SETBIT SETEX SETNX SETRANGE LCS STRLEN}
    set hash_commands {HDEL HEXISTS HGET HGETALL HINCRBY HINCRBYFLOAT HKEYS HLEN HMGET HMSET HSCAN HSET HSETNX HSTRLEN HVALS HRANDFIELD}
    set zset_commands {ZADD ZCARD ZCOUNT ZINCRBY ZINTERSTORE ZLEXCOUNT ZPOPMAX ZPOPMIN ZRANGE ZRANGEBYLEX ZRANGEBYSCORE ZRANK ZREM ZREMRANGEBYLEX ZREMRANGEBYRANK ZREMRANGEBYSCORE ZREVRANGE ZREVRANGEBYLEX ZREVRANGEBYSCORE ZREVRANK ZSCAN ZSCORE ZUNIONSTORE ZRANDMEMBER}
    set list_commands {LINDEX LINSERT LLEN LPOP LPOS LPUSH LPUSHX LRANGE LREM LSET LTRIM RPOP RPOPLPUSH RPUSH RPUSHX}
    set set_commands {SADD SCARD SDIFF SDIFFSTORE SINTER SINTERSTORE SISMEMBER SMEMBERS SMOVE SPOP SRANDMEMBER SREM SSCAN SUNION SUNIONSTORE}
    set stream_commands {XACK XADD XCLAIM XDEL XGROUP XINFO XLEN XPENDING XRANGE XREAD XREADGROUP XREVRANGE XTRIM}
    set commands [dict create string $string_commands hash $hash_commands zset $zset_commands list $list_commands set $set_commands stream $stream_commands]

    set type [r type $key]
    set cmds [dict get $commands $type]
    set start_time [clock seconds]
    set sent {}
    set succeeded 0
    while {([clock seconds]-$start_time) < $duration} {
        # find a random command for our key type
        set cmd_idx [expr {int(rand()*[llength $cmds])}]
        set cmd [lindex $cmds $cmd_idx]
        # get the command details from redis
        if { [ catch {
            set cmd_info [lindex [r command info $cmd] 0]
        } err ] } {
            # if we failed, it means redis crashed after the previous command
            return $sent
        }
        # try to build a valid command argument
        set arity [lindex $cmd_info 1]
        set arity [expr $arity < 0 ? - $arity: $arity]
        set firstkey [lindex $cmd_info 3]
        set lastkey [lindex $cmd_info 4]
        set i 1
        if {$cmd == "XINFO"} {
            lappend cmd "STREAM"
            lappend cmd $key
            lappend cmd "FULL"
            incr i 3
        }
        if {$cmd == "XREAD"} {
            lappend cmd "STREAMS"
            lappend cmd $key
            randpath {
                lappend cmd \$
            } {
                lappend cmd [randomValue]
            }
            incr i 3
        }
        if {$cmd == "XADD"} {
            lappend cmd $key
            randpath {
                lappend cmd "*"
            } {
                lappend cmd [randomValue]
            }
            lappend cmd [randomValue]
            lappend cmd [randomValue]
            incr i 4
        }
        for {} {$i < $arity} {incr i} {
            if {$i == $firstkey || $i == $lastkey} {
                lappend cmd $key
            } else {
                lappend cmd [randomValue]
            }
        }
        # execute the command, we expect commands to fail on syntax errors
        lappend sent $cmd
        if { ! [ catch {
            r {*}$cmd
        } err ] } {
            incr succeeded
        } else {
            set err [format "%s" $err] ;# convert to string for pattern matching
            if {[string match "*SIGTERM*" $err]} {
                puts "command caused test to hang? $cmd"
                exit 1
            }
        }
    }

    # print stats so that we know if we managed to generate commands that actually made senes
    #if {$::verbose} {
    #    set count [llength $sent]
    #    puts "Fuzzy traffic sent: $count, succeeded: $succeeded"
    #}

    # return the list of commands we sent
    return $sent
}

proc string2printable s {
    set res {}
    set has_special_chars false
    foreach i [split $s {}] {
        scan $i %c int
        # non printable characters, including space and excluding: " \ $ { }
        if {$int < 32 || $int > 122 || $int == 34 || $int == 36 || $int == 92} {
            set has_special_chars true
        }
        # TCL8.5 has issues mixing \x notation and normal chars in the same
        # source code string, so we'll convert the entire string.
        append res \\x[format %02X $int]
    }
    if {!$has_special_chars} {
        return $s
    }
    set res "\"$res\""
    return $res
}

# Calculation value of Chi-Square Distribution. By this value
# we can verify the random distribution sample confidence.
# Based on the following wiki:
# https://en.wikipedia.org/wiki/Chi-square_distribution
#
# param res    Random sample list
# return       Value of Chi-Square Distribution
#
# x2_value: return of chi_square_value function
# df: Degrees of freedom, Number of independent values minus 1
#
# By using x2_value and df to back check the cardinality table,
# we can know the confidence of the random sample.
proc chi_square_value {res} {
    unset -nocomplain mydict
    foreach key $res {
        dict incr mydict $key 1
    }

    set x2_value 0
    set p [expr [llength $res] / [dict size $mydict]]
    foreach key [dict keys $mydict] {
        set value [dict get $mydict $key]

        # Aggregate the chi-square value of each element
        set v [expr {pow($value - $p, 2) / $p}]
        set x2_value [expr {$x2_value + $v}]
    }

    return $x2_value
}

#subscribe to Pub/Sub channels
proc consume_subscribe_messages {client type channels} {
    set numsub -1
    set counts {}

    for {set i [llength $channels]} {$i > 0} {incr i -1} {
        set msg [$client read]
        assert_equal $type [lindex $msg 0]

        # when receiving subscribe messages the channels names
        # are ordered. when receiving unsubscribe messages
        # they are unordered
        set idx [lsearch -exact $channels [lindex $msg 1]]
        if {[string match "*unsubscribe" $type]} {
            assert {$idx >= 0}
        } else {
            assert {$idx == 0}
        }
        set channels [lreplace $channels $idx $idx]

        # aggregate the subscription count to return to the caller
        lappend counts [lindex $msg 2]
    }

    # we should have received messages for channels
    assert {[llength $channels] == 0}
    return $counts
}

proc subscribe {client channels} {
    $client subscribe {*}$channels
    consume_subscribe_messages $client subscribe $channels
}

proc ssubscribe {client channels} {
    $client ssubscribe {*}$channels
    consume_subscribe_messages $client ssubscribe $channels
}

proc unsubscribe {client {channels {}}} {
    $client unsubscribe {*}$channels
    consume_subscribe_messages $client unsubscribe $channels
}

proc sunsubscribe {client {channels {}}} {
    $client sunsubscribe {*}$channels
    consume_subscribe_messages $client sunsubscribe $channels
}

proc psubscribe {client channels} {
    $client psubscribe {*}$channels
    consume_subscribe_messages $client psubscribe $channels
}

proc punsubscribe {client {channels {}}} {
    $client punsubscribe {*}$channels
    consume_subscribe_messages $client punsubscribe $channels
}

proc debug_digest_value {key} {
    if {[lsearch $::denytags "needs:debug"] >= 0 || $::ignoredigest} {
        return "dummy-digest-value"
    }
    r debug digest-value $key
}

proc debug_digest {{level 0}} {
    if {[lsearch $::denytags "needs:debug"] >= 0 || $::ignoredigest} {
        return "dummy-digest"
    }
    r $level debug digest
}

proc wait_for_blocked_client {} {
    wait_for_condition 50 100 {
        [s blocked_clients] ne 0
    } else {
        fail "no blocked clients"
    }
}

proc wait_for_blocked_clients_count {count {maxtries 100} {delay 10}} {
    wait_for_condition $maxtries $delay  {
        [s blocked_clients] == $count
    } else {
        fail "Timeout waiting for blocked clients"
    }
}

proc read_from_aof {fp} {
    # Input fp is a blocking binary file descriptor of an opened AOF file.
    if {[gets $fp count] == -1} return ""
    set count [string range $count 1 end]

    # Return a list of arguments for the command.
    set res {}
    for {set j 0} {$j < $count} {incr j} {
        read $fp 1
        set arg [::redis::redis_bulk_read $fp]
        if {$j == 0} {set arg [string tolower $arg]}
        lappend res $arg
    }
    return $res
}

proc assert_aof_content {aof_path patterns} {
    set fp [open $aof_path r]
    fconfigure $fp -translation binary
    fconfigure $fp -blocking 1

    for {set j 0} {$j < [llength $patterns]} {incr j} {
        assert_match [lindex $patterns $j] [read_from_aof $fp]
    }
}

proc config_set {param value {options {}}} {
    set mayfail 0
    foreach option $options {
        switch $option {
            "mayfail" {
                set mayfail 1
            }
            default {
                error "Unknown option $option"
            }
        }
    }

    if {[catch {r config set $param $value} err]} {
        if {!$mayfail} {
            error $err
        } else {
            if {$::verbose} {
                puts "Ignoring CONFIG SET $param $value failure: $err"
            }
        }
    }
}

proc delete_lines_with_pattern {filename tmpfilename pattern} {
    set fh_in [open $filename r]
    set fh_out [open $tmpfilename w]
    while {[gets $fh_in line] != -1} {
        if {![regexp $pattern $line]} {
            puts $fh_out $line
        }
    }
    close $fh_in
    close $fh_out
    file rename -force $tmpfilename $filename
}

proc get_nonloopback_addr {} {
    set addrlist [list {}]
    catch { set addrlist [exec hostname -I] }
    return [lindex $addrlist 0]
}

proc get_nonloopback_client {} {
    return [redis [get_nonloopback_addr] [srv 0 "port"] 0 $::tls]
}

# The following functions and variables are used only when running large-memory
# tests. We avoid defining them when not running large-memory tests because the 
# global variables takes up lots of memory.
proc init_large_mem_vars {} {
    if {![info exists ::str500]} {
        set ::str500 [string repeat x 500000000] ;# 500mb
        set ::str500_len [string length $::str500]
    }
}

# Utility function to write big argument into redis client connection
proc write_big_bulk {size {prefix ""} {skip_read no}} {
    init_large_mem_vars

    assert {[string length prefix] <= $size}
    r write "\$$size\r\n"
    r write $prefix
    incr size -[string length $prefix]
    while {$size >= 500000000} {
        r write $::str500
        incr size -500000000
    }
    if {$size > 0} {
        r write [string repeat x $size]
    }
    r write "\r\n"
    if {!$skip_read} {
        r flush
        r read
    }
}

# Utility to read big bulk response (work around Tcl limitations)
proc read_big_bulk {code {compare no} {prefix ""}} {
    init_large_mem_vars

    r readraw 1
    set resp_len [uplevel 1 $code] ;# get the first line of the RESP response
    assert_equal [string range $resp_len 0 0] "$"
    set resp_len [string range $resp_len 1 end]
    set prefix_len [string length $prefix]
    if {$compare} {
        assert {$prefix_len <= $resp_len}
        assert {$prefix_len <= $::str500_len}
    }

    set remaining $resp_len
    while {$remaining > 0} {
        set l $remaining
        if {$l > $::str500_len} {set l $::str500_len} ; # can't read more than 2gb at a time, so read 500mb so we can easily verify read data
        set read_data [r rawread $l]
        set nbytes [string length $read_data]
        if {$compare} {
            set comp_len $nbytes
            # Compare prefix part
            if {$remaining == $resp_len} {
                assert_equal $prefix [string range $read_data 0 [expr $prefix_len - 1]]
                set read_data [string range $read_data $prefix_len $nbytes]
                incr comp_len -$prefix_len
            }
            # Compare rest of data, evaluate and then assert to avoid huge print in case of failure
            set data_equal [expr {$read_data == [string range $::str500 0 [expr $comp_len - 1]]}]
            assert $data_equal
        }
        incr remaining -$nbytes
    }
    assert_equal [r rawread 2] "\r\n"
    r readraw 0
    return $resp_len
}

proc prepare_value {size} {
    set _v "c"
    for {set i 1} {$i < $size} {incr i} {
        append _v 0
    }
    return $_v
}

proc memory_usage {key} {
    set usage [r memory usage $key]
    if {![string match {*jemalloc*} [s mem_allocator]]} {
        # libc allocator can sometimes return a different size allocation for the same requested size
        # this makes tests that rely on MEMORY USAGE unreliable, so instead we return a constant 1
        set usage 1
    }
    return $usage
}
