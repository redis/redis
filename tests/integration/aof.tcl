set defaults { appendonly {yes} appendfilename {appendonly.aof} }
set server_path [tmpdir server.aof]
set aof_path "$server_path/appendonly.aof"

proc append_to_aof {str} {
    upvar fp fp
    puts -nonewline $fp $str
}

proc create_aof {code} {
    upvar fp fp aof_path aof_path
    set fp [open $aof_path w+]
    uplevel 1 $code
    close $fp
}

proc start_server_aof {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    set srv [start_server [list overrides $config]]
    uplevel 1 $code
    kill_server $srv
}

tags {"aof"} {
    ## Test the server doesn't start when the AOF contains an unfinished MULTI
    create_aof {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [formatCommand multi]
        append_to_aof [formatCommand set bar world]
    }

    start_server_aof [list dir $server_path] {
        test "Unfinished MULTI: Server should have logged an error" {
            set pattern "*Unexpected end of file reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -n1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    ## Test that the server exits when the AOF contains a short read
    create_aof {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [string range [formatCommand set bar world] 0 end-1]
    }

    start_server_aof [list dir $server_path] {
        test "Short read: Server should have logged an error" {
            set pattern "*Bad file format reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -n1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    ## Test that redis-check-aof indeed sees this AOF is not valid
    test "Short read: Utility should confirm the AOF is not valid" {
        catch {
            exec src/redis-check-aof $aof_path
        } result
        assert_match "*not valid*" $result
    }

    test "Short read: Utility should be able to fix the AOF" {
        set result [exec src/redis-check-aof --fix $aof_path << "y\n"]
        assert_match "*Successfully truncated AOF*" $result
    }

    ## Test that the server can be started using the truncated AOF
    start_server_aof [list dir $server_path] {
        test "Fixed AOF: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "Fixed AOF: Keyspace should contain values that were parsable" {
            set client [redis [dict get $srv host] [dict get $srv port]]
            assert_equal "hello" [$client get foo]
            assert_equal "" [$client get bar]
        }
    }

    ## Test that SPOP (that modifies the client's argc/argv) is correctly free'd
    create_aof {
        append_to_aof [formatCommand sadd set foo]
        append_to_aof [formatCommand sadd set bar]
        append_to_aof [formatCommand spop set]
    }

    start_server_aof [list dir $server_path] {
        test "AOF+SPOP: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "AOF+SPOP: Set should have 1 member" {
            set client [redis [dict get $srv host] [dict get $srv port]]
            assert_equal 1 [$client scard set]
        }
    }

    ## Test that EXPIREAT is loaded correctly
    create_aof {
        append_to_aof [formatCommand rpush list foo]
        append_to_aof [formatCommand expireat list 1000]
        append_to_aof [formatCommand rpush list bar]
    }

    start_server_aof [list dir $server_path] {
        test "AOF+EXPIRE: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "AOF+EXPIRE: List should be empty" {
            set client [redis [dict get $srv host] [dict get $srv port]]
            assert_equal 0 [$client llen list]
        }
    }

    start_server {overrides {appendonly {yes} appendfilename {appendonly.aof}}} {
        test {Redis should not try to convert DEL into EXPIREAT for EXPIRE -1} {
            r set x 10
            r expire x -1
        }
    }
}
