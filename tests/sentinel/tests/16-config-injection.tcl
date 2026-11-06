# Test that control characters are rejected where appropriate, and that
# string values are safely quoted when persisted to disk.
#
# Config injection is prevented by sentinelSdscatConfigArg(), which escapes
# values containing special characters at persistence time. Fields like
# notification-script, rename-command, master name, and announce-ip also
# reject control characters at input time as an additional safeguard.

source "../tests/includes/init-tests.tcl"

# Helper: read the sentinel config file for a given sentinel id.
proc read_sentinel_config {id} {
    set configfile [file join "sentinel_${id}" "sentinel.conf"]
    set fp [open $configfile r]
    set content [read $fp]
    close $fp
    return $content
}

# Helper: count how many lines in the config match a pattern.
proc count_config_lines {content pattern} {
    set count 0
    foreach line [split $content "\n"] {
        if {[string match $pattern $line]} {
            incr count
        }
    }
    return $count
}

# Helper: restart a (already stopped) sentinel and wait until it responds to PING.
proc start_sentinel_and_wait {sid} {
    restart_instance sentinel $sid
    wait_for_condition 200 50 {
        [catch {S $sid PING}] == 0
    } else {
        fail "Sentinel $sid did not restart in time"
    }
}

# Helper: kill sentinel, restart it, and wait until it responds to PING.
proc restart_sentinel_and_wait {sid} {
    kill_instance sentinel $sid
    start_sentinel_and_wait $sid
}

# Helper: assert that the sentinel config file contains the expected substring.
proc assert_config_contains {sid expected} {
    set content [read_sentinel_config $sid]
    assert {[string first $expected $content] >= 0}
}

# Helper: append lines to a sentinel's config file (sentinel must be stopped).
proc append_to_sentinel_config {sid lines} {
    set configfile [file join "sentinel_${sid}" "sentinel.conf"]
    set fp [open $configfile a]
    foreach line $lines {
        puts $fp $line
    }
    close $fp
}

# Helper: create an executable script with spaces in its path.
# Returns the full path. Caller should "file delete -force" the directory.
proc create_script_with_spaces {sid} {
    set script_dir [file join [pwd] "sentinel_${sid}" "script dir"]
    file mkdir $script_dir
    set script_path [file join $script_dir "my script.sh"]
    set fp [open $script_path w]
    puts $fp "#!/bin/sh"
    close $fp
    file attributes $script_path -permissions 0755
    return $script_path
}

# --------------------------------------------------------------------------
# Section 1: Control character rejection in SENTINEL SET
# --------------------------------------------------------------------------

test "SENTINEL SET notification-script rejects control characters" {
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL SET mymaster notification-script "/tmp/ok\n/tmp/evil.sh"
    }
}

test "SENTINEL SET client-reconfig-script rejects control characters" {
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL SET mymaster client-reconfig-script "/tmp/ok\n/tmp/evil.sh"
    }
}

test "SENTINEL SET rename-command rejects control characters" {
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL SET mymaster rename-command "CONFIG\nEVIL" "NEWCONFIG"
    }
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL SET mymaster rename-command "CONFIG" "NEW\nCONFIG"
    }
}

# --------------------------------------------------------------------------
# Section 2: Control character rejection in SENTINEL MONITOR
# --------------------------------------------------------------------------

test "SENTINEL MONITOR rejects master name with control characters" {
    set port [get_instance_attrib redis 0 port]
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL MONITOR "bad\nmaster" 127.0.0.1 $port 2
    }
    assert_error "*must not contain control characters*" {
        S 0 SENTINEL MONITOR "bad\rmaster" 127.0.0.1 $port 2
    }
}

# --------------------------------------------------------------------------
# Section 3: Control character rejection in SENTINEL CONFIG SET
# --------------------------------------------------------------------------

test "SENTINEL CONFIG SET announce-ip rejects control characters" {
    catch {S 0 SENTINEL CONFIG SET announce-ip "1.2.3.4\nevil-directive"} e
    assert_match "*must not contain control characters*" $e
}

# --------------------------------------------------------------------------
# Section 4: Config injection attempt does not pollute config file
# --------------------------------------------------------------------------

test "Newline injection in auth-pass does not pollute config file" {
    # Auth-pass accepts control characters, but sentinelSdscatConfigArg
    # escapes them at persistence time, preventing config injection.
    S 0 SENTINEL SET mymaster auth-pass "x\nsentinel notification-script mymaster /tmp/evil.sh"
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    assert {[count_config_lines $content "sentinel notification-script mymaster /tmp/evil.sh"] == 0}
    assert_config_contains 0 {sentinel auth-pass mymaster "x\nsentinel notification-script mymaster /tmp/evil.sh"}
    S 0 SENTINEL SET mymaster auth-pass ""
}

test "Newline injection in auth-user does not pollute config file" {
    S 0 SENTINEL SET mymaster auth-user "x\nsentinel notification-script mymaster /tmp/evil.sh"
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    assert {[count_config_lines $content "sentinel notification-script mymaster /tmp/evil.sh"] == 0}
    assert_config_contains 0 {sentinel auth-user mymaster "x\nsentinel notification-script mymaster /tmp/evil.sh"}
    S 0 SENTINEL SET mymaster auth-user ""
}

test "Newline injection in sentinel-pass does not pollute config file" {
    S 0 SENTINEL CONFIG SET sentinel-pass "x\nsentinel notification-script mymaster /tmp/evil.sh"
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    assert {[count_config_lines $content "sentinel notification-script mymaster /tmp/evil.sh"] == 0}
    assert_config_contains 0 {sentinel sentinel-pass "x\nsentinel notification-script mymaster /tmp/evil.sh"}
    S 0 SENTINEL CONFIG SET sentinel-pass ""
}

test "Newline injection in sentinel-user does not pollute config file" {
    S 0 SENTINEL CONFIG SET sentinel-user "x\nsentinel notification-script mymaster /tmp/evil.sh"
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    assert {[count_config_lines $content "sentinel notification-script mymaster /tmp/evil.sh"] == 0}
    assert_config_contains 0 {sentinel sentinel-user "x\nsentinel notification-script mymaster /tmp/evil.sh"}
    S 0 SENTINEL CONFIG SET sentinel-user ""
}

# --------------------------------------------------------------------------
# Section 5: Values with special characters survive config round-trip
# --------------------------------------------------------------------------

test "auth-pass with special characters persists correctly through restart" {
    S 0 SENTINEL SET mymaster auth-pass {my "comp#$&^`'!,lex pass}
    set expected {sentinel auth-pass mymaster "my \"comp#$&^`'!,lex pass"}
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    restart_sentinel_and_wait 0
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    S 0 SENTINEL SET mymaster auth-pass ""
}

test "auth-user with spaces persists correctly through restart" {
    S 0 SENTINEL SET mymaster auth-user {user with spaces}
    set expected {sentinel auth-user mymaster "user with spaces"}
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    restart_sentinel_and_wait 0
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    S 0 SENTINEL SET mymaster auth-user ""
}

test "notification-script with spaces persists correctly through restart" {
    set script_path [create_script_with_spaces 0]
    S 0 SENTINEL SET mymaster notification-script $script_path
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    # The path must be quoted since it contains spaces.
    assert {[string first "notification-script" $content] >= 0}
    restart_sentinel_and_wait 0
    set info [S 0 SENTINEL MASTER mymaster]
    set idx [lsearch $info "notification-script"]
    assert {$idx >= 0}
    assert_equal [lindex $info [expr {$idx+1}]] $script_path
    S 0 SENTINEL SET mymaster notification-script ""
    file delete -force [file dirname $script_path]
}

test "client-reconfig-script with spaces persists correctly through restart" {
    set script_path [create_script_with_spaces 0]
    S 0 SENTINEL SET mymaster client-reconfig-script $script_path
    S 0 SENTINEL FLUSHCONFIG
    set content [read_sentinel_config 0]
    # The path must be quoted since it contains spaces.
    assert {[string first "client-reconfig-script" $content] >= 0}
    restart_sentinel_and_wait 0
    set info [S 0 SENTINEL MASTER mymaster]
    set idx [lsearch $info "client-reconfig-script"]
    assert {$idx >= 0}
    assert_equal [lindex $info [expr {$idx+1}]] $script_path
    S 0 SENTINEL SET mymaster client-reconfig-script ""
    file delete -force [file dirname $script_path]
}

test "rename-command persists unquoted through restart" {
    S 0 SENTINEL SET mymaster rename-command CONFIG CONF_RENAMED
    set expected {sentinel rename-command mymaster CONFIG CONF_RENAMED}
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    restart_sentinel_and_wait 0
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    S 0 SENTINEL SET mymaster rename-command CONFIG CONFIG
}

# --------------------------------------------------------------------------
# Section 6: Backward compatibility -- old unquoted config format still loads
# --------------------------------------------------------------------------

test "Old unquoted config format for auth-pass and auth-user loads correctly" {
    kill_instance sentinel 0
    append_to_sentinel_config 0 {
        "sentinel auth-pass mymaster oldformatpass"
        "sentinel auth-user mymaster oldformatuser"
    }
    start_sentinel_and_wait 0
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 "sentinel auth-pass mymaster oldformatpass"
    assert_config_contains 0 "sentinel auth-user mymaster oldformatuser"
    S 0 SENTINEL SET mymaster auth-pass ""
    S 0 SENTINEL SET mymaster auth-user ""
}

test "Old unquoted config format for rename-command loads correctly" {
    kill_instance sentinel 0
    append_to_sentinel_config 0 {
        "sentinel rename-command mymaster CONFIG NEWCONFIGNAME"
    }
    start_sentinel_and_wait 0
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 "sentinel rename-command mymaster CONFIG NEWCONFIGNAME"
    S 0 SENTINEL SET mymaster rename-command CONFIG CONFIG
}

test "Old unquoted config format for sentinel-pass loads correctly" {
    kill_instance sentinel 0
    append_to_sentinel_config 0 {
        "sentinel sentinel-pass oldsentinelpass"
    }
    start_sentinel_and_wait 0
    set result [S 0 SENTINEL CONFIG GET sentinel-pass]
    assert_equal [lindex $result 1] "oldsentinelpass"
    S 0 SENTINEL CONFIG SET sentinel-pass ""
}

test "Old unquoted config format for sentinel-user loads correctly" {
    kill_instance sentinel 0
    append_to_sentinel_config 0 {
        "sentinel sentinel-user oldsentineluser"
    }
    start_sentinel_and_wait 0
    set result [S 0 SENTINEL CONFIG GET sentinel-user]
    assert_equal [lindex $result 1] "oldsentineluser"
    S 0 SENTINEL CONFIG SET sentinel-user ""
}

# --------------------------------------------------------------------------
# Section 7: Values with special characters survive config round-trip
# --------------------------------------------------------------------------

test "sentinel-pass with special characters persists correctly through restart" {
    set test_pass {sentinel pass word}
    S 0 SENTINEL CONFIG SET sentinel-pass $test_pass
    set expected {sentinel sentinel-pass "sentinel pass word"}
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    restart_sentinel_and_wait 0
    set result [S 0 SENTINEL CONFIG GET sentinel-pass]
    assert_equal [lindex $result 1] $test_pass
    S 0 SENTINEL CONFIG SET sentinel-pass ""
}

test "sentinel-user with special characters persists correctly through restart" {
    set test_user {sentinel user name}
    S 0 SENTINEL CONFIG SET sentinel-user $test_user
    set expected {sentinel sentinel-user "sentinel user name"}
    S 0 SENTINEL FLUSHCONFIG
    assert_config_contains 0 $expected
    restart_sentinel_and_wait 0
    set result [S 0 SENTINEL CONFIG GET sentinel-user]
    assert_equal [lindex $result 1] $test_user
    S 0 SENTINEL CONFIG SET sentinel-user ""
}
