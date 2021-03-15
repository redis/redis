start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {If min-slaves-to-write is honored, write is accepted} {
            r config set min-slaves-to-write 1
            r config set min-slaves-max-lag 10
            r set foo 12345
            wait_for_condition 50 100 {
                [r -1 get foo] eq {12345}
            } else {
                fail "Write did not reached replica"
            }
        }

        test {No write if min-slaves-to-write is < attached slaves} {
            r config set min-slaves-to-write 2
            r config set min-slaves-max-lag 10
            catch {r set foo 12345} err
            set err
        } {NOREPLICAS*}

        test {If min-slaves-to-write is honored, write is accepted (again)} {
            r config set min-slaves-to-write 1
            r config set min-slaves-max-lag 10
            r set foo 12345
            wait_for_condition 50 100 {
                [r -1 get foo] eq {12345}
            } else {
                fail "Write did not reached replica"
            }
        }

        test {No write if min-slaves-max-lag is > of the slave lag} {
            r config set min-slaves-to-write 1
            r config set min-slaves-max-lag 2
            exec kill -SIGSTOP [srv -1 pid]
            assert {[r set foo 12345] eq {OK}}
            wait_for_condition 100 100 {
                [catch {r set foo 12345}] != 0
            } else {
                fail "Master didn't become readonly"
            }
            catch {r set foo 12345} err
            assert_match {NOREPLICAS*} $err
        }
        exec kill -SIGCONT [srv -1 pid]

        test {min-slaves-to-write is ignored by slaves} {
            r config set min-slaves-to-write 1
            r config set min-slaves-max-lag 10
            r -1 config set min-slaves-to-write 1
            r -1 config set min-slaves-max-lag 10
            r set foo aaabbb
            wait_for_condition 50 100 {
                [r -1 get foo] eq {aaabbb}
            } else {
                fail "Write did not reached replica"
            }
        }

        # Fix parameters for the next test to work
        r config set min-slaves-to-write 0
        r -1 config set min-slaves-to-write 0
        r flushall

        test {MASTER and SLAVE dataset should be identical after complex ops} {
            createComplexDataset r 10000
            after 500
            if {[r debug digest] ne [r -1 debug digest]} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Master - Replica inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}
