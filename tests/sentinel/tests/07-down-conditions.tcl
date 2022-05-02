# Test conditions where an instance is considered to be down

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 1000
    S $id sentinel debug ask-period 100
    S $id sentinel debug default-down-after 3000
    S $id sentinel debug publish-period 200
    S $id sentinel debug ping-period 100
}

set ::alive_sentinel [expr {$::instances_count/2+2}]
proc ensure_master_up {} {
    S $::alive_sentinel sentinel debug info-period 1000
    S $::alive_sentinel sentinel debug ping-period 100
    S $::alive_sentinel sentinel debug ask-period 100
    S $::alive_sentinel sentinel debug publish-period 100
    wait_for_condition 1000 50 {
        [dict get [S $::alive_sentinel sentinel master mymaster] flags] eq "master"
    } else {
        fail "Master flags are not just 'master'"
    }
}

proc ensure_master_down {} {
    S $::alive_sentinel sentinel debug info-period 1000
    S $::alive_sentinel sentinel debug ping-period 100
    S $::alive_sentinel sentinel debug ask-period 100
    S $::alive_sentinel sentinel debug publish-period 100
    wait_for_condition 1000 50 {
        [string match *down* \
            [dict get [S $::alive_sentinel sentinel master mymaster] flags]]
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
    ensure_master_up
    set master_addr [S $::alive_sentinel SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $master_addr 1]]

    set pid [get_instance_attrib redis $master_id pid]
    exec kill -SIGSTOP $pid
    ensure_master_down
    exec kill -SIGCONT $pid
    ensure_master_up
}

test "SDOWN is triggered by crashed instance" {
    lassign [S $::alive_sentinel SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] host port
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

test "SDOWN is triggered by misconfigured instance replying with errors" {
    ensure_master_up
    set orig_dir [lindex [R 0 config get dir] 1]
    set orig_save [lindex [R 0 config get save] 1]
    # Set dir to / and filename to "tmp" to make sure it will fail.
    R 0 config set dir /
    R 0 config set dbfilename tmp
    R 0 config set save "1000000 1000000"
    after 5000
    R 0 bgsave
    after 5000
    ensure_master_down
    R 0 config set save $orig_save
    R 0 config set dir $orig_dir
    R 0 config set dbfilename dump.rdb
    R 0 bgsave
    ensure_master_up
}

# We use this test setup to also test command renaming, as a side
# effect of the master going down if we send PONG instead of PING
test "SDOWN is triggered if we rename PING to PONG" {
    ensure_master_up
    S $::alive_sentinel SENTINEL SET mymaster rename-command PING PONG
    ensure_master_down
    S $::alive_sentinel SENTINEL SET mymaster rename-command PING PING
    ensure_master_up
}
