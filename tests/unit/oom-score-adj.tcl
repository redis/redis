set system_name [string tolower [exec uname -s]]
set user_id [exec id -u]

if {$system_name eq {linux}} {
    start_server {tags {"oom-score-adj"}} {
        proc get_oom_score_adj {{pid ""}} {
            if {$pid == ""} {
                set pid [srv 0 pid]
            }
            set fd [open "/proc/$pid/oom_score_adj" "r"]
            set val [gets $fd]
            close $fd

            return $val
        }

        test {CONFIG SET oom-score-adj works as expected} {
            set base [get_oom_score_adj]

            # Enable oom-score-adj, check defaults
            r config set oom-score-adj-values "10 20 30"
            r config set oom-score-adj yes

            assert {[get_oom_score_adj] == [expr $base + 10]}

            # Modify current class
            r config set oom-score-adj-values "15 20 30"
            assert {[get_oom_score_adj] == [expr $base + 15]}

            # Check replica class
            r replicaof localhost 1
            assert {[get_oom_score_adj] == [expr $base + 20]}
            r replicaof no one
            assert {[get_oom_score_adj] == [expr $base + 15]}

            # Check child process
            r set key-a value-a
            r config set rdb-key-save-delay 1000000
            r bgsave

            set child_pid [get_child_pid 0]
            assert_equal [get_oom_score_adj $child_pid] [expr $base + 30]
        }

        # Failed oom-score-adj tests can only run unprivileged
        if {$user_id != 0} {
            test {CONFIG SET oom-score-adj handles configuration failures} {
                # Bad config
                r config set oom-score-adj no
                r config set oom-score-adj-values "-1000 -1000 -1000"

                # Make sure it fails
                catch {r config set oom-score-adj yes} e
                assert_match {*Failed to set*} $e

                # Make sure it remains off
                assert {[r config get oom-score-adj] == "oom-score-adj no"}

                # Fix config
                r config set oom-score-adj-values "0 100 100"
                r config set oom-score-adj yes

                # Make sure it fails
                catch {r config set oom-score-adj-values "-1000 -1000 -1000"} e
                assert_match {*Failed*} $e

                # Make sure previous values remain
                assert {[r config get oom-score-adj-values] == {oom-score-adj-values {0 100 100}}}
            }
        }
    }
}
