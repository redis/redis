# Test conditions where an instance is considered to be down

source "../tests/includes/init-tests.tcl"

proc ensure_master_up {} {
    wait_for_condition 1000 50 {
        [dict get [S 4 sentinel master mymaster] flags] eq "master"
    } else {
        fail "Master flags are not just 'master'"
    }
}

proc ensure_master_down {} {
    wait_for_condition 1000 50 {
        [string match *down* \
            [dict get [S 4 sentinel master mymaster] flags]]
    } else {
        fail "Master is not flagged SDOWN"
    }
}

test "Crash the majority of Sentinels to prevent failovers for this unit" {
    for {set id 0} {$id < $quorum} {incr id} {
        kill_instance sentinel $id
    }
}

test "SDOWN is triggered by non-responding but not crashed instance" {
    lassign [S 4 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] host port
    ensure_master_up
    exec ../../../src/redis-cli -h $host -p $port debug sleep 10 > /dev/null &
    ensure_master_down
    ensure_master_up
}

test "SDOWN is triggered by crashed instance" {
    lassign [S 4 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] host port
    ensure_master_up
    kill_instance redis 0
    ensure_master_down
    restart_instance redis 0
    ensure_master_up
}

test "SDOWN is triggered by masters advertising as slaves" {
    ensure_master_up
    R 0 slaveof 127.0.0.1 34567
    ensure_master_down
    R 0 slaveof no one
    ensure_master_up
}

test "SDOWN is triggered by misconfigured instance repling with errors" {
    ensure_master_up
    set orig_dir [lindex [R 0 config get dir] 1]
    set orig_save [lindex [R 0 config get save] 1]
    # Set dir to / and filename to "tmp" to make sure it will fail.
    R 0 config set dir /
    R 0 config set dbfilename tmp
    R 0 config set save "1000000 1000000"
    R 0 bgsave
    ensure_master_down
    R 0 config set save $orig_save
    R 0 config set dir $orig_dir
    R 0 config set dbfilename dump.rdb
    R 0 bgsave
    ensure_master_up
}
