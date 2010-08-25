start_server {} {
    test {Handle an empty query well} {
        set fd [r channel]
        puts -nonewline $fd "\r\n"
        flush $fd
        r ping
    } {PONG}

    test {Negative multi bulk command does not create problems} {
        set fd [r channel]
        puts -nonewline $fd "*-10\r\n"
        flush $fd
        r ping
    } {PONG}

    test {Negative multi bulk payload} {
        set fd [r channel]
        puts -nonewline $fd "SET x -10\r\n"
        flush $fd
        gets $fd
    } {*invalid bulk*}

    test {Too big bulk payload} {
        set fd [r channel]
        puts -nonewline $fd "SET x 2000000000\r\n"
        flush $fd
        gets $fd
    } {*invalid bulk*count*}

    test {bulk payload is not a number} {
        set fd [r channel]
        puts -nonewline $fd "SET x blabla\r\n"
        flush $fd
        gets $fd
    } {*invalid bulk*count*}

    test {Multi bulk request not followed by bulk args} {
        set fd [r channel]
        puts -nonewline $fd "*1\r\nfoo\r\n"
        flush $fd
        gets $fd
    } {*protocol error*}

    test {Generic wrong number of args} {
        catch {r ping x y z} err
        set _ $err
    } {*wrong*arguments*ping*}
}
