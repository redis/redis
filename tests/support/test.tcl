set ::passed 0
set ::failed 0
set ::testnum 0

proc test {name code okpattern} {
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
            if {[lsearch $::tags $tag]} {
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
        puts "EXCEPTION"
        puts "\nCaught error: $error"
        error "exception"
    }
    if {$okpattern eq $retval || [string match $okpattern $retval]} {
        puts "PASSED"
        incr ::passed
    } else {
        puts "!! ERROR expected\n'$okpattern'\nbut got\n'$retval'"
        incr ::failed
    }
    if {$::traceleaks} {
        if {![string match {*0 leaks*} [exec leaks redis-server]]} {
            puts "--------- Test $::testnum LEAKED! --------"
            exit 1
        }
    }
}
