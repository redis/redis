#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2025-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

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
        # (it seems that rediscli_exec behaves differently in RESP3, possibly
        # because CONFIG GET returns a dict instead of a list so redis-cli emits
        # it in a single line)
        if {$::force_resp3} {
            assert_equal {{bind }} [rediscli_exec 0 config get bind]
        } else {
            assert_equal {bind {}} [rediscli_exec 0 config get bind]
        }
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

start_server {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes} io-threads 2}} {
    set server_pid [s process_id]
    # Since each thread may perform memory prefetch independently, this test is
    # only run when the number of IO threads is 2 to ensure deterministic results.
    if {[r config get io-threads] eq "io-threads 2"} {
        test {prefetch works as expected when killing a client from the middle of prefetch commands batch} {
            # Create 16 (prefetch batch size) +1 clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [redis_deferring_client]
            }

            # set a key that will be later be prefetch
            r set a 0

            # Get the client ID of rd4
            $rd4 client id
            set rd4_id [$rd4 read]

            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid

            # The first client will kill the fourth client
            $rd0 client kill id $rd4_id

            # Send set commands for all clients except the first
            for {set i 1} {$i < 16} {incr i} {
                [set rd$i] set $i $i
                [set rd$i] flush
            }

            # Resume the server
            resume_process $server_pid

            # Read the results
            assert_equal {1} [$rd0 read]
            catch {$rd4 read} res
            if {$res eq "OK"} {
                # maybe OK then err, we can not control the order of execution
                catch {$rd4 read} err
            } else {
                set err $res
            }
            assert_match {I/O error reading reply} $err

            # verify the prefetch stats are as expected
            set info [r info stats]
            set prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            assert_range $prefetch_entries 2 15; # With slower machines, the number of prefetch entries can be lower
            set prefetch_batches [getInfoProperty $info io_threaded_total_prefetch_batches]
            assert_range $prefetch_batches 1 7; # With slower machines, the number of batches can be higher

            # verify other clients are working as expected
            for {set i 1} {$i < 16} {incr i} {
                if {$i != 4} { ;# 4th client was killed
                    [set rd$i] get $i
                    assert_equal {OK} [[set rd$i] read]
                    assert_equal $i [[set rd$i] read]
                }
            }
        }

        test {prefetch works as expected when changing the batch size while executing the commands batch} {
            # Create 16 (default prefetch batch size) clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [redis_deferring_client]
            }

            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid

            # Send set commands for all clients the 5th client will change the prefetch batch size
            for {set i 0} {$i < 16} {incr i} {
                if {$i == 4} {
                    [set rd$i] config set prefetch-batch-max-size 1
                }
                [set rd$i] set a $i
                [set rd$i] flush
            }
            # Resume the server
            resume_process $server_pid
            # Read the results
            for {set i 0} {$i < 16} {incr i} {
                assert_equal {OK} [[set rd$i] read]
                [set rd$i] close
            }

            # assert the configured prefetch batch size was changed
            assert {[r config get prefetch-batch-max-size] eq "prefetch-batch-max-size 1"}
        }
 
        proc do_prefetch_batch {server_pid batch_size} {
            # Create clients
            for {set i 0} {$i < $batch_size} {incr i} {
                set rd$i [redis_deferring_client]
            }

            # Suspend the server to batch the commands
            pause_process $server_pid

            # Send commands from all clients
            for {set i 0} {$i < $batch_size} {incr i} {
                [set rd$i] set a $i
                [set rd$i] flush
            }

            # Resume the server to process the batch
            resume_process $server_pid

            # Verify responses
            for {set i 0} {$i < $batch_size} {incr i} {
                assert_equal {OK} [[set rd$i] read]
                [set rd$i] close
            }
        }

        test {no prefetch when the batch size is set to 0} {
            # set the batch size to 0
            r config set prefetch-batch-max-size 0
            # save the current value of prefetch entries
            set info [r info stats]
            set prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]

            do_prefetch_batch $server_pid 16

            # assert the prefetch entries did not change
            set info [r info stats]
            set new_prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            assert_equal $prefetch_entries $new_prefetch_entries
        }

        test {Prefetch can resume working when the configuration option is set to a non-zero value} {
            # save the current value of prefetch entries
            set info [r info stats]
            set prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            # set the batch size to 0
            r config set prefetch-batch-max-size 16

            do_prefetch_batch $server_pid 16

            # assert the prefetch entries did not change
            set info [r info stats]
            set new_prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            # With slower machines, the number of prefetch entries can be lower
            assert_range $new_prefetch_entries [expr {$prefetch_entries + 2}] [expr {$prefetch_entries + 16}]
        }
    }
}

start_server {tags {"timeout external:skip"}} {
    test {Multiple clients idle timeout test} {
        # set client timeout to 1 second
        r config set timeout 1

        # create multiple client connections
        set clients {}
        set num_clients 10

        for {set i 0} {$i < $num_clients} {incr i} {
            set client [redis_deferring_client]
            $client ping
            assert_equal "PONG" [$client read]
            lappend clients $client
        }
        assert_equal [llength $clients] $num_clients

        # wait for 2.5 seconds
        after 2500

        # try to send commands to all clients - they should all fail due to timeout
        set disconnected_count 0
        foreach client $clients {
            $client ping
            if {[catch {$client read} err]} {
                incr disconnected_count
                # expected error patterns for connection timeout
                assert_match {*I/O error*} $err
            }
            catch {$client close}
        }

        # all clients should have been disconnected due to timeout
        assert_equal $disconnected_count $num_clients

        # redis server still works well
        reconnect
        assert_equal "PONG" [r ping]
    }
}
