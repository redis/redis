test {CONFIG SET port number} {
    start_server {} {
        # available port
        set avail_port [find_available_port $::baseport $::portcount]
        set rd [redis [srv 0 host] [srv 0 port] 0 0]
        $rd CONFIG SET port $avail_port
        $rd close
        set rd [redis [srv 0 host] $avail_port 0 0]
        $rd PING

        # already inuse port
        catch {$rd CONFIG SET port $::test_server_port} e
        assert_match {*Unable to listen on this port*} $e
        $rd close

        # make sure server still listening on the previous port
        set rd [redis [srv 0 host] $avail_port 0 0]
        $rd PING
        $rd close
    }
}

test {CONFIG SET bind address} {
    start_server {} {
        # non-valid address
        catch {r CONFIG SET bind "some.wrong.bind.address"} e
        assert_match {*Failed to bind to specified addresses*} $e

        # make sure server still bound to the previous address
        set rd [redis [srv 0 host] [srv 0 port] 0 0]
        $rd PING
        $rd close
    }
}