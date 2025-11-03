set server_path [tmpdir aof.rdb.fallback]

tags {aof} {
    # Stage 1: Create an RDB snapshot without AOF present.
    start_server [list overrides [list dir $server_path appendonly {no}] keep_persistence true] {
        test {Prepare dataset and force RDB SAVE} {
            r set foo bar
            r save
        }
    }

    # Sanity: Starting with AOF enabled, without fallback, should not load RDB.
    start_server [list overrides [list dir $server_path appendonly {yes}] keep_persistence true] {
        test {Without fallback, AOF enabled but missing: dataset is empty} {
            assert_equal 0 [r exists foo]
        }
    }

    # Remove any AOF artifacts to simulate missing AOF before fallback test
    catch { exec rm -rf [file join $server_path appendonlydir] }

    # Stage 2: Enable fallback and verify RDB is loaded and AOF is active.
    start_server [list overrides [list dir $server_path appendonly {yes} aof-load-rdb-on-startup {yes}] keep_persistence true] {
        test {With fallback, AOF enabled and missing: load RDB on startup} {
            assert_equal {bar} [r get foo]
        }
    }
}


