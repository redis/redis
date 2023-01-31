source tests/support/aofmanifest.tcl
set defaults { appendonly {yes} appendfilename {appendonly.aof} appenddirname {appendonlydir} aof-use-rdb-preamble {no} }
set server_path [tmpdir server.aof]

tags {"aof external:skip"} {
    # Specific test for a regression where internal buffers were not properly
    # cleaned after a child responsible for an AOF rewrite exited. This buffer
    # was subsequently appended to the new AOF, resulting in duplicate commands.
    start_server_aof [list dir $server_path] {
        set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
        set bench [open "|src/redis-benchmark -q -s [dict get $srv unixsocket] -c 20 -n 20000 incr foo" "r+"]

        wait_for_condition 100 1 {
            [$client get foo] > 0
        } else {
            # Don't care if it fails.
        }

        # Benchmark should be running by now: start background rewrite
        $client bgrewriteaof

        # Read until benchmark pipe reaches EOF
        while {[string length [read $bench]] > 0} {}

        waitForBgrewriteaof $client

        # Check contents of foo
        assert_equal 20000 [$client get foo]
    }

    # Restart server to replay AOF
    start_server_aof [list dir $server_path] {
        set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
        wait_done_loading $client
        assert_equal 20000 [$client get foo]
    }
}
