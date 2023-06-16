proc randstring {min max {type binary}} {
    set len [expr {$min+int(rand()*($max-$min+1))}]
    set output {}
    if {$type eq {binary}} {
        set minval 0
        set maxval 255
    } elseif {$type eq {alpha}} {
        set minval 48
        set maxval 122
    } elseif {$type eq {compr}} {
        set minval 48
        set maxval 52
    }
    while {$len} {
        append output [format "%c" [expr {$minval+int(rand()*($maxval-$minval+1))}]]
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
proc warnings_from_file {filename} {
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

# Return value for INFO property
proc status {r property} {
    if {[regexp "\r\n$property:(.*?)\r\n" [{*}$r info] _ value]} {
        set _ $value
    }
}

proc waitForBgsave r {
    while 1 {
        if {[status r rdb_bgsave_in_progress] eq 1} {
            if {$::verbose} {
                puts -nonewline "\nWaiting for background save to finish... "
                flush stdout
            }
            after 1000
        } else {
            break
        }
    }
}

proc waitForBgrewriteaof r {
    while 1 {
        if {[status r aof_rewrite_in_progress] eq 1} {
            if {$::verbose} {
                puts -nonewline "\nWaiting for background AOF rewrite to finish... "
                flush stdout
            }
            after 1000
        } else {
            break
        }
    }
}

proc wait_for_sync r {
    while 1 {
        if {[status $r master_link_status] eq "down"} {
            after 10
        } else {
            break
        }
    }
}

proc wait_for_ofs_sync {r1 r2} {
    wait_for_condition 50 100 {
        [status $r1 master_repl_offset] eq [status $r2 master_repl_offset]
    } else {
        fail "replica didn't sync in time"
    }
}

proc wait_done_loading r {
    wait_for_condition 50 100 {
        [catch {$r ping} e] == 0
    } else {
        fail "Loading DB is taking too much time."
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
    for {set j 0} {$j < $ops} {incr j} {
        set k [randomKey]
        set k2 [randomKey]
        set f [randomValue]
        set v [randomValue]

        if {[lsearch -exact $opt useexpire] != -1} {
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
    for {set db 0} {$db < 16} {incr db} {
        {*}$r select $db
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
    {*}$r select 9
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
        if {[catch {set fd1 [socket 127.0.0.1 $port]}] &&
            [catch {set fd2 [socket 127.0.0.1 [expr $port+10000]]}]} {
            set ::last_port_attempted $port
            return $port
        } else {
            catch {
                close $fd1
                close $fd2
            }
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

proc find_valgrind_errors {stderr} {
    set fd [open $stderr]
    set buf [read $fd]
    close $fd

    # Look for stack trace (" at 0x") and other errors (Invalid, Mismatched, etc).
    # Look for "Warnings", but not the "set address range perms". These don't indicate any real concern.
    # Look for the absense of a leak free summary (happens when redis isn't terminated properly).
    if {[regexp -- { at 0x} $buf] ||
        [regexp -- {^(?=.*Warning)(?:(?!set address range perms).)*$} $buf] ||
        [regexp -- {Invalid} $buf] ||
        [regexp -- {Mismatched} $buf] ||
        [regexp -- {uninitialized} $buf] ||
        [regexp -- {has a fishy} $buf] ||
        [regexp -- {overlap} $buf] ||
        (![regexp -- {definitely lost: 0 bytes} $buf] &&
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

proc K { x y } { set x } 

# Shuffle a list. From Tcl wiki. Originally from Steve Cohen that improved
# other versions. Code should be under public domain.
proc lshuffle {list} {
    set n [llength $list]
    while {$n>0} {
        set j [expr {int(rand()*$n)}]
        lappend slist [lindex $list $j]
        incr n -1
        set temp [lindex $list $n]
        set list [lreplace [K $list [set list {}]] $j $j $temp]
    }
    return $slist
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

proc populate {num prefix size} {
    set rd [redis_deferring_client]
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

proc config_get_set {param value {options {}}} {
    set config [lindex [r config get $param] 1]
    config_set $param $value $options
    return $config
}

