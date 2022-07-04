# [functional testing]
# open or close gtid-enabled efficient
start_server {tags {"master"} overrides} {
    test "change gtid-enabled efficient" {
        set repl [attach_to_replication_stream]
        r set k v1
        assert_replication_stream $repl {
            {select *}
            {set k v1}
        }
        assert_equal [r get k] v1

        r config set gtid-enabled yes

        set repl [attach_to_replication_stream]
        r set k v2
        assert_replication_stream $repl {
            {select *}
            {gtid * 9 set k v2}
        }
        assert_equal [r get k] v2

        r config set gtid-enabled no 
        set repl [attach_to_replication_stream]
        r set k v3
        assert_replication_stream $repl {
            {select *}
            {set k v3}
        }
        assert_equal [r get k] v3
    }
}

#closed gtid-enabled, can exec gtid command
start_server {tags {"gtid"} overrides} {
    test "exec gtid command" {
        r gtid A:1 9 set k v 
        assert_equal [r get k] v 
        assert_equal [dict get [get_gtid r] "A"] "1"
    }
}

# stand-alone redis exec gtid related commands
start_server {tags {"gtid"} overrides {gtid-enabled yes}} {
    test {COMMANDS} {
        test {GTID SET} {
            r gtid A:1 9 set x foobar
            r get x 
        } {foobar}

        test {GTID AND COMMENT SET} {
            r gtid A:2 9 {/*comment*/} set x1 foobar 
            r get x1 
        } {foobar}

        test {GTID REPATE SET} {
            catch {r gtid A:1 9 set x foobar} error
            assert_match $error "ERR gtid command is executed, `A:1`, `9`, `set`,"
        } 
        test {SET} {
            r set y foobar
            r get y 
        } {foobar}

        test {GTID.AUTO} {
            r gtid.auto /*comment*/ set y foobar1
            r get y 
        } {foobar1}

        test {MULTI} {
            r multi 
            r set z foobar 
            r gtid A:3 9 exec
            r set z f
            r get z
        } {f}
        test {MULTI} {
            set z_value [r get z]
            r del x
            assert_equal [r get x] {}
            r multi 
            r set z foobar1 
            catch {r gtid A:3 9 exec} error 
            assert_equal $error "ERR gtid command is executed, `A:3`, `9`, `exec`,"
            assert_equal [r get z] $z_value
            r set x f1
            r get x
        } {f1}
        test "ERR WRONG NUMBER" {
            catch {r gtid A } error 
            assert_match "ERR wrong number of arguments for 'gtid' command" $error
        }
        
    }

    test {INFO GTID} {
        assert_equal [string match {*all:*A:1-*} [r info gtid]] 1
        set dicts [dict get [get_gtid r] [status r run_id]]
        set value [lindex $dicts 0]
        assert_equal [string match {1-*} $value] 1  
    } 

    test "GTID.LWM" {
        assert_equal [string match {*all:*A:1-100*} [r info gtid]] 0
        r gtid.lwm A 100
        assert_equal [dict get [get_gtid r] "A"] "1-100"
    }
    
}

# verify gtid command db
start_server {tags {"gtid"} overrides {gtid-enabled yes}} {
    test "multi-exec select db" {
        set repl [attach_to_replication_stream]
        r set k v 
        r select 0
        r set k v 
        assert_replication_stream $repl {
            {select *}
            {gtid * 9 set k v}
            {select *}
            {gtid * 0 set k v}
        }
        r select 9
    }

    test "multi-exec select db" {
        set repl [attach_to_replication_stream]
        r multi 
        r set k v 
        r select 0
        r set k v 
        r exec
        r set k v1
        assert_replication_stream $repl {
            {select *}
            {multi}
            {set k v}
            {select 0}
            {set k v}
            {gtid * 9 exec}
            {gtid * 0 set k v1}
        }
    }
}