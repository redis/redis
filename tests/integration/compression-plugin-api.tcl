set testmodule [file normalize src/compression_plugins/lzf_plugin_sample/libcompression_lzf.so]

set server_path [tmpdir "server.rdb-compressionplugin-string"]
test {Test string compress/decompress data with compression plugin before/after RDB save and load} {
  start_server [list overrides [list "dir" $server_path "loadcompression" "$testmodule default"]] {
    r set key1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
    assert_equal "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" [r get key1]
    r debug reload
    assert_equal "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" [r get key1]
    r save
  }
}

# Helper function to start a server and kill it, just to check the error
# logged.
set defaults {}
proc start_server_and_kill_it {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    set srv [start_server [list overrides $config]]
    uplevel 1 $code
    kill_server $srv
}

test {Test RDB load without the correct compression plugin loaded at Redis startup} {
  start_server_and_kill_it [list "dir" $server_path] {
    wait_for_condition 50 100 {
            [string match {*not found.*} [exec tail -20 < [dict get $srv stdout]]]
        } else {
            puts $[dict get $srv stdout]
            fail "Server started even if RDB was corrupted!"
        }
  }
}

set server_path [tmpdir "server.rdb-compressionplugin-quicklist"]
#test quicklist encoding with plugin before and after RDB reload
start_server [list overrides [list "dir" $server_path "loadcompression" "$testmodule default" "list-compress-depth" 1 "list-max-ziplist-size" 5]] {
    test {Test list compress/decompress data with compression plugin before/after RDB save and load} {
        assert_equal 15 [r lpush list1 aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee]
        assert_equal {eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa} [r lrange list1 0 14]
        r debug reload
        assert_equal {eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa eeeeeeeeee dddddddddd cccccccccc bbbbbbbbbb aaaaaaaaaa} [r lrange list1 0 14]
        r del list1
    }
}
