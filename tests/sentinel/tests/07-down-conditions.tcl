# Test conditions where an instance is considered to be down

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

proc ensure_primary_up {} {
    wait_for_condition 1000 50 {
        [dict get [S 4 sentinel primary myprimary] flags] eq "primary"
    } else {
        fail "Primary flags are not just 'primary'"
    }
}

proc ensure_primary_down {} {
    wait_for_condition 1000 50 {
        [string match *down* \
            [dict get [S 4 sentinel primary myprimary] flags]]
    } else {
        fail "Primary is not flagged SDOWN"
    }
}

test "Crash the majority of Sentinels to prevent failovers for this unit" {
    for {set id 0} {$id < $quorum} {incr id} {
        kill_instance sentinel $id
    }
}

test "SDOWN is triggered by non-responding but not crashed instance" {
    lassign [S 4 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] host port
    ensure_primary_up
    exec ../../../src/redis-cli -h $host -p $port {*}[rediscli_tls_config "../../../tests"] debug sleep 10 > /dev/null &
    ensure_primary_down
    ensure_primary_up
}

test "SDOWN is triggered by crashed instance" {
    lassign [S 4 SENTINEL GET-MASTER-ADDR-BY-NAME myprimary] host port
    ensure_primary_up
    kill_instance redis 0
    ensure_primary_down
    restart_instance redis 0
    ensure_primary_up
}

test "SDOWN is triggered by primaries advertising as slaves" {
    ensure_primary_up
    R 0 slaveof 127.0.0.1 34567
    ensure_primary_down
    R 0 slaveof no one
    ensure_primary_up
}

test "SDOWN is triggered by misconfigured instance repling with errors" {
    ensure_primary_up
    set orig_dir [lindex [R 0 config get dir] 1]
    set orig_save [lindex [R 0 config get save] 1]
    # Set dir to / and filename to "tmp" to make sure it will fail.
    R 0 config set dir /
    R 0 config set dbfilename tmp
    R 0 config set save "1000000 1000000"
    R 0 bgsave
    ensure_primary_down
    R 0 config set save $orig_save
    R 0 config set dir $orig_dir
    R 0 config set dbfilename dump.rdb
    R 0 bgsave
    ensure_primary_up
}

# We use this test setup to also test command renaming, as a side
# effect of the primary going down if we send PONG instead of PING
test "SDOWN is triggered if we rename PING to PONG" {
    ensure_primary_up
    S 4 SENTINEL SET myprimary rename-command PING PONG
    ensure_primary_down
    S 4 SENTINEL SET myprimary rename-command PING PING
    ensure_primary_up
}
