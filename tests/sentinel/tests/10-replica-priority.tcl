source "../tests/includes/init-tests.tcl"

test "Check acceptable replica-priority values" {
    foreach_redis_id id {
        if {$id == $master_id} continue

        # ensure replica-announced accepts yes and no
        catch {R $id CONFIG SET replica-announced no} e
        if {$e ne "OK"} {
            fail "Unable to set replica-announced to no"
        }
        catch {R $id CONFIG SET replica-announced yes} e
        if {$e ne "OK"} {
            fail "Unable to set replica-announced to yes"
        }

        # ensure a random value throw error
        catch {R $id CONFIG SET replica-announced 321} e
        if {$e eq "OK"} {
            fail "Able to set replica-announced with something else than yes or no (321) whereas it should not be possible"
        }
        catch {R $id CONFIG SET replica-announced a3b2c1} e
        if {$e eq "OK"} {
            fail "Able to set replica-announced with something else than yes or no (a3b2c1) whereas it should not be possible"
        }

        # test only the first redis replica, no need to double test
        break
    }
}

proc 10_test_number_of_replicas {n_replicas_expected} {
    test "Check sentinel replies with $n_replicas_expected replicas" {
        # ensure sentinels replies with the right number of replicas
        foreach_sentinel_id id {
            S $id sentinel debug info-period 100
            S $id sentinel debug default-down-after 1000
            S $id sentinel debug publish-period 100
            set len [llength [S $id SENTINEL REPLICAS mymaster]]
            wait_for_condition 200 100 {
                [llength [S $id SENTINEL REPLICAS mymaster]] == $n_replicas_expected
            } else {
                fail "Sentinel replies with a wrong number of replicas with replica-announced=yes (expected $n_replicas_expected but got $len) on sentinel $id"
            }
        }
    }
}

proc 10_set_replica_announced {master_id announced n_replicas} {
    test "Set replica-announced=$announced on $n_replicas replicas" {
        set i 0
        foreach_redis_id id {
            if {$id == $master_id} continue
            #puts "set replica-announce=$announced on redis #$id"
            R $id CONFIG SET replica-announced "$announced"
            incr i
            if { $n_replicas!="all" && $i >= $n_replicas } { break }
        }
    }
}

# ensure all replicas are announced
10_set_replica_announced $master_id "yes" "all"
# ensure all replicas are announced by sentinels
10_test_number_of_replicas 4

# ensure the first 2 replicas are not announced
10_set_replica_announced $master_id "no" 2
# ensure sentinels are not announcing the first 2 replicas that have been set unannounced
10_test_number_of_replicas 2

# ensure all replicas are announced
10_set_replica_announced $master_id "yes" "all"
# ensure all replicas are not announced by sentinels
10_test_number_of_replicas 4

