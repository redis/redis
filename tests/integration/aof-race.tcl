set defaults { appendonly {yes} appendfilename {appendonly.aof} }
set server_path [tmpdir server.aof]
set aof_path "$server_path/appendonly.aof"

proc start_server_aof {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    start_server [list overrides $config] $code
}

tags {"aof"} {
    # Specific test for a regression where internal buffers were not properly
    # cleaned after a child responsible for an AOF rewrite exited. This buffer
    # was subsequently appended to the new AOF, resulting in duplicate commands.
    start_server_aof [list dir $server_path] {
        set client [redis [srv host] [srv port]]
        set bench [open "|src/redis-benchmark -q -p [srv port] -c 20 -n 20000 incr foo" "r+"]
        after 100

        # Benchmark should be running by now: start background rewrite
        $client bgrewriteaof

        # Read until benchmark pipe reaches EOF
        while {[string length [read $bench]] > 0} {}

        # Check contents of foo
        assert_equal 20000 [$client get foo]
    }

    # Restart server to replay AOF
    start_server_aof [list dir $server_path] {
        set client [redis [srv host] [srv port]]
        assert_equal 20000 [$client get foo]
    }
}
