set ::num_tests 0
set ::num_passed 0
set ::num_failed 0
set ::num_skipped 0
set ::num_aborted 0
set ::tests_failed {}

proc fail {msg} {
    error "assertion:$msg"
}

proc assert {condition} {
    if {![uplevel 1 [list expr $condition]]} {
        error "assertion:Expected condition '$condition' to be true ([uplevel 1 [list subst -nocommands $condition]])"
    }
}

proc assert_match {pattern value} {
    if {![string match $pattern $value]} {
        error "assertion:Expected '$value' to match '$pattern'"
    }
}

proc assert_equal {expected value {detail ""}} {
    if {$expected ne $value} {
        if {$detail ne ""} {
            set detail " (detail: $detail)"
        }
        error "assertion:Expected '$value' to be equal to '$expected'$detail"
    }
}

proc assert_error {pattern code} {
    if {[catch {uplevel 1 $code} error]} {
        assert_match $pattern $error
    } else {
        error "assertion:Expected an error but nothing was caught"
    }
}

proc assert_encoding {enc key} {
    set dbg [r debug object $key]
    assert_match "* encoding:$enc *" $dbg
}

proc assert_type {type key} {
    assert_equal $type [r type $key]
}

# Wait for the specified condition to be true, with the specified number of
# max retries and delay between retries. Otherwise the 'elsescript' is
# executed.
proc wait_for_condition {maxtries delay e _else_ elsescript} {
    while {[incr maxtries -1] >= 0} {
        set errcode [catch {uplevel 1 [list expr $e]} result]
        if {$errcode == 0} {
            if {$result} break
        } else {
            return -code $errcode $result
        }
        after $delay
    }
    if {$maxtries == -1} {
        set errcode [catch [uplevel 1 $elsescript] result]
        return -code $errcode $result
    }
}

proc test {name code {okpattern undefined}} {
    # abort if tagged with a tag to deny
    foreach tag $::denytags {
        if {[lsearch $::tags $tag] >= 0} {
            incr ::num_aborted
            send_data_packet $::test_server_fd ignore $name
            return
        }
    }

    # abort if test name in skiptests
    if {[lsearch $::skiptests $name] >= 0} {
        incr ::num_skipped
        send_data_packet $::test_server_fd skip $name
        return
    }

    # abort if test name in skiptests
    if {[llength $::only_tests] > 0 && [lsearch $::only_tests $name] < 0} {
        incr ::num_skipped
        send_data_packet $::test_server_fd skip $name
        return
    }

    # check if tagged with at least 1 tag to allow when there *is* a list
    # of tags to allow, because default policy is to run everything
    if {[llength $::allowtags] > 0} {
        set matched 0
        foreach tag $::allowtags {
            if {[lsearch $::tags $tag] >= 0} {
                incr matched
            }
        }
        if {$matched < 1} {
            incr ::num_aborted
            send_data_packet $::test_server_fd ignore $name
            return
        }
    }

    incr ::num_tests
    set details {}
    lappend details "$name in $::curfile"

    send_data_packet $::test_server_fd testing $name

    if {[catch {set retval [uplevel 1 $code]} error]} {
        if {[string match "assertion:*" $error]} {
            set msg [string range $error 10 end]
            lappend details $msg
            lappend ::tests_failed $details

            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        if {$okpattern eq "undefined" || $okpattern eq $retval || [string match $okpattern $retval]} {
            incr ::num_passed
            send_data_packet $::test_server_fd ok $name
        } else {
            set msg "Expected '$okpattern' to equal or match '$retval'"
            lappend details $msg
            lappend ::tests_failed $details

            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]
        }
    }

    if {$::traceleaks} {
        set output [exec leaks redis-server]
        if {![string match {*0 leaks*} $output]} {
            send_data_packet $::test_server_fd err "Detected a memory leak in test '$name': $output"
        }
    }
}
