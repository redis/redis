source tests/support/cli.tcl

test {CONFIG SET port number} {
    start_server {} {
        if {$::tls} { set port_cfg tls-port} else { set port_cfg port }

        # available port
        set avail_port [find_available_port $::baseport $::portcount]
        set rd [redis [srv 0 host] [srv 0 port] 0 $::tls]
        $rd CONFIG SET $port_cfg $avail_port
        $rd close
        set rd [redis [srv 0 host] $avail_port 0 $::tls]
        $rd PING

        # already inuse port
        catch {$rd CONFIG SET $port_cfg $::test_server_port} e
        assert_match {*Unable to listen on this port*} $e
        $rd close

        # make sure server still listening on the previous port
        set rd [redis [srv 0 host] $avail_port 0 $::tls]
        $rd PING
        $rd close
    }
} {} {external:skip}

test {CONFIG SET bind address} {
    start_server {} {
        # non-valid address
        catch {r CONFIG SET bind "999.999.999.999"} e
        assert_match {*Failed to bind to specified addresses*} $e

        # make sure server still bound to the previous address
        set rd [redis [srv 0 host] [srv 0 port] 0 $::tls]
        $rd PING
        $rd close
    }
} {} {external:skip}

start_server {config "minimal.conf" tags {"external:skip"}} {
    test {Default bind address configuration handling} {
        # Default is explicit and sane
        assert_equal "* -::*" [lindex [r CONFIG GET bind] 1]

        # CONFIG REWRITE acknowledges this as a default
        r CONFIG REWRITE
        assert_equal 0 [count_message_lines [srv 0 config_file] bind]

        # Removing the bind address works
        r CONFIG SET bind ""
        assert_equal "" [lindex [r CONFIG GET bind] 1]

        # No additional clients can connect
        catch {redis_client} err
        assert_match {*connection refused*} $err

        # CONFIG REWRITE handles empty bindaddr
        r CONFIG REWRITE
        assert_equal 1 [count_message_lines [srv 0 config_file] bind]

        # Make sure we're able to restart
        restart_server 0 0 0 0

        # Make sure bind parameter is as expected and server handles binding
        # accordingly.
        assert_equal {bind {}} [rediscli_exec 0 config get bind]
        catch {reconnect 0} err
        assert_match {*connection refused*} $err

        assert_equal {OK} [rediscli_exec 0 config set bind *]
        reconnect 0
        r ping
    } {PONG}
}
