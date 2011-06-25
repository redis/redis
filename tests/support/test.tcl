set ::num_tests 0
set ::num_passed 0
set ::num_failed 0
set ::tests_failed {}

proc assert {condition} {
    if {![uplevel 1 expr $condition]} {
        error "assertion:Expected '$value' to be true"
    }
}

proc assert_match {pattern value} {
    if {![string match $pattern $value]} {
        error "assertion:Expected '$value' to match '$pattern'"
    }
}

proc assert_equal {expected value} {
    if {$expected ne $value} {
        error "assertion:Expected '$value' to be equal to '$expected'"
    }
}

proc assert_error {pattern code} {
    if {[catch {uplevel 1 $code} error]} {
        assert_match $pattern $error
    } else {
        error "assertion:Expected an error but nothing was catched"
    }
}

proc assert_encoding {enc key} {
    # Swapped out values don't have an encoding, so make sure that
    # the value is swapped in before checking the encoding.
    set dbg [r debug object $key]
    while {[string match "* swapped at:*" $dbg]} {
        r debug swapin $key
        set dbg [r debug object $key]
    }
    assert_match "* encoding:$enc *" $dbg
}

proc assert_type {type key} {
    assert_equal $type [r type $key]
}

# Test if TERM looks like to support colors
proc color_term {} {
    expr {[info exists ::env(TERM)] && [string match *xterm* $::env(TERM)]}
}

# This is called before starting the test
proc announce_test {s} {
    if {[color_term]} {
        puts -nonewline "$s\033\[0K"
        flush stdout
        set ::backward_count [string length $s]
    }
}

# This is called after the test finished
proc colored_dot {tags passed} {
    if {[color_term]} {
        # Go backward and delete what announc_test function printed.
        puts -nonewline "\033\[${::backward_count}D\033\[0K\033\[J"

        # Print a coloured char, accordingly to test outcome and tags.
        if {[lsearch $tags list] != -1} {
            set colorcode {31}
            set ch L
        } elseif {[lsearch $tags hash] != -1} {
            set colorcode {32}
            set ch H
        } elseif {[lsearch $tags set] != -1} {
            set colorcode {33}
            set ch S
        } elseif {[lsearch $tags zset] != -1} {
            set colorcode {34}
            set ch Z
        } elseif {[lsearch $tags basic] != -1} {
            set colorcode {35}
            set ch B
        } elseif {[lsearch $tags scripting] != -1} {
            set colorcode {36}
            set ch X
        } else {
            set colorcode {37}
            set ch .
        }
        if {$colorcode ne {}} {
            if {$passed} {
                puts -nonewline "\033\[0;${colorcode};40m"
            } else {
                puts -nonewline "\033\[7;${colorcode};40m"
            }
            puts -nonewline $ch
            puts -nonewline "\033\[0m"
            flush stdout
        }
    } else {
        if {$passed} {
            puts -nonewline .
        } else {
            puts -nonewline F
        }
    }
}

proc test {name code {okpattern undefined}} {
    # abort if tagged with a tag to deny
    foreach tag $::denytags {
        if {[lsearch $::tags $tag] >= 0} {
            return
        }
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
            return
        }
    }

    incr ::num_tests
    set details {}
    lappend details $::curfile
    lappend details $::tags
    lappend details $name

    if {$::verbose} {
        puts -nonewline [format "#%03d %-68s " $::num_tests $name]
        flush stdout
    } else {
        announce_test $name
    }

    if {[catch {set retval [uplevel 1 $code]} error]} {
        if {[string match "assertion:*" $error]} {
            set msg [string range $error 10 end]
            lappend details $msg
            lappend ::tests_failed $details

            incr ::num_failed
            if {$::verbose} {
                puts "FAILED"
                puts "$msg\n"
            } else {
                colored_dot $::tags 0
            }
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        if {$okpattern eq "undefined" || $okpattern eq $retval || [string match $okpattern $retval]} {
            incr ::num_passed
            if {$::verbose} {
                puts "PASSED"
            } else {
                colored_dot $::tags 1
            }
        } else {
            set msg "Expected '$okpattern' to equal or match '$retval'"
            lappend details $msg
            lappend ::tests_failed $details

            incr ::num_failed
            if {$::verbose} {
                puts "FAILED"
                puts "$msg\n"
            } else {
                colored_dot $::tags 0
            }
        }
    }
    flush stdout

    if {$::traceleaks} {
        set output [exec leaks redis-server]
        if {![string match {*0 leaks*} $output]} {
            puts "--- Test \"$name\" leaked! ---"
            puts $output
            exit 1
        }
    }
}
