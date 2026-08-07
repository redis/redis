start_server {tags {"tls"}} {
    if {$::tls} {
        package require tls

        test {TLS: Not accepting non-TLS connections on a TLS port} {
            set s [redis [srv 0 host] [srv 0 port]]
            catch {$s PING} e
            set e
        } {*I/O error*}

        test {TLS: Verify tls-auth-clients behaves as expected} {
            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e

            r CONFIG SET tls-auth-clients no

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients optional

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients yes

            set s [redis [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e
        }

        test {TLS: Verify tls-protocols behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-tls1.2 0}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-tls1.2 1}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
        }

        test {TLS: Verify tls-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "DEFAULT:-AES128-SHA256"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES256-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-ciphers "DEFAULT"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }

        test {TLS: Verify tls-prefer-server-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "AES128-SHA256:AES256-SHA256"

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES256-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-prefer-server-ciphers yes

            set s [redis [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES128-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }

        test {TLS: Verify tls-cert-file is also used as a client cert if none specified} {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            # Use a non-restricted client/server cert for the replica
            set redis_crt [format "%s/tests/tls/redis.crt" [pwd]]
            set redis_key [format "%s/tests/tls/redis.key" [pwd]]

            start_server [list overrides [list tls-cert-file $redis_crt tls-key-file $redis_key] \
                               omit [list tls-client-cert-file tls-client-key-file]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                wait_for_condition 30 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Can't authenticate to master using just tls-cert-file!"
                }
            }
        }

        test {TLS: switch between tcp and tls ports} {
            set srv_port [srv 0 port]

            # TLS
            set rd [redis [srv 0 host] $srv_port 0 1]
            $rd PING

            # TCP
            $rd CONFIG SET tls-port 0
            $rd CONFIG SET port $srv_port
            $rd close

            set rd [redis [srv 0 host] $srv_port 0 0]
            $rd PING

            # TLS
            $rd CONFIG SET port 0
            $rd CONFIG SET tls-port $srv_port
            $rd close

            set rd [redis [srv 0 host] $srv_port 0 1]
            $rd PING
            $rd close
        }

        test {TLS: Working with an encrypted keyfile} {
            # Create an encrypted version
            set keyfile [lindex [r config get tls-key-file] 1]
            set keyfile_encrypted "$keyfile.encrypted"
            exec -ignorestderr openssl rsa -in $keyfile -out $keyfile_encrypted -aes256 -passout pass:1234 2>/dev/null

            # Using it without a password fails
            catch {r config set tls-key-file $keyfile_encrypted} e
            assert_match {*Unable to update TLS*} $e

            # Now use a password
            r config set tls-key-file-pass 1234
            r config set tls-key-file $keyfile_encrypted
        }

	    test {TLS: Auto-authenticate using tls-auth-clients-user (CN)} {
	        # Create a user matching the CN in the client certificate (CN=Client-only)
	        r ACL SETUSER {Client-only} on >clientpass allcommands allkeys

	        # Map the client certificate CN to the ACL user name.
	        r CONFIG SET tls-auth-clients-user CN

	        # Connect over TLS using the test client certificate (CN=Client-only)
	        set s [redis [srv 0 host] [srv 0 port] 0 1]
	        catch {$s PING} e
	        assert_match {PONG} $e
	        assert_equal "Client-only" [$s ACL WHOAMI]
	    }

	    foreach user_type {"non-existent" "disabled"} {
	        test "TLS: $user_type user cannot auto-authenticate via certificate" {
	            if {$user_type eq "non-existent"} {
	                # Ensure the Client-only user does not exist so auto-auth will fail
	                catch {r ACL DELUSER {Client-only}}
	            } else {
	                r ACL SETUSER {Client-only} on >clientpass allcommands allkeys
	                r ACL SETUSER {Client-only} off  ;# Disable the user
	            }
	            r ACL LOG RESET
	            r CONFIG SET tls-auth-clients-user CN

	            # Capture the current value of acl_access_denied_tls_cert from INFO stats
	            set info_before [r INFO stats]
	            regexp {acl_access_denied_tls_cert:(\d+)} $info_before -> before

	            # Connect over TLS using the test client certificate (CN=Client-only)
	            # Since there is no matching ACL user or user is disabled, auto-auth should fail
	            # and the connection should remain authenticated as the default user
	            set s [redis [srv 0 host] [srv 0 port] 0 1]
	            assert_equal "default" [$s ACL WHOAMI]

	            # The ACL LOG should contain a single entry with reason "tls-cert"
	            # and username "Client-only"
	            set log [r ACL LOG]
	            assert_equal 1 [llength $log]
	            set entry [lindex $log 0]
	            assert_equal "tls-cert" [dict get $entry reason]
	            assert_equal "Client-only" [dict get $entry username]

	            # INFO stats should report that acl_access_denied_tls_cert increased by 1
	            set info_after [r INFO stats]
	            regexp {acl_access_denied_tls_cert:(\d+)} $info_after -> after
	            assert {$after == $before + 1}

	            # Verify fallback to password auth works after cert auth fails
	            r ACL SETUSER testuser on >testpass +@all ~*
	            $s AUTH testuser testpass
	            assert_equal "testuser" [$s ACL WHOAMI]
	            assert_equal "PONG" [$s PING]

	            # Clean up
	            r ACL DELUSER testuser
	            catch {r ACL DELUSER {Client-only}}
	        }
	    }
    }
}

# tls-expected-peer-name certificate name verification.
#
# These tests share a single top-level server that presents a certificate whose
# SAN contains redis.local / cluster.local (tests/tls/san.crt) and acts as the
# replication master / MIGRATE target. Each test nests only the replica (or
# MIGRATE source) it needs, so the top-level server is actually used rather than
# being an idle wrapper.
if {$::tls} {
    set san_crt [format "%s/tests/tls/san.crt" [pwd]]
    set san_key [format "%s/tests/tls/san.key" [pwd]]
    start_server [list tags {"tls"} overrides [list tls-cert-file $san_crt tls-key-file $san_key]] {
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {TLS: tls-expected-peer-name accepts a matching certificate SAN} {
            start_server [list overrides [list tls-expected-peer-name "redis.local"]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Replication link did not come up with a matching tls-expected-peer-name"
                }
            }
        }

        test {TLS: tls-expected-peer-name rejects a mismatching certificate SAN} {
            start_server [list overrides [list tls-expected-peer-name "wrong.example.com"]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                # Wait for the connection attempt to resolve one way or the other:
                # either the link comes up (which would be a bug) or the replica logs
                # the TLS certificate verification failure. Anchoring on the failure
                # log lets us conclude quickly instead of waiting out a fixed window.
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]] ||
                    [count_log_message 0 "certificate verify failed"] > 0
                } else {
                    fail "Replica never resolved its connection attempt (neither up nor errored)"
                }
                # The peer cert has no matching SAN, so the link must not be up.
                assert_no_match {*master_link_status:up*} [$replica info replication]
            }
        }

        test {TLS: tls-expected-peer-name matches any of multiple listed names} {
            # Start with a single non-matching name so the link stays down; then set a
            # space-separated list via CONFIG SET (a single argument, so it is not split)
            # whose second entry (cluster.local) is present in the cert SAN.
            start_server [list overrides [list tls-expected-peer-name "absent.example.com"]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]] ||
                    [count_log_message 0 "certificate verify failed"] > 0
                } else {
                    fail "Replica never resolved its connection attempt (neither up nor errored)"
                }
                assert_no_match {*master_link_status:up*} [$replica info replication]
                $replica config set tls-expected-peer-name "absent.example.com cluster.local"
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Replication link did not come up when the cert matched the second listed name"
                }
            }
        }

        test {TLS: tls-expected-peer-name takes effect on CONFIG SET at runtime} {
            start_server [list overrides [list tls-expected-peer-name "wrong.example.com"]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                # Wrong name: the link stays down and the replica logs a verify failure.
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]] ||
                    [count_log_message 0 "certificate verify failed"] > 0
                } else {
                    fail "Replica never resolved its connection attempt (neither up nor errored)"
                }
                assert_no_match {*master_link_status:up*} [$replica info replication]
                # Correct the expected name at runtime; the next reconnect should succeed.
                $replica config set tls-expected-peer-name "redis.local"
                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Replication link did not recover after correcting tls-expected-peer-name at runtime"
                }
            }
        }

        test {TLS: tls-expected-peer-name is enforced on MIGRATE (blocking connect)} {
            # MIGRATE opens its target via the blocking connect path, which must apply
            # the same peer-name verification as the async connect path. The shared
            # server is the MIGRATE target (it presents the SAN cert).
            start_server [list overrides [list tls-expected-peer-name "wrong.example.com"]] {
                set src [srv 0 client]
                $src set k1 v1
                # Mismatch: the TLS handshake to the target fails, so MIGRATE errors
                # and the key is not moved. (A failed connect is not cached.)
                catch {$src migrate $master_host $master_port k1 9 5000} e
                assert_match {*IOERR*} $e
                assert_equal 1 [$src exists k1]

                # Correct the expected name: a fresh connection verifies and succeeds.
                $src config set tls-expected-peer-name "redis.local"
                assert_equal {OK} [$src migrate $master_host $master_port k1 9 5000]
                assert_equal 0 [$src exists k1]
            }
        }

        test {TLS: tls-expected-peer-name can be set and cleared} {
            # Round-trip through the config's validation (isValidTlsExpectedPeerName):
            # a name is accepted, and an empty value clears it (the validator receives
            # "" before the empty-to-null conversion and always allows clearing).
            r config set tls-expected-peer-name node.cluster.local
            assert_equal {node.cluster.local} [lindex [r config get tls-expected-peer-name] 1]
            r config set tls-expected-peer-name ""
            assert_equal {} [lindex [r config get tls-expected-peer-name] 1]
        }

        test {TLS: tls-expected-peer-name rejects a whitespace-only value} {
            # A value with no usable name (only spaces) would otherwise be stored
            # and then refuse every connection; it must be rejected at config time.
            catch {r config set tls-expected-peer-name "   "} e
            assert_match {*no usable name*} $e
            # Tabs/other whitespace are not name separators (only spaces are), so a
            # tab-only value, or whitespace embedded in a name, would be passed to
            # OpenSSL verbatim and silently match nothing. Reject those too.
            catch {r config set tls-expected-peer-name "\t"} e
            assert_match {*whitespace*} $e
            catch {r config set tls-expected-peer-name "redis.local\tcluster.local"} e
            assert_match {*whitespace*} $e
            # A real name is still accepted; clear afterwards.
            r config set tls-expected-peer-name node.cluster.local
            assert_equal {node.cluster.local} [lindex [r config get tls-expected-peer-name] 1]
            r config set tls-expected-peer-name ""
        }
    }
}
