# Links to other Sentinels are shared between the masters that know them, so
# the TLS identity check must not hold on to the instance that negotiated a
# connection: SENTINEL REMOVE of that instance's master releases it while the
# handshake may still be pending, with the link kept alive by another master's
# instance. The OpenSSL verify callback used to keep a pointer to that instance
# and dereferenced it after the release, a use-after-free that AddressSanitizer
# reports. It now records the mismatch on the link, and the reconnect path
# reports it through whichever instance still holds the link.
#
# The pending handshake is produced with a fake Sentinel: a plain TCP acceptor
# that holds the connections it receives without speaking TLS, then completes
# them on demand with a certificate lacking the dialed address
# (tests/tls/redis.crt). Sentinel 1 announces the fake's port, so the other
# sentinels dial the fake believing it is sentinel 1.

if {$::tls} {
    source "../tests/includes/init-tests.tcl"

    # The fake sentinel. Accepted connections are held while ::fake_hold is set
    # and closed at once afterwards.
    set ::fake_conns {}
    set ::fake_hold 1
    proc fake_accept {chan addr port} {
        fconfigure $chan -translation binary -blocking 0
        if {$::fake_hold} {
            lappend ::fake_conns $chan
        } else {
            catch {close $chan}
        }
    }
    set fake_server [socket -server fake_accept -myaddr 127.0.0.1 0]
    set fake_port [lindex [fconfigure $fake_server -sockname] 2]

    # Without a callback the tls package prints every failed handshake to
    # stderr, and failing is what the handshakes below are meant to do.
    proc fake_tls_callback {args} { return 1 }

    # wait_for_condition sleeps without servicing the event loop, which the
    # accept callback above needs; this variant does.
    proc wait_for_condition_servicing_events {maxtries delay e _else elsescript} {
        while {[incr maxtries -1] >= 0} {
            update
            if {[uplevel 1 [list expr $e]]} return
            after $delay
        }
        uplevel 1 $elsescript
    }

    # Sentinel $sid's entry for the sentinel with run ID $runid under $master,
    # or {} if unknown.
    proc sentinel_entry {sid master runid} {
        foreach s [S $sid SENTINEL SENTINELS $master] {
            if {[dict get $s runid] eq $runid} { return $s }
        }
        return {}
    }

    proc count_log_matches {sid pattern} {
        set fd [open [file join "sentinel_${sid}" "log.txt"] r]
        set n [regexp -all -- $pattern [read $fd]]
        close $fd
        return $n
    }

    set s1_runid [S 1 SENTINEL MYID]
    set master_host [get_instance_attrib redis 0 host]
    set master_port [get_instance_attrib redis 0 port]
    set second_host [get_instance_attrib redis 4 host]
    set second_port [get_instance_attrib redis 4 port]

    test "(shared link) Sentinel 0 logs at NOTICE level" {
        # Earlier units may leave the loglevel anywhere.
        S 0 SENTINEL CONFIG SET loglevel notice
    }

    test "(shared link) The other sentinels dial sentinel 1 at the fake's port" {
        S 1 SENTINEL CONFIG SET announce-port $fake_port
        # Each of the four other sentinels switches sentinel 1's address and
        # connects to the fake, where its TLS handshake stays pending.
        wait_for_condition_servicing_events 200 100 {
            [llength $::fake_conns] >= 4
        } else {
            fail "the other sentinels did not connect to the fake sentinel"
        }
        set e [sentinel_entry 0 mymaster $s1_runid]
        assert {$e ne {}}
        assert_equal $fake_port [dict get $e port]
    }

    test "(shared link) A second master shares sentinel 0's pending link to sentinel 1" {
        # Redis 4 becomes a master of its own, monitored by sentinels 0 and 1.
        R 4 replicaof no one
        S 1 SENTINEL MONITOR second $second_host $second_port 2
        S 0 SENTINEL MONITOR second $second_host $second_port 2
        # Sentinel 1's hello messages on the new master carry the fake's port;
        # sentinel 0 learns it there and shares the link it already holds for
        # the same run ID under mymaster, the one with the pending handshake.
        wait_for_condition_servicing_events 200 100 {
            [sentinel_entry 0 second $s1_runid] ne {} &&
            [dict get [sentinel_entry 0 second $s1_runid] link-refcount] == 2
        } else {
            fail "sentinel 0 did not share its link to sentinel 1 with the second master"
        }
    }

    test "(shared link) Removing the master whose instance negotiated the connection keeps the link" {
        S 0 SENTINEL REMOVE mymaster
        assert_equal 1 [dict get [sentinel_entry 0 second $s1_runid] link-refcount]
    }

    test "(shared link) A mismatch after the release is reported through the surviving instance" {
        # Complete every held handshake as a server presenting redis.crt, which
        # chains to the CA but carries no address, so each client rejects it
        # with an identity mismatch. Sentinel 0's verify callback runs now,
        # after the instance that negotiated its connection has been freed.
        set ::fake_hold 0
        foreach chan $::fake_conns {
            catch {
                fconfigure $chan -blocking 1
                tls::import $chan -server 1 -require 0 -request 0 \
                    -command fake_tls_callback \
                    -cafile "$::tlsdir/ca.crt" \
                    -certfile "$::tlsdir/redis.crt" \
                    -keyfile "$::tlsdir/redis.key"
                tls::handshake $chan
            }
            catch {close $chan}
        }
        set ::fake_conns {}

        # Sentinel 0 is alive -- without the fix an AddressSanitizer build dies
        # here -- and, on its next reconnection attempt, reports the mismatch
        # naming the second master, holder of the surviving instance.
        assert_equal PONG [S 0 PING]
        wait_for_condition_servicing_events 100 100 {
            [count_log_matches 0 {-tls-identity-mismatch sentinel [^\n]* @ second }] > 0
        } else {
            fail "sentinel 0 did not report the identity mismatch through the surviving instance"
        }
        assert_equal PONG [S 0 PING]
    }

    test "(shared link) Cleanup" {
        close $fake_server
        S 1 SENTINEL CONFIG SET announce-port 0
        catch {S 0 SENTINEL REMOVE second}
        catch {S 1 SENTINEL REMOVE second}
        R 4 replicaof $master_host $master_port
    }
}
