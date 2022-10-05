source tests/support/cli.tcl

start_server {tags {"wait network external:skip"}} {
start_server {} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_pid [srv 0 pid]
    set master [srv -1 client]
    set master_host [srv -1 host]
    set master_port [srv -1 port]

    test {Setup slave} {
        $slave slaveof $master_host $master_port
        wait_for_condition 50 100 {
            [s 0 master_link_status] eq {up}
        } else {
            fail "Replication not started."
        }
    }

    test {WAIT replicas number should be a number, 'majority', 'all'} {
        assert {[$master wait 1 5000] == 1}
        assert {[$master wait majority 5000] == 1}
        assert {[$master wait all 5000] == 1}

        set err "*replicas*number*majority*all*"
        catch {[$master wait -1 5000]} e
        assert_match $err $e
        catch {[$master wait less 5000]} e
        assert_match $err $e
    }

    test {WAIT should acknowledge 1 additional copy of the data} {
        $master set foo 0
        $master incr foo
        $master incr foo
        $master incr foo
        assert {[$master wait 1 5000] == 1}
        assert {[$slave get foo] == 3}
    }

    test {WAIT should not acknowledge 2 additional copies of the data} {
        $master incr foo
        assert {[$master wait 2 1000] <= 1}
    }

    test {WAIT should not acknowledge 1 additional copy if slave is blocked} {
        exec kill -SIGSTOP $slave_pid
        $master set foo 0
        $master incr foo
        $master incr foo
        $master incr foo
        assert {[$master wait 1 1000] == 0}
        exec kill -SIGCONT $slave_pid
        assert {[$master wait 1 1000] == 1}
    }

    test {WAIT implicitly blocks on client pause since ACKs aren't sent} {
        exec kill -SIGSTOP $slave_pid
        $master multi
        $master incr foo
        $master client pause 10000 write
        $master exec
        assert {[$master wait 1 1000] == 0}
        $master client unpause
        exec kill -SIGCONT $slave_pid
        assert {[$master wait 1 1000] == 1}
    }
}}
