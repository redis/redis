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
        r fork.create 3
        wait_for_condition 20 100 {
            [r fork.exitcode] != -1
        } else {
            fail "fork didn't terminate"
        }
        r fork.exitcode
    } {3}

    test {Module fork kill} {
        r fork.create 3
        after 250
        r fork.kill

        assert {[count_log_message "fork child started"] eq "2"}
        assert {[count_log_message "Received SIGUSR1 in child"] eq "1"}
        assert {[count_log_message "fork child exiting"] eq "1"}
    }

    test {Module fork twice} {
        r fork.create 0
        after 250
        catch {r fork.create 0}
        assert {[count_log_message "Can't fork for module: File exists"] eq "1"}
    }

    test "Unload the module - fork" {
        assert_equal {OK} [r module unload fork]
    }
}
