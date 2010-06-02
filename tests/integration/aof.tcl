set defaults [list [list appendonly yes] [list appendfilename appendonly.aof]]
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
    set _defaults $defaults
    set srv [start_server {overrides [lappend _defaults $overrides]}]
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
        test {Unfinished MULTI: Server should not have been started} {
            is_alive $srv
        } {0}

        test {Unfinished MULTI: Server should have logged an error} {
            exec cat [dict get $srv stdout] | tail -n1
        } {*Unexpected end of file reading the append only file*}
    }

    ## Test that the server exits when the AOF contains a short read
    create_aof {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [string range [formatCommand set bar world] 0 end-1]
    }

    start_server_aof [list dir $server_path] {
        test {Short read: Server should not have been started} {
            is_alive $srv
        } {0}

        test {Short read: Server should have logged an error} {
            exec cat [dict get $srv stdout] | tail -n1
        } {*Bad file format reading the append only file*}
    }

    ## Test that redis-check-aof indeed sees this AOF is not valid
    test {Short read: Utility should confirm the AOF is not valid} {
        catch {
            exec ./redis-check-aof $aof_path
        } str
        set _ $str
    } {*not valid*}

    test {Short read: Utility should be able to fix the AOF} {
        exec echo y | ./redis-check-aof --fix $aof_path
    } {*Successfully truncated AOF*}

    ## Test that the server can be started using the truncated AOF
    start_server_aof [list dir $server_path] {
        test {Fixed AOF: Server should have been started} {
            is_alive $srv
        } {1}

        test {Fixed AOF: Keyspace should contain values that were parsable} {
            set client [redis [dict get $srv host] [dict get $srv port]]
            list [$client get foo] [$client get bar]
        } {hello {}}
    }
}
