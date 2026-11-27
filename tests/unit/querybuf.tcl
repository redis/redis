#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#
proc client_idle_sec {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    assert {[regexp {idle=([0-9]+)} $c - idle]}
    return $idle
}

# Calculate query buffer memory of client
proc client_query_buffer {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    if {[string length $c] > 0} {
        assert {[regexp {qbuf=([0-9]+)} $c - qbuf]}
        assert {[regexp {qbuf-free=([0-9]+)} $c - qbuf_free]}
        return [expr $qbuf + $qbuf_free]
    }
    return 0
}

start_server {tags {"querybuf slow"}} {
    # increase the execution frequency of clientsCron
    r config set hz 100

    # The test will run at least 2s to check if client query
    # buffer will be resized when client idle 2s.
    test "query buffer resized correctly" {

        set rd [redis_deferring_client]

        $rd client setname test_client
        $rd read

        # Make sure query buff has size of 0 bytes at start as the client uses the reusable qb.
        assert {[client_query_buffer test_client] == 0}

        # Pause cron to prevent premature shrinking (timing issue).
        r debug pause-cron 1

        # Send partial command to client to make sure it doesn't use the reusable qb.
        $rd write "*3\r\n\$3\r\nset\r\n\$2\r\na"
        $rd flush
        # Wait for the client to start using a private query buffer. 
        wait_for_condition 1000 10 {
            [client_query_buffer test_client] > 0
        } else {
            fail "client should start using a private query buffer"
        }
     
        # send the rest of the command
        $rd write "a\r\n\$1\r\nb\r\n"
        $rd flush
        assert_equal {OK} [$rd read]

        set orig_test_client_qbuf [client_query_buffer test_client]
        # Make sure query buff has less than the peak resize threshold (PROTO_RESIZE_THRESHOLD) 32k
        # but at least the basic IO reading buffer size (PROTO_IOBUF_LEN) 16k
        set MAX_QUERY_BUFFER_SIZE [expr 32768 + 2] ; # 32k + 2, allowing for potential greedy allocation of (16k + 1) * 2 bytes for the query buffer.
        assert {$orig_test_client_qbuf >= 16384 && $orig_test_client_qbuf <= $MAX_QUERY_BUFFER_SIZE}

        # Allow shrinking to occur
        r debug pause-cron 0

        # Check that the initial query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] < $orig_test_client_qbuf
        } else {
            fail "query buffer was not resized"
        }
        $rd close
    }

    test "query buffer resized correctly when not idle" {
        # Pause cron to prevent premature shrinking (timing issue).
        r debug pause-cron 1

        # Memory will increase by more than 32k due to client query buffer.
        set rd [redis_client]
        $rd client setname test_client

        # Create a large query buffer (more than PROTO_RESIZE_THRESHOLD - 32k)
        $rd set x [string repeat A 400000]

        # Make sure query buff is larger than the peak resize threshold (PROTO_RESIZE_THRESHOLD) 32k
        set orig_test_client_qbuf [client_query_buffer test_client]
        assert {$orig_test_client_qbuf > 32768}

        r debug pause-cron 0

        # Wait for qbuf to shrink due to lower peak
        set t [clock milliseconds]
        while true {
            # Write something smaller, so query buf peak can shrink
            $rd set x [string repeat A 100]
            set new_test_client_qbuf [client_query_buffer test_client]
            if {$new_test_client_qbuf < $orig_test_client_qbuf} { break } 
            if {[expr [clock milliseconds] - $t] > 1000} { break }
            after 10
        }
        # Validate qbuf shrunk but isn't 0 since we maintain room based on latest peak
        assert {[client_query_buffer test_client] > 0 && [client_query_buffer test_client] < $orig_test_client_qbuf}
        $rd close
    } {0} {needs:debug}

    test "query buffer resized correctly with fat argv" {
        set rd [redis_client]
        $rd client setname test_client

        # Pause cron to prevent premature shrinking (timing issue).
        r debug pause-cron 1

        $rd write "*3\r\n\$3\r\nset\r\n\$1\r\na\r\n\$1000000\r\n"
        $rd flush

        # Wait for the client to start using a private query buffer of > 1000000 size.
        wait_for_condition 1000 10 {
            [client_query_buffer test_client] > 1000000
        } else {
            fail "client should start using a private query buffer"
        }
        
        # Send the start of the arg and make sure the client is not using reusable qb for it rather a private buf of > 1000000 size.
        $rd write "a" 
        $rd flush

        r debug pause-cron 0

        after 120
        if {[client_query_buffer test_client] < 1000000} {
            fail "query buffer should not be resized when client idle time smaller than 2s"
        }
     
        # Check that the query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] < 1000000
        } else {
            fail "query buffer should be resized when client idle time bigger than 2s"
        }
     
        $rd close
    }
}

start_server {tags {"querybuf"}} {
    test "Client executes small argv commands using reusable query buffer" {
        set rd [redis_deferring_client]
        $rd client setname test_client
        $rd read
        set res [r client list]

        # Verify that the client does not create a private query buffer after
        # executing a small parameter command.
        assert_match {*name=test_client * qbuf=0 qbuf-free=0 * cmd=client|setname *} $res 

        # The client executing the command is currently using the reusable query buffer,
        # so the size shown is that of the reusable query buffer. It will be returned
        # to the reusable query buffer after command execution.
        # Note that if IO threads are enabled, the reusable query buffer will be dereferenced earlier.
        if {[lindex [r config get io-threads] 1] == 1} {
            assert_match {*qbuf=26 qbuf-free=* cmd=client|list *} $res
        } else {
            assert_match {*qbuf=0 qbuf-free=* cmd=client|list *} $res
        }

        $rd close
    } 
}

start_server {tags {"querybuf"}} {
    # The query buffer limit also covers commands that are queued inside a
    # MULTI but not executed yet, since they hold memory just the same. The
    # check lives in readQueryFromClient() and sums mstate.argv_len_sums with
    # the current query buffer length.
    test "query buffer limit accounts for queued MULTI commands" {
        set org_limit [lindex [r config get client-query-buffer-limit] 1]
        r config resetstat
        r config set client-query-buffer-limit 1048576

        set rd [redis_client]
        $rd client setname multi_qbuf

        # Each queued command is only ~100kb, well under the 1mb limit on its
        # own, so a disconnect here can only come from the queued commands
        # being accounted for together.
        set payload [string repeat A 102400]
        $rd multi
        set queued 0
        catch {
            for {set i 0} {$i < 20} {incr i} {
                $rd set key:$i{t} $payload
                incr queued
            }
        } e

        # The client is killed with freeClientAsync() and gets no error reply,
        # so the write simply fails once the server is gone.
        assert {$queued < 20}

        # Make sure it was the query buffer limit that killed it, and not the
        # output buffer limit or anything else.
        set info [r info stats]
        assert_equal [getInfoProperty $info client_query_buffer_limit_disconnections] {1}

        catch {$rd close}
        r config set client-query-buffer-limit $org_limit
    }

    test "query buffer limit is not reached by a MULTI below the limit" {
        set org_limit [lindex [r config get client-query-buffer-limit] 1]
        r config resetstat
        r config set client-query-buffer-limit 1048576

        set rd [redis_client]
        set payload [string repeat A 102400]
        $rd multi
        for {set i 0} {$i < 5} {incr i} {
            $rd set key:$i{t} $payload
        }
        assert_equal [llength [$rd exec]] 5

        set info [r info stats]
        assert_equal [getInfoProperty $info client_query_buffer_limit_disconnections] {0}

        $rd close
        r config set client-query-buffer-limit $org_limit
    }
}
