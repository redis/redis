# Check that slaves are reconfigured at a latter time if they are partitioned.
#
# Here we should test:
# 1) That slaves point to the new master after failover.
# 2) That partitioned slaves point to new master when they are partitioned
#    away during failover and return at a latter time.

source "../tests/includes/init-tests.tcl"

test "Check acceptable replica-priority values" {
    foreach_redis_id id {
        if {$id == $master_id} continue

        # ensure a priority of -2 throw an error
        catch {R $id CONFIG SET replica-priority -2} e
        if {$e eq "OK"} {
            fail "Able to set replica-priority -2 to one redis replica whereas it should not be possible"
        }

        # ensure a priority between -1 and 1000 is valid
        for {set priority -1} {$priority <= 1000} {incr priority} {
            catch {R $id CONFIG SET replica-priority $priority} e
            if {$e ne "OK"} {
                fail "Unable to set replica-priority $priority to one redis replica"
            }
        }

        # test only the first redis replica, no need to double test
        break
    }
}

proc 08_test_number_of_replicas {} {
    uplevel 1 {
        test "Check sentinel replies with replicas ignoring those with priority of -1" {
            # count number of replicas
            set n_replicas 0
            foreach_redis_id id {
                if {$id == $master_id} continue
                if {[R $id CONFIG GET replica-priority] ne "replica-priority -1"} {
                    incr n_replicas
                }
            }

            # ensure sentinels replies with the right number of replicas
            foreach_sentinel_id id {
                # retries 40 x 500ms = 20s as SENTINEL_INFO_PERIOD = 10s
                wait_for_condition 40 500 {
                    [llength [S $id SENTINEL REPLICAS mymaster]] == $n_replicas
                } else {
                    fail "Sentinel replies with a wrong number of replicas with priority >=0"
                }
            }
        }
    }
}


08_test_number_of_replicas
test "Set replica-priority -1 of the first found replica" {
    foreach_redis_id id {
        if {$id == $master_id} continue
        R $id CONFIG SET replica-priority "-1"
        break
    }
}
08_test_number_of_replicas
