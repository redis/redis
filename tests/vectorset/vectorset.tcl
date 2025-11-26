proc check_python_environment {} {
    set ret [catch {exec sh -c "which python3 || which python" 2>@1} python_cmd]
    if {$ret != 0} {
        return ""
    }

    # Check if redis-py is installed
    # Use a more robust way to check redis module
    set ret [catch {exec $python_cmd -c "import redis" 2>@1} e]
    if {$ret != 0} {
        return ""
    }

    return $python_cmd
}

start_server {} {
    set slave_port [srv 0 port]

    start_server {} {
        set master_port [srv 0 port]

        test {Vector set Python test execution} {
            set python_cmd [check_python_environment]
            if {$python_cmd eq ""} {
                puts "Python or redis-py module not found, skipping vectorset tests"
            } else {
                # Run the Python script with real-time output
                puts "Running vectorset tests ..."
                puts "Vectorset test output:"

                set pipe [open "|$python_cmd modules/vector-sets/test.py --primary-port $master_port --replica-port $slave_port 2>@1" r]
                # Read output line by line in real-time
                while {[gets $pipe line] >= 0} {
                    puts $line
                }

                # Close pipe and check for errors
                set result [catch {close $pipe} close_error]
                if {$result != 0} {
                    fail "Vector set Python test failed: $close_error"
                }
            }
        }
    }
}
