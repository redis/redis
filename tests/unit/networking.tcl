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

# Attempt to connect to host using a client bound to bindaddr,
# and return a non-zero value if successful within specified
# millisecond timeout, or zero otherwise.
proc test_loopback {host bindaddr timeout} {
    if {[exec uname] != {Linux}} {
        return 0
    }

    after $timeout set ::test_loopback_state timeout
    if {[catch {
        set server_sock [socket -server accept 0]
        set port [lindex [fconfigure $server_sock -sockname] 2] } err]} {
            return 0
    }

    proc accept {channel clientaddr clientport} {
        set ::test_loopback_state "connected"
        close $channel
    }

    if {[catch {set client_sock [socket -async -myaddr $bindaddr $host $port]} err]} {
        puts "test_loopback: Client connect failed: $err"
    } else {
        close $client_sock
    }

    vwait ::test_loopback_state
    close $server_sock

    return [expr {$::test_loopback_state == {connected}}]
}

test {CONFIG SET bind-source-addr} {
    if {[test_loopback 127.0.0.1 127.0.0.2 1000]} {
        start_server {} {
            start_server {} {
                set replica [srv 0 client]
                set master [srv -1 client]

                $master config set protected-mode no

                $replica config set bind-source-addr 127.0.0.2
                $replica replicaof [srv -1 host] [srv -1 port]

                wait_for_condition 50 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "Replication not started."
                }

                assert_match {*ip=127.0.0.2*} [s -1 slave0]
            }
        }
    } else {
        if {$::verbose} { puts "Skipping bind-source-addr test." }
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

    test {Protected mode works as expected} {
        # Get a non-loopback address of this instance for this test.
        set myaddr [get_nonloopback_addr]
        if {$myaddr != "" && ![string match {127.*} $myaddr]} {
            # Non-loopback client should fail by default
            set r2 [get_nonloopback_client]
            catch {$r2 ping} err
            assert_match {*DENIED*} $err

            # Bind configuration should not matter
            assert_equal {OK} [r config set bind "*"]
            set r2 [get_nonloopback_client]
            catch {$r2 ping} err
            assert_match {*DENIED*} $err

            # Setting a password should disable protected mode
            assert_equal {OK} [r config set requirepass "secret"]
            set r2 [redis $myaddr [srv 0 "port"] 0 $::tls]
            assert_equal {OK} [$r2 auth secret]
            assert_equal {PONG} [$r2 ping]

            # Clearing the password re-enables protected mode
            assert_equal {OK} [r config set requirepass ""]
            set r2 [redis $myaddr [srv 0 "port"] 0 $::tls]
            assert_match {*DENIED*} $err

            # Explicitly disabling protected-mode works
            assert_equal {OK} [r config set protected-mode no]
            set r2 [redis $myaddr [srv 0 "port"] 0 $::tls]
            assert_equal {PONG} [$r2 ping]
        }
    }
}
