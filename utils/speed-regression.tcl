#!/usr/bin/env tclsh8.5
# Copyright (C) 2011 Salvatore Sanfilippo
# Released under the BSD license like Redis itself

source ../tests/support/redis.tcl
set ::port 12123
set ::tests {PING,SET,GET,INCR,LPUSH,LPOP,SADD,SPOP,LRANGE_100,LRANGE_600,MSET}
set ::datasize 16
set ::requests 100000

proc run-tests branches {
    set runs {}
    set branch_id 0
    foreach b $branches {
        cd ../src
        puts "Benchmarking $b"
        exec -ignorestderr git checkout $b 2> /dev/null
        exec -ignorestderr make clean 2> /dev/null
        puts "  compiling..."
        exec -ignorestderr make 2> /dev/null

        if {$branch_id == 0} {
            puts "  copy redis-benchmark from unstable to /tmp..."
            exec -ignorestderr cp ./redis-benchmark /tmp
            incr branch_id
            continue
        }

        # Start the Redis server
        puts "  starting the server... [exec ./redis-server -v]"
        set pids [exec echo "port $::port\nloglevel warning\n" | ./redis-server - > /dev/null 2> /dev/null &]
        puts "  pids: $pids"
        after 1000
        puts "  running the benchmark"

        set r [redis 127.0.0.1 $::port]
        set i [$r info]
        puts "  redis INFO shows version: [lindex [split $i] 0]"
        $r close

        set output [exec /tmp/redis-benchmark -n $::requests -t $::tests -d $::datasize --csv -p $::port]
        lappend runs $b $output
        puts "  killing server..."
        catch {exec kill -9 [lindex $pids 0]}
        catch {exec kill -9 [lindex $pids 1]}
        incr branch_id
    }
    return $runs
}

proc get-result-with-name {output name} {
    foreach line [split $output "\n"] {
        lassign [split $line ","] key value
        set key [string tolower [string range $key 1 end-1]]
        set value [string range $value 1 end-1]
        if {$key eq [string tolower $name]} {
            return $value
        }
    }
    return "n/a"
}

proc get-test-names output {
    set names {}
    foreach line [split $output "\n"] {
        lassign [split $line ","] key value
        set key [string tolower [string range $key 1 end-1]]
        lappend names $key
    }
    return $names
}

proc combine-results {results} {
    set tests [get-test-names [lindex $results 1]]
    foreach test $tests {
        puts $test
        foreach {branch output} $results {
            puts [format "%-20s %s" \
                $branch [get-result-with-name $output $test]]
        }
        puts {}
    }
}

proc main {} {
    # Note: the first branch is only used in order to get the redis-benchmark
    # executable. Tests are performed starting from the second branch.
    set branches {
        slowset 2.2.0 2.4.0 unstable slowset
    }
    set results [run-tests $branches]
    puts "\n"
    puts "# Test results: datasize=$::datasize requests=$::requests"
    puts [combine-results $results]
}

# Force the user to run the script from the 'utils' directory.
if {![file exists speed-regression.tcl]} {
    puts "Please make sure to run speed-regression.tcl while inside /utils."
    puts "Example: cd utils; ./speed-regression.tcl"
    exit 1
}

# Make sure there is not already a server runnign on port 12123
set is_not_running [catch {set r [redis 127.0.0.1 $::port]}]
if {!$is_not_running} {
    puts "Sorry, you have a running server on port $::port"
    exit 1
}

# parse arguments
for {set j 0} {$j < [llength $argv]} {incr j} {
    set opt [lindex $argv $j]
    set arg [lindex $argv [expr $j+1]]
    if {$opt eq {--tests}} {
        set ::tests $arg
        incr j
    } elseif {$opt eq {--datasize}} {
        set ::datasize $arg
        incr j
    } elseif {$opt eq {--requests}} {
        set ::requests $arg
        incr j
    } else {
        puts "Wrong argument: $opt"
        exit 1
    }
}

main
