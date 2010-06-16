set ::passed 0
set ::failed 0
set ::testnum 0

proc assert {condition} {
    if {![uplevel 1 expr $condition]} {
        puts "!! ERROR\nExpected '$value' to evaluate to true"
        error "assertion"
    }
}

proc assert_match {pattern value} {
    if {![string match $pattern $value]} {
        puts "!! ERROR\nExpected '$value' to match '$pattern'"
        error "assertion"
    }
}

proc assert_equal {expected value} {
    if {$expected ne $value} {
        puts "!! ERROR\nExpected '$value' to be equal to '$expected'"
        error "assertion"
    }
}

proc assert_error {pattern code} {
    if {[catch {uplevel 1 $code} error]} {
        assert_match $pattern $error
    } else {
        puts "!! ERROR\nExpected an error but nothing was catched"
        error "assertion"
    }
}

proc assert_encoding {enc key} {
    # swapped out value doesn't have encoding, so swap in first
    r debug swapin $key
    assert_match "* encoding:$enc *" [r debug object $key]
}

proc assert_type {type key} {
    assert_equal $type [r type $key]
}

proc test {name code {okpattern notspecified}} {
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

    incr ::testnum
    puts -nonewline [format "#%03d %-68s " $::testnum $name]
    flush stdout
    if {[catch {set retval [uplevel 1 $code]} error]} {
        if {$error eq "assertion"} {
            incr ::failed
        } else {
            puts "EXCEPTION"
            puts "\nCaught error: $error"
            error "exception"
        }
    } else {
        if {$okpattern eq "notspecified" || $okpattern eq $retval || [string match $okpattern $retval]} {
            puts "PASSED"
            incr ::passed
        } else {
            puts "!! ERROR expected\n'$okpattern'\nbut got\n'$retval'"
            incr ::failed
        }
    }
    if {$::traceleaks} {
        if {![string match {*0 leaks*} [exec leaks redis-server]]} {
            puts "--------- Test $::testnum LEAKED! --------"
            exit 1
        }
    }
}
