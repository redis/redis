set ::passed 0
set ::failed 0
set ::testnum 0

proc test {name code okpattern} {
    incr ::testnum
    # if {$::testnum < $::first || $::testnum > $::last} return
    puts -nonewline [format "#%03d %-68s " $::testnum $name]
    flush stdout
    if {[catch {set retval [uplevel 1 $code]} error]} {
        puts "ERROR\n\nLogged warnings:"
        foreach file [glob tests/tmp/server.[pid].*/stdout] {
            set warnings [warnings_from_file $file]
            if {[string length $warnings] > 0} {
                puts $warnings
            }
        }
        puts "Script died with $error"
        exit 1
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
