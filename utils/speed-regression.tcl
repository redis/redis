#!/usr/bin/env tclsh8.5
# Copyright (C) 2011 Salvatore Sanfilippo
# Released under the BSD license like Redis itself

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
        set pids [exec echo "port 12123\nloglevel warning\n" | ./redis-server - > /dev/null 2> /dev/null &]
        after 1000
        puts "  running the benchmark"
        set output [exec /tmp/redis-benchmark -n 100000 --csv -p 12123]
        lappend runs $b $output
        puts "  killing server..."
        catch {
            exec kill -9 [lindex $pids 0]
            exec kill -9 [lindex $pids 1]
        }
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

proc combine-results {results} {
    set tests {
        ping set get incr lpush lpop sadd spop
        "lrange (first 100 elements)"
        "lrange (first 600 elements)"
        "mset (10 keys)"
    }
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
    puts [combine-results $results]
}

# Force the user to run the script from the 'utils' directory.
if {![file exists speed-regression.tcl]} {
    puts "Please make sure to run speed-regression.tcl while inside /utils."
    puts "Example: cd utils; ./speed-regression.tcl"
    exit 1
}
main
