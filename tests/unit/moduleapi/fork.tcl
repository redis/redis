set testmodule [file normalize tests/modules/fork.so]

proc count_log_message {pattern} {
    set status [catch {exec grep -c $pattern < [srv 0 stdout]} result]
    if {$status == 1} {
        set result 0
    }
    return $result
}

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module fork} {
        # the argument to fork.create is the exitcode on termination
        # the second argument to fork.create is passed to usleep
        r fork.create 3 100000 ;# 100ms
        wait_for_condition 20 100 {
            [r fork.exitcode] != -1
        } else {
            fail "fork didn't terminate"
        }
        r fork.exitcode
    } {3}

    test {Module fork kill} {
        # use a longer time to avoid the child exiting before being killed
        r fork.create 3 100000000 ;# 100s
        wait_for_condition 20 100 {
            [count_log_message "fork child started"] == 2
        } else {
            fail "fork didn't start"
        }
        r fork.kill

        assert {[count_log_message "Received SIGUSR1 in child"] eq "1"}
        assert {[count_log_message "fork child exiting"] eq "1"}
    }

    test {Module fork twice} {
        r fork.create 0 10000000 ;# 10s
        wait_for_condition 20 100 {
            [count_log_message "fork child started"] == 3
        } else {
            fail "fork didn't start"
        }
        catch {r fork.create 0 1}
        assert {[count_log_message "Can't fork for module: File exists"] eq "1"}
    }

    test "Unload the module - fork" {
        assert_equal {OK} [r module unload fork]
    }
}
