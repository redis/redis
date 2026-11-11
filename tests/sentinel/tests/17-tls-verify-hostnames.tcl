# Sentinel verifies the TLS certificate of every instance it connects to against
# the address it is known by (tls-verify-hostnames, default yes), in addition to
# CA chain validation. These links carry the auth-pass/auth-user credentials, so
# without the check any certificate issued by the trusted CA could impersonate a
# monitored master, replica or sentinel and capture them.
#
# Fixtures: the harness's instance certificate (tests/tls/server.crt) carries
# IP:127.0.0.1 in its SAN, so the default passes. tests/tls/redis.crt chains to
# the same CA but carries no SAN at all: switching an instance to it makes that
# instance's identity unverifiable.

if {$::tls} {
    source "../tests/includes/init-tests.tcl"

    set redis_crt [format "%s/../../tls/redis.crt" [pwd]]
    set redis_key [format "%s/../../tls/redis.key" [pwd]]
    set server_crt [format "%s/../../tls/server.crt" [pwd]]
    set server_key [format "%s/../../tls/server.key" [pwd]]

    # Drop an instance's client connections so the sentinels handshake again:
    # the certificate identity is checked only when a link is established.
    proc drop_instance_links {id} {
        catch {R $id client kill type normal}
        catch {R $id client kill type pubsub}
    }

    # Make a redis instance present a different certificate, then drop its
    # existing client connections so the sentinels handshake again.
    proc set_instance_cert {id crt key} {
        R $id config set tls-cert-file $crt tls-key-file $key
        drop_instance_links $id
    }

    # The entry of the replica listening on $port, as seen by sentinel $sid.
    proc replica_entry {sid port} {
        foreach replica [S $sid SENTINEL REPLICAS mymaster] {
            if {[dict get $replica port] == $port} { return $replica }
        }
        return {}
    }

    proc count_identity_warnings {sid} {
        set fd [open [file join "sentinel_${sid}" "log.txt"] r]
        set n [regexp -all -- {-tls-identity-mismatch} [read $fd]]
        close $fd
        return $n
    }

    # Sentinel has no CONFIG SET for tls-expected-peer-name (it is a server
    # option, not a sentinel one), so apply it through the config file and a
    # restart. An empty name clears it.
    proc sentinel_set_expected_peer_name {id name} {
        set cfgfile [file join "sentinel_${id}" "sentinel.conf"]
        set f [open $cfgfile r]
        set lines {}
        foreach line [split [read $f] "\n"] {
            if {![string match "tls-expected-peer-name*" $line]} { lappend lines $line }
        }
        close $f
        # Quote the value: a space-separated list is a single argument.
        if {$name ne ""} { lappend lines "tls-expected-peer-name \"$name\"" }
        set f [open $cfgfile w]
        puts $f [join $lines "\n"]
        close $f
        kill_instance sentinel $id
        restart_instance sentinel $id
    }

    set replica_id 1
    set replica_port [get_instance_attrib redis $replica_id port]

    test "(tls-verify-hostnames) Identity verification is on by default and the links are up" {
        # Earlier units may leave sentinel 0's loglevel anywhere (01-conf-update
        # cycles it down to "nothing"); the log assertions below need WARNING
        # events to reach the log file.
        S 0 SENTINEL CONFIG SET loglevel notice
        assert_equal {yes} [lindex [S 0 SENTINEL CONFIG GET tls-verify-hostnames] 1]
        wait_for_condition 100 100 {
            [dict get [S 0 SENTINEL MASTER mymaster] flags] eq "master" &&
            [replica_entry 0 $replica_port] ne {} &&
            ![string match "*s_down*" [dict get [replica_entry 0 $replica_port] flags]]
        } else {
            fail "Links not up although every certificate carries the instance address"
        }
    }

    test "(tls-verify-hostnames) An instance whose certificate lacks its address is flagged down" {
        set_instance_cert $replica_id $redis_crt $redis_key
        # Every sentinel now fails the handshake with this replica: it becomes
        # subjectively down, and the mismatch is logged as a WARNING-level event
        # naming the expected identity, not buried at DEBUG level.
        wait_for_condition 100 100 {
            [string match "*s_down*" [dict get [replica_entry 0 $replica_port] flags]]
        } else {
            fail "Replica not flagged s_down although its certificate lacks its address"
        }
        wait_for_condition 100 100 {
            [count_identity_warnings 0] > 0
        } else {
            # The log tail tells whether events reached the file at all (e.g. a
            # loglevel left behind by an earlier unit) or the mismatch never
            # happened.
            puts "--- sentinel 0 log tail:"
            puts [exec tail -20 sentinel_0/log.txt]
            fail "No -tls-identity-mismatch event logged by sentinel 0"
        }
        set fd [open "sentinel_0/log.txt" r]; set log [read $fd]; close $fd
        assert_match {*expected identity '127.0.0.1'*} $log
        assert_match {*sentinel tls-verify-hostnames no*} $log
    }

    test "(tls-verify-hostnames) Setting it to no accepts the certificate again" {
        S 0 SENTINEL CONFIG SET tls-verify-hostnames no
        assert_equal {no} [lindex [S 0 SENTINEL CONFIG GET tls-verify-hostnames] 1]
        wait_for_condition 100 100 {
            ![string match "*s_down*" [dict get [replica_entry 0 $replica_port] flags]]
        } else {
            fail "Replica still s_down on sentinel 0 with tls-verify-hostnames no"
        }
        # Control: sentinel 1 still has tls-verify-hostnames yes, so it must
        # still reject this replica's certificate. That shows the certificate is
        # still unacceptable, and that sentinel 0 accepts it now only because of
        # the setting we changed on sentinel 0.
        wait_for_condition 100 100 {
            [string match "*s_down*" [dict get [replica_entry 1 $replica_port] flags]]
        } else {
            fail "Sentinel 1, still verifying, does not flag the replica s_down"
        }
    }

    test "(tls-verify-hostnames) The setting survives a config rewrite and restart" {
        kill_instance sentinel 0
        restart_instance sentinel 0
        assert_equal {no} [lindex [S 0 SENTINEL CONFIG GET tls-verify-hostnames] 1]
        S 0 SENTINEL CONFIG SET tls-verify-hostnames yes
    }

    test "(tls-verify-hostnames) tls-expected-peer-name overrides the address check" {
        # With address verification back on, sentinel 0 rejects the replica again
        # -- once the link is re-established, since the identity is verified at
        # handshake time and the existing link was accepted under "no".
        drop_instance_links $replica_id
        wait_for_condition 100 100 {
            [string match "*s_down*" [dict get [replica_entry 0 $replica_port] flags]]
        } else {
            fail "Replica not flagged s_down after re-enabling tls-verify-hostnames"
        }
        # A configured expected name replaces the address check, and any listed
        # name may match. The harness certificate matches through its DNS SAN
        # (localhost); redis.crt has no DNS SAN at all, so OpenSSL falls back to
        # its Common Name (Generic-cert) -- which is why the same replica that
        # fails the address check passes here.
        sentinel_set_expected_peer_name 0 "localhost Generic-cert"
        wait_for_condition 100 100 {
            [dict get [S 0 SENTINEL MASTER mymaster] flags] eq "master" &&
            [replica_entry 0 $replica_port] ne {} &&
            ![string match "*s_down*" [dict get [replica_entry 0 $replica_port] flags]]
        } else {
            fail "Links not up with tls-expected-peer-name matching every certificate"
        }
    }

    test "(tls-verify-hostnames) Restore the instance certificate and defaults" {
        set_instance_cert $replica_id $server_crt $server_key
        sentinel_set_expected_peer_name 0 ""
        assert_equal {yes} [lindex [S 0 SENTINEL CONFIG GET tls-verify-hostnames] 1]
        foreach_sentinel_id id {
            wait_for_condition 100 100 {
                [dict get [S $id SENTINEL MASTER mymaster] flags] eq "master" &&
                [replica_entry $id $replica_port] ne {} &&
                ![string match "*s_down*" [dict get [replica_entry $id $replica_port] flags]]
            } else {
                fail "Sentinel $id links not back up after restoring the certificate"
            }
        }
    }
}
