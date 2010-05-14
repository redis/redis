set ::passed 0
set ::failed 0
set ::testnum 0

proc test {name code okpattern} {
    incr ::testnum
    # if {$::testnum < $::first || $::testnum > $::last} return
    puts -nonewline [format "#%03d %-70s " $::testnum $name]
    flush stdout
    set retval [uplevel 1 $code]
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
