#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

start_server {tags {"introspection"}} {
    test "PING" {
        assert_equal {PONG} [r ping]
        assert_equal {redis} [r ping redis]
        assert_error {*wrong number of arguments for 'ping' command} {r ping hello redis}
    }

    test {CLIENT LIST} {
        set client_list [r client list]
        if {[lindex [r config get io-threads] 1] == 1} {
            assert_match {id=* addr=*:* laddr=*:* fd=* name=* age=* idle=* flags=N db=* sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=26 qbuf-free=* argv-mem=* multi-mem=0 rbs=* rbp=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client|list user=* redir=-1 resp=* lib-name=* lib-ver=* io-thread=* tot-net-in=* tot-net-out=* tot-cmds=*} $client_list
        } else {
            assert_match {id=* addr=*:* laddr=*:* fd=* name=* age=* idle=* flags=N db=* sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=0 qbuf-free=* argv-mem=* multi-mem=0 rbs=* rbp=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client|list user=* redir=-1 resp=* lib-name=* lib-ver=* io-thread=* tot-net-in=* tot-net-out=* tot-cmds=*} $client_list
        }
    }

    test {CLIENT LIST with IDs} {
        set myid [r client id]
        set cl [split [r client list id $myid] "\r\n"]
        assert_match "id=$myid * cmd=client|list *" [lindex $cl 0]
    }

    test {CLIENT INFO} {
        set client [r client info]
        if {[lindex [r config get io-threads] 1] == 1} {
            assert_match {id=* addr=*:* laddr=*:* fd=* name=* age=* idle=* flags=N db=* sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=26 qbuf-free=* argv-mem=* multi-mem=0 rbs=* rbp=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client|info user=* redir=-1 resp=* lib-name=* lib-ver=* io-thread=* tot-net-in=* tot-net-out=* tot-cmds=*} $client
        } else {
            assert_match {id=* addr=*:* laddr=*:* fd=* name=* age=* idle=* flags=N db=* sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=0 qbuf-free=* argv-mem=* multi-mem=0 rbs=* rbp=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client|info user=* redir=-1 resp=* lib-name=* lib-ver=* io-thread=* tot-net-in=* tot-net-out=* tot-cmds=*} $client
        }
    } 

    proc get_field_in_client_info {info field} {
        set info [string trim $info]
        foreach item [split $info " "] {
            set kv [split $item "="]
            set k [lindex $kv 0]
            if {[string match $field $k]} {
                return [lindex $kv 1]   
            }
        }
        return ""
    }

    proc get_field_in_client_list {id client_list filed} {
        set list [split $client_list "\r\n"]
        foreach info $list {
            if {[string match "id=$id *" $info] } {
                return [get_field_in_client_info $info $filed]
            }
        }
        return ""
    }

    test {CLIENT INFO input/output/cmds-processed stats} {
        set info1 [r client info]
        set input1 [get_field_in_client_info $info1 "tot-net-in"]
        set output1 [get_field_in_client_info $info1 "tot-net-out"]
        set cmd1 [get_field_in_client_info $info1 "tot-cmds"]

        # Run a command by that client and test if the stats change correctly
        set info2 [r client info]
        set input2 [get_field_in_client_info $info2 "tot-net-in"]
        set output2 [get_field_in_client_info $info2 "tot-net-out"]
        set cmd2 [get_field_in_client_info $info2 "tot-cmds"]

        # NOTE if CLIENT INFO changes it's stats the output_bytes here and in the
        # other related tests will need to be updated.
        set input_bytes 26 ; # CLIENT INFO request
        set output_bytes 300 ; # CLIENT INFO result
        set cmds_processed 1 ; # processed the command CLIENT INFO
        assert_equal [expr $input1+$input_bytes] $input2
        assert {[expr $output1+$output_bytes] < $output2}
        assert_equal [expr $cmd1+$cmds_processed] $cmd2
    }

    test {CLIENT INFO input/output/cmds-processed stats for blocking command} {
        r del mylist
        set rd [redis_deferring_client]
        $rd client id
        set rd_id [$rd read]
 
        set info_list [r client list]
        set input1 [get_field_in_client_list $rd_id $info_list "tot-net-in"]
        set output1 [get_field_in_client_list $rd_id $info_list "tot-net-out"]
        set cmd1 [get_field_in_client_list $rd_id $info_list "tot-cmds"]
        $rd blpop mylist 0

        # Make sure to wait for the $rd client to be blocked
        wait_for_blocked_client

        # Check if input stats have changed for $rd. Since command is blocking
        # and has not been unblocked yet we expect no change in output/cmds-processed
        # stats.
        set info_list [r client list]
        set input2 [get_field_in_client_list $rd_id $info_list "tot-net-in"]
        set output2 [get_field_in_client_list $rd_id $info_list "tot-net-out"]
        set cmd2 [get_field_in_client_list $rd_id $info_list "tot-cmds"]
        assert_equal [expr $input1+34] $input2
        assert_equal $output1 $output2
        assert_equal $cmd1 $cmd2

        # Unblock the $rd client (which will send a reply and thus update output
        # and cmd-processed stats).
        r lpush mylist a

        # Note that the per-client stats are from the POV of the server. The
        # deferred client may have not read the response yet, but the stats
        # are still updated.
        set info_list [r client list]
        set input3 [get_field_in_client_list $rd_id $info_list "tot-net-in"]
        set output3 [get_field_in_client_list $rd_id $info_list "tot-net-out"]
        set cmd3 [get_field_in_client_list $rd_id $info_list "tot-cmds"]
        assert_equal $input2 $input3
        assert_equal [expr $output2+23] $output3
        assert_equal [expr $cmd2+1] $cmd3

        $rd close
    }

    test {CLIENT INFO cmds-processed stats for recursive command} {
        set info [r client info]
        set tot_cmd_before [get_field_in_client_info $info "tot-cmds"]
        r eval "redis.call('ping')" 0
        set info [r client info]
        set tot_cmd_after [get_field_in_client_info $info "tot-cmds"]

        # We executed 3 commands - EVAL, which in turn executed PING and finally CLIENT INFO
        assert_equal [expr $tot_cmd_before+3] $tot_cmd_after
    }

    test {CLIENT KILL with illegal arguments} {
        assert_error "ERR wrong number of arguments for 'client|kill' command" {r client kill}
        assert_error "ERR syntax error*" {r client kill id 10 wrong_arg}

        assert_error "ERR *greater than 0*" {r client kill id str}
        assert_error "ERR *greater than 0*" {r client kill id -1}
        assert_error "ERR *greater than 0*" {r client kill id 0}

        assert_error "ERR Unknown client type*" {r client kill type wrong_type}

        assert_error "ERR No such user*" {r client kill user wrong_user}

        assert_error "ERR syntax error*" {r client kill skipme yes_or_no}

        assert_error "ERR *not an integer or out of range*" {r client kill maxage str}
        assert_error "ERR *not an integer or out of range*" {r client kill maxage 9999999999999999999}
        assert_error "ERR *greater than 0*" {r client kill maxage -1}
    }

    test {CLIENT KILL maxAGE will kill old clients} {
        # This test is very likely to do a false positive if the execute time
        # takes longer than the max age, so give it a few more chances. Go with
        # 3 retries of increasing sleep_time, i.e. start with 2s, then go 4s, 8s.
        set sleep_time 2
        for {set i 0} {$i < 3} {incr i} {
            set rd1 [redis_deferring_client]
            r debug sleep $sleep_time
            set rd2 [redis_deferring_client]
            r acl setuser dummy on nopass +ping
            $rd1 auth dummy ""
            $rd1 read
            $rd2 auth dummy ""
            $rd2 read

            # Should kill rd1 but not rd2
            set max_age [expr $sleep_time / 2]
            set res [r client kill user dummy maxage $max_age]
            if {$res == 1} {
                break
            } else {
                # Clean up and try again next time
                set sleep_time [expr $sleep_time * 2]
                $rd1 close
                $rd2 close
            }

        } ;# for

        if {$::verbose} { puts "CLIENT KILL maxAGE will kill old clients test attempts: $i" }
        assert_equal $res 1

        # rd2 should still be connected
        $rd2 ping
        assert_equal "PONG" [$rd2 read]

        $rd1 close
        $rd2 close
    } {0} {"needs:debug"}

    test {CLIENT KILL SKIPME YES/NO will kill all clients} {
        # Kill all clients except `me`
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set connected_clients [s connected_clients]
        assert {$connected_clients >= 3}
        set res [r client kill skipme yes]
        assert {$res == $connected_clients - 1}
        wait_for_condition 1000 10 {
            [s connected_clients] eq 1
        } else {
            fail "Can't kill all clients except the current one"
        }

        # Kill all clients, including `me`
        set rd3 [redis_deferring_client]
        set rd4 [redis_deferring_client]
        set connected_clients [s connected_clients]
        assert {$connected_clients == 3}
        set res [r client kill skipme no]
        assert_equal $res $connected_clients

        # After killing `me`, the first ping will throw an error
        assert_error "*I/O error*" {r ping}
        assert_equal "PONG" [r ping]

        $rd1 close
        $rd2 close
        $rd3 close
        $rd4 close
    }

    test {CLIENT command unhappy path coverage} {
        assert_error "ERR*wrong number of arguments*" {r client caching}
        assert_error "ERR*when the client is in tracking mode*" {r client caching maybe}
        assert_error "ERR*syntax*" {r client no-evict wrongInput}
        assert_error "ERR*syntax*" {r client reply wrongInput}
        assert_error "ERR*syntax*" {r client tracking wrongInput}
        assert_error "ERR*syntax*" {r client tracking on wrongInput}
        assert_error "ERR*when the client is in tracking mode*" {r client caching off}
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}

        r CLIENT TRACKING ON optout
        assert_error "ERR*syntax*" {r client caching on}

        r CLIENT TRACKING off optout
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}

        assert_error "ERR*No such*" {r client kill 000.123.321.567:0000}
        assert_error "ERR*No such*" {r client kill 127.0.0.1:}

        assert_error "ERR*timeout is not an integer*" {r client pause abc}
        assert_error "ERR timeout is negative" {r client pause -1}
    }

    test "CLIENT KILL close the client connection during bgsave" {
        # Start a slow bgsave, trigger an active fork.
        r flushall
        r set k v
        r config set rdb-key-save-delay 10000000
        r bgsave
        wait_for_condition 1000 10 {
            [s rdb_bgsave_in_progress] eq 1
        } else {
            fail "bgsave did not start in time"
        }

        # Kill (close) the connection
        r client kill skipme no

        # In the past, client connections needed to wait for bgsave
        # to end before actually closing, now they are closed immediately.
        assert_error "*I/O error*" {r ping} ;# get the error very quickly
        assert_equal "PONG" [r ping]

        # Make sure the bgsave is still in progress
        assert_equal [s rdb_bgsave_in_progress] 1

        # Stop the child before we proceed to the next test
        r config set rdb-key-save-delay 0
        r flushall
        wait_for_condition 1000 10 {
            [s rdb_bgsave_in_progress] eq 0
        } else {
            fail "bgsave did not stop in time"
        }
    } {} {needs:save}

    test "CLIENT REPLY OFF/ON: disable all commands reply" {
        set rd [redis_deferring_client]

        # These replies were silenced.
        $rd client reply off
        $rd ping pong
        $rd ping pong2

        $rd client reply on
        assert_equal {OK} [$rd read]
        $rd ping pong3
        assert_equal {pong3} [$rd read]

        $rd close
    }

    test "CLIENT REPLY SKIP: skip the next command reply" {
        set rd [redis_deferring_client]

        # The first pong reply was silenced.
        $rd client reply skip
        $rd ping pong

        $rd ping pong2
        assert_equal {pong2} [$rd read]

        $rd close
    }

    test "CLIENT REPLY ON: unset SKIP flag" {
        set rd [redis_deferring_client]

        $rd client reply skip
        $rd client reply on
        assert_equal {OK} [$rd read] ;# OK from CLIENT REPLY ON command

        $rd ping
        assert_equal {PONG} [$rd read]

        $rd close
    }

    test {MONITOR can log executed commands} {
        set rd [redis_deferring_client]
        $rd monitor
        assert_match {*OK*} [$rd read]
        r set foo bar
        r get foo
        set res [list [$rd read] [$rd read]]
        $rd close
        set _ $res
    } {*"set" "foo"*"get" "foo"*}

    test {MONITOR can log commands issued by the scripting engine} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ;# Discard the OK
        r eval {redis.call('set',KEYS[1],ARGV[1])} 1 foo bar
        assert_match {*eval*} [$rd read]
        assert_match {*lua*"set"*"foo"*"bar"*} [$rd read]
        $rd close
    }

    test {MONITOR can log commands issued by functions} {
        r function load replace {#!lua name=test
            redis.register_function('test', function() return redis.call('set', 'foo', 'bar') end)
        }
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ;# Discard the OK
        r fcall test 0
        assert_match {*fcall*test*} [$rd read]
        assert_match {*lua*"set"*"foo"*"bar"*} [$rd read]
        $rd close
    }

    test {MONITOR supports redacting command arguments} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ; # Discard the OK

        r migrate [srv 0 host] [srv 0 port] key 9 5000
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH user
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH2 user password
        catch {r auth not-real} _
        catch {r auth not-real not-a-password} _
        
        assert_match {*"key"*"9"*"5000"*} [$rd read]
        assert_match {*"key"*"9"*"5000"*"(redacted)"*} [$rd read]
        assert_match {*"key"*"9"*"5000"*"(redacted)"*"(redacted)"*} [$rd read]
        assert_match {*"auth"*"(redacted)"*} [$rd read]
        assert_match {*"auth"*"(redacted)"*"(redacted)"*} [$rd read]

        foreach resp {3 2} {
            if {[lsearch $::denytags "resp3"] >= 0} {
                if {$resp == 3} {continue}
            } elseif {$::force_resp3} {
                if {$resp == 2} {continue}
            }
            catch {r hello $resp AUTH not-real not-a-password} _
            assert_match "*\"hello\"*\"$resp\"*\"AUTH\"*\"(redacted)\"*\"(redacted)\"*" [$rd read]
        }
        $rd close
    } {0} {needs:repl}

    test {MONITOR correctly handles multi-exec cases} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ; # Discard the OK

        # Make sure multi-exec statements are ordered
        # correctly
        r multi
        r set foo bar
        r exec
        assert_match {*"multi"*} [$rd read]
        assert_match {*"set"*"foo"*"bar"*} [$rd read]
        assert_match {*"exec"*} [$rd read]

        # Make sure we close multi statements on errors
        r multi
        catch {r syntax error} _
        catch {r exec} _

        assert_match {*"multi"*} [$rd read]
        assert_match {*"exec"*} [$rd read]

        $rd close
    }
    
    test {MONITOR log blocked command only once} {
        
        # need to reconnect in order to reset the clients state
        reconnect
        
        set rd [redis_deferring_client]
        set bc [redis_deferring_client]
        r del mylist
        
        $rd monitor
        $rd read ; # Discard the OK
        
        $bc blpop mylist 0
        # make sure the blpop arrives first
        $bc flush
        after 100
        wait_for_blocked_clients_count 1
        r lpush mylist 1
        wait_for_blocked_clients_count 0
        r lpush mylist 2
        
        # we expect to see the blpop on the monitor first
        assert_match {*"blpop"*"mylist"*"0"*} [$rd read]
        
        # we scan out all the info commands on the monitor
        set monitor_output [$rd read]
        while { [string match {*"info"*} $monitor_output] } {
            set monitor_output [$rd read]
        }
        
        # we expect to locate the lpush right when the client was unblocked
        assert_match {*"lpush"*"mylist"*"1"*} $monitor_output
        
        # we scan out all the info commands
        set monitor_output [$rd read]
        while { [string match {*"info"*} $monitor_output] } {
            set monitor_output [$rd read]
        }
        
        # we expect to see the next lpush and not duplicate blpop command
        assert_match {*"lpush"*"mylist"*"2"*} $monitor_output
        
        $rd close
        $bc close
    }

    test {CLIENT GETNAME should return NIL if name is not assigned} {
        r client getname
    } {}

    test {CLIENT GETNAME check if name set correctly} {
        r client setname testName
        r client getName
    } {testName}

    test {CLIENT LIST shows empty fields for unassigned names} {
        r client list
    } {*name= *}

    test {CLIENT SETNAME does not accept spaces} {
        catch {r client setname "foo bar"} e
        set e
    } {ERR*}

    test {CLIENT SETNAME can assign a name to this connection} {
        assert_equal [r client setname myname] {OK}
        r client list
    } {*name=myname*}

    test {CLIENT SETNAME can change the name of an existing connection} {
        assert_equal [r client setname someothername] {OK}
        r client list
    } {*name=someothername*}

    test {After CLIENT SETNAME, connection can still be closed} {
        set rd [redis_deferring_client]
        $rd client setname foobar
        assert_equal [$rd read] "OK"
        assert_match {*foobar*} [r client list]
        $rd close
        # Now the client should no longer be listed
        wait_for_condition 50 100 {
            [string match {*foobar*} [r client list]] == 0
        } else {
            fail "Client still listed in CLIENT LIST after SETNAME."
        }
    }

    test {CLIENT SETINFO can set a library name to this connection} {
        r CLIENT SETINFO lib-name redis.py
        r CLIENT SETINFO lib-ver 1.2.3
        r client info
    } {*lib-name=redis.py lib-ver=1.2.3*}

    test {CLIENT SETINFO invalid args} {
        assert_error {*wrong number of arguments*} {r CLIENT SETINFO lib-name}
        assert_error {*cannot contain spaces*} {r CLIENT SETINFO lib-name "redis py"}
        assert_error {*newlines*} {r CLIENT SETINFO lib-name "redis.py\n"}
        assert_error {*Unrecognized*} {r CLIENT SETINFO badger hamster}
        # test that all of these didn't affect the previously set values
        r client info
    } {*lib-name=redis.py lib-ver=1.2.3*}

    test {RESET does NOT clean library name} {
        r reset
        r client info
    } {*lib-name=redis.py*} {needs:reset}

    test {CLIENT SETINFO can clear library name} {
        r CLIENT SETINFO lib-name ""
        r client info
    } {*lib-name= *}

    test {CONFIG save params special case handled properly} {
        # No "save" keyword - defaults should apply
        start_server {config "minimal.conf"} {
            assert_match [r config get save] {save {3600 1 300 100 60 10000}}
        }

        # First "save" keyword overrides hard coded defaults
        start_server {config "minimal.conf" overrides {save {100 100}}} {
            # Defaults
            assert_match [r config get save] {save {100 100}}
        }

        # First "save" keyword appends default from config file
        start_server {config "default.conf" overrides {save {900 1}} args {--save 100 100}} {
            assert_match [r config get save] {save {900 1 100 100}}
        }

        # Empty "save" keyword resets all
        start_server {config "default.conf" overrides {save {900 1}} args {--save {}}} {
            assert_match [r config get save] {save {}}
        }
    } {} {external:skip}

    test {CONFIG sanity} {
        # Do CONFIG GET, CONFIG SET and then CONFIG GET again
        # Skip immutable configs, one with no get, and other complicated configs
        set skip_configs {
            rdbchecksum
            daemonize
            tcp-backlog
            always-show-logo
            syslog-enabled
            cluster-enabled
            disable-thp
            aclfile
            unixsocket
            pidfile
            syslog-ident
            appendfilename
            appenddirname
            supervised
            syslog-facility
            databases
            io-threads
            logfile
            unixsocketperm
            replicaof
            slaveof
            requirepass
            server-cpulist
            bio-cpulist
            aof-rewrite-cpulist
            bgsave-cpulist
            server_cpulist
            bio_cpulist
            aof_rewrite_cpulist
            bgsave_cpulist
            set-proc-title
            cluster-config-file
            cluster-port
            oom-score-adj
            oom-score-adj-values
            enable-protected-configs
            enable-debug-command
            enable-module-command
            dbfilename
            logfile
            dir
            socket-mark-id
            req-res-logfile
            client-default-resp
            vset-force-single-threaded-execution
        }

        if {!$::tls} {
            append skip_configs {
                tls-prefer-server-ciphers
                tls-session-cache-timeout
                tls-session-cache-size
                tls-session-caching
                tls-cert-file
                tls-key-file
                tls-client-cert-file
                tls-client-key-file
                tls-dh-params-file
                tls-ca-cert-file
                tls-ca-cert-dir
                tls-protocols
                tls-ciphers
                tls-ciphersuites
                tls-port
            }
        }

        set configs {}
        foreach {k v} [r config get *] {
            if {[lsearch $skip_configs $k] != -1} {
                continue
            }
            dict set configs $k $v
            # try to set the config to the same value it already has
            r config set $k $v
        }

        set newconfigs {}
        foreach {k v} [r config get *] {
            if {[lsearch $skip_configs $k] != -1} {
                continue
            }
            dict set newconfigs $k $v
        }

        dict for {k v} $configs {
            set vv [dict get $newconfigs $k]
            if {$v != $vv} {
                fail "config $k mismatch, expecting $v but got $vv"
            }

        }
    }

    # Do a force-all config rewrite and make sure we're able to parse
    # it.
    test {CONFIG REWRITE sanity} {
        # Capture state of config before
        set configs {}
        foreach {k v} [r config get *] {
            dict set configs $k $v
        }

        # Rewrite entire configuration, restart and confirm the
        # server is able to parse it and start.
        assert_equal [r debug config-rewrite-force-all] "OK"
        restart_server 0 true false
        wait_done_loading r

        # Verify no changes were introduced
        dict for {k v} $configs {
            assert_equal $v [lindex [r config get $k] 1]
        }
    } {} {external:skip}

    test {CONFIG REWRITE handles save and shutdown properly} {
        r config set save "3600 1 300 100 60 10000"
        r config set shutdown-on-sigterm "nosave now"
        r config set shutdown-on-sigint "save"
        r config rewrite
        restart_server 0 true false
        assert_equal [r config get save] {save {3600 1 300 100 60 10000}}
        assert_equal [r config get shutdown-on-sigterm] {shutdown-on-sigterm {nosave now}}
        assert_equal [r config get shutdown-on-sigint] {shutdown-on-sigint save}

        r config set save ""
        r config set shutdown-on-sigterm "default"
        r config rewrite
        restart_server 0 true false
        assert_equal [r config get save] {save {}}
        assert_equal [r config get shutdown-on-sigterm] {shutdown-on-sigterm default}

        start_server {config "minimal.conf"} {
            assert_equal [r config get save] {save {3600 1 300 100 60 10000}}
            r config set save ""
            r config rewrite
            restart_server 0 true false
            assert_equal [r config get save] {save {}}
        }
    } {} {external:skip}
    
    test {CONFIG SET with multiple args} {
        set some_configs {maxmemory 10000001 repl-backlog-size 10000002 save {3000 5}}

        # Backup
        set backups {}
        foreach c [dict keys $some_configs] {
            lappend backups $c [lindex [r config get $c] 1]
        }

        # multi config set and veirfy
        assert_equal [eval "r config set $some_configs"] "OK"
        dict for {c val} $some_configs {
            assert_equal [lindex [r config get $c] 1] $val
        }

        # Restore backup
        assert_equal [eval "r config set $backups"] "OK"
    }

    test {CONFIG SET rollback on set error} {
        # This test passes an invalid percent value to maxmemory-clients which should cause an
        # input verification failure during the "set" phase before trying to apply the 
        # configuration. We want to make sure the correct failure happens and everything
        # is rolled back.
        # backup maxmemory config
        set mm_backup [lindex [r config get maxmemory] 1]
        set mmc_backup [lindex [r config get maxmemory-clients] 1]
        set qbl_backup [lindex [r config get client-query-buffer-limit] 1]
        # Set some value to maxmemory
        assert_equal [r config set maxmemory 10000002] "OK"
        # Set another value to maxmeory together with another invalid config
        assert_error "ERR CONFIG SET failed (possibly related to argument 'maxmemory-clients') - percentage argument must be less or equal to 100" {
            r config set maxmemory 10000001 maxmemory-clients 200% client-query-buffer-limit invalid
        }
        # Validate we rolled back to original values
        assert_equal [lindex [r config get maxmemory] 1] 10000002
        assert_equal [lindex [r config get maxmemory-clients] 1] $mmc_backup
        assert_equal [lindex [r config get client-query-buffer-limit] 1] $qbl_backup
        # Make sure we revert back to the previous maxmemory
        assert_equal [r config set maxmemory $mm_backup] "OK"
    }

    test {CONFIG SET rollback on apply error} {
        # This test tries to configure a used port number in redis. This is expected
        # to pass the `CONFIG SET` validity checking implementation but fail on 
        # actual "apply" of the setting. This will validate that after an "apply"
        # failure we rollback to the previous values.
        proc dummy_accept {chan addr port} {}

        set some_configs {maxmemory 10000001 port 0 client-query-buffer-limit 10m}

        # On Linux we also set the oom score adj which has an apply function. This is
        # used to verify that even successful applies are rolled back if some other
        # config's apply fails.
        set oom_adj_avail [expr {!$::external && [exec uname] == "Linux"}]
        if {$oom_adj_avail} {
            proc get_oom_score_adj {} {
                set pid [srv 0 pid]
                set fd [open "/proc/$pid/oom_score_adj" "r"]
                set val [gets $fd]
                close $fd
                return $val
            }
            set some_configs [linsert $some_configs 0 oom-score-adj yes oom-score-adj-values {1 1 1}]
            set read_oom_adj [get_oom_score_adj]
        }

        # Backup
        set backups {}
        foreach c [dict keys $some_configs] {
            lappend backups $c [lindex [r config get $c] 1]
        }

        set used_port [find_available_port $::baseport $::portcount]
        dict set some_configs port $used_port

        # Run a dummy server on used_port so we know we can't configure redis to 
        # use it. It's ok for this to fail because that means used_port is invalid 
        # anyway
        catch {set sockfd [socket -server dummy_accept -myaddr 127.0.0.1 $used_port]} e
        if {$::verbose} { puts "dummy_accept: $e" }

        # Try to listen on the used port, pass some more configs to make sure the
        # returned failure message is for the first bad config and everything is rolled back.
        assert_error "ERR CONFIG SET failed (possibly related to argument 'port') - Unable to listen on this port*" {
            eval "r config set $some_configs"
        }

        # Make sure we reverted back to previous configs
        dict for {conf val} $backups {
            assert_equal [lindex [r config get $conf] 1] $val
        }

        if {$oom_adj_avail} {
            assert_equal [get_oom_score_adj] $read_oom_adj
        }

        # Make sure we can still communicate with the server (on the original port)
        set r1 [redis_client]
        assert_equal [$r1 ping] "PONG"
        $r1 close
        close $sockfd
    }

    test {CONFIG SET duplicate configs} {
        assert_error "ERR *duplicate*" {r config set maxmemory 10000001 maxmemory 10000002}
    }

    test {CONFIG SET set immutable} {
        assert_error "ERR *immutable*" {r config set daemonize yes}
    }

    test {CONFIG GET hidden configs} {
        set hidden_config "key-load-delay"

        # When we use a pattern we shouldn't get the hidden config
        assert {![dict exists [r config get *] $hidden_config]}

        # When we explicitly request the hidden config we should get it
        assert {[dict exists [r config get $hidden_config] "$hidden_config"]}
    }

    test {CONFIG GET multiple args} {
        set res [r config get maxmemory maxmemory* bind *of]
        
        # Verify there are no duplicates in the result
        assert_equal [expr [llength [dict keys $res]]*2] [llength $res]
        
        # Verify we got both name and alias in result
        assert {[dict exists $res slaveof] && [dict exists $res replicaof]}  

        # Verify pattern found multiple maxmemory* configs
        assert {[dict exists $res maxmemory] && [dict exists $res maxmemory-samples] && [dict exists $res maxmemory-clients]}  

        # Verify we also got the explicit config
        assert {[dict exists $res bind]}  
    }

    test {redis-server command line arguments - error cases} {
        # Take '--invalid' as the option.
        catch {exec src/redis-server --invalid} err
        assert_match {*Bad directive or wrong number of arguments*} $err

        catch {exec src/redis-server --port} err
        assert_match {*'port'*wrong number of arguments*} $err

        catch {exec src/redis-server --port 6380 --loglevel} err
        assert_match {*'loglevel'*wrong number of arguments*} $err

        # Take `6379` and `6380` as the port option value.
        catch {exec src/redis-server --port 6379 6380} err
        assert_match {*'port "6379" "6380"'*wrong number of arguments*} $err

        # Take `--loglevel` and `verbose` as the port option value.
        catch {exec src/redis-server --port --loglevel verbose} err
        assert_match {*'port "--loglevel" "verbose"'*wrong number of arguments*} $err

        # Take `--bla` as the port option value.
        catch {exec src/redis-server --port --bla --loglevel verbose} err
        assert_match {*'port "--bla"'*argument couldn't be parsed into an integer*} $err

        # Take `--bla` as the loglevel option value.
        catch {exec src/redis-server --logfile --my--log--file --loglevel --bla} err
        assert_match {*'loglevel "--bla"'*argument(s) must be one of the following*} $err

        # Using MULTI_ARG's own check, empty option value
        catch {exec src/redis-server --shutdown-on-sigint} err
        assert_match {*'shutdown-on-sigint'*argument(s) must be one of the following*} $err
        catch {exec src/redis-server --shutdown-on-sigint "now force" --shutdown-on-sigterm} err
        assert_match {*'shutdown-on-sigterm'*argument(s) must be one of the following*} $err

        # Something like `redis-server --some-config --config-value1 --config-value2 --loglevel debug` would break,
        # because if you want to pass a value to a config starting with `--`, it can only be a single value.
        catch {exec src/redis-server --replicaof 127.0.0.1 abc} err
        assert_match {*'replicaof "127.0.0.1" "abc"'*Invalid master port*} $err
        catch {exec src/redis-server --replicaof --127.0.0.1 abc} err
        assert_match {*'replicaof "--127.0.0.1" "abc"'*Invalid master port*} $err
        catch {exec src/redis-server --replicaof --127.0.0.1 --abc} err
        assert_match {*'replicaof "--127.0.0.1"'*wrong number of arguments*} $err
    } {} {external:skip}

    test {redis-server command line arguments - allow passing option name and option value in the same arg} {
        start_server {config "default.conf" args {"--maxmemory 700mb" "--maxmemory-policy volatile-lru"}} {
            assert_match [r config get maxmemory] {maxmemory 734003200}
            assert_match [r config get maxmemory-policy] {maxmemory-policy volatile-lru}
        }
    } {} {external:skip}

    test {redis-server command line arguments - wrong usage that we support anyway} {
        start_server {config "default.conf" args {loglevel verbose "--maxmemory '700mb'" "--maxmemory-policy 'volatile-lru'"}} {
            assert_match [r config get loglevel] {loglevel verbose}
            assert_match [r config get maxmemory] {maxmemory 734003200}
            assert_match [r config get maxmemory-policy] {maxmemory-policy volatile-lru}
        }
    } {} {external:skip}

    test {redis-server command line arguments - allow option value to use the `--` prefix} {
        start_server {config "default.conf" args {--proc-title-template --my--title--template --loglevel verbose}} {
            assert_match [r config get proc-title-template] {proc-title-template --my--title--template}
            assert_match [r config get loglevel] {loglevel verbose}
        }
    } {} {external:skip}

    test {redis-server command line arguments - option name and option value in the same arg and `--` prefix} {
        start_server {config "default.conf" args {"--proc-title-template --my--title--template" "--loglevel verbose"}} {
            assert_match [r config get proc-title-template] {proc-title-template --my--title--template}
            assert_match [r config get loglevel] {loglevel verbose}
        }
    } {} {external:skip}

    test {redis-server command line arguments - save with empty input} {
        start_server {config "default.conf" args {--save --loglevel verbose}} {
            assert_match [r config get save] {save {}}
            assert_match [r config get loglevel] {loglevel verbose}
        }

        start_server {config "default.conf" args {--loglevel verbose --save}} {
            assert_match [r config get save] {save {}}
            assert_match [r config get loglevel] {loglevel verbose}
        }

        start_server {config "default.conf" args {--save {} --loglevel verbose}} {
            assert_match [r config get save] {save {}}
            assert_match [r config get loglevel] {loglevel verbose}
        }

        start_server {config "default.conf" args {--loglevel verbose --save {}}} {
            assert_match [r config get save] {save {}}
            assert_match [r config get loglevel] {loglevel verbose}
        }

        start_server {config "default.conf" args {--proc-title-template --save --save {} --loglevel verbose}} {
            assert_match [r config get proc-title-template] {proc-title-template --save}
            assert_match [r config get save] {save {}}
            assert_match [r config get loglevel] {loglevel verbose}
        }

    } {} {external:skip}

    test {redis-server command line arguments - take one bulk string with spaces for MULTI_ARG configs parsing} {
        start_server {config "default.conf" args {--shutdown-on-sigint nosave force now --shutdown-on-sigterm "nosave force"}} {
            assert_match [r config get shutdown-on-sigint] {shutdown-on-sigint {nosave now force}}
            assert_match [r config get shutdown-on-sigterm] {shutdown-on-sigterm {nosave force}}
        }
    } {} {external:skip}

    # Config file at this point is at a weird state, and includes all
    # known keywords. Might be a good idea to avoid adding tests here.
}

start_server {tags {"introspection external:skip"} overrides {enable-protected-configs {no} enable-debug-command {no}}} {
    test {cannot modify protected configuration - no} {
        assert_error "ERR *protected*" {r config set dir somedir}
        assert_error "ERR *DEBUG command not allowed*" {r DEBUG HELP}
    } {} {needs:debug}
}

start_server {config "minimal.conf" tags {"introspection external:skip"} overrides {protected-mode {no} enable-protected-configs {local} enable-debug-command {local}}} {
    test {cannot modify protected configuration - local} {
        # verify that for local connection it doesn't error
        r config set dbfilename somename
        r DEBUG HELP

        # Get a non-loopback address of this instance for this test.
        set myaddr [get_nonloopback_addr]
        if {$myaddr != "" && ![string match {127.*} $myaddr]} {
            # Non-loopback client should fail
            set r2 [get_nonloopback_client]
            assert_error "ERR *protected*" {$r2 config set dir somedir}
            assert_error "ERR *DEBUG command not allowed*" {$r2 DEBUG HELP}
        }
    } {} {needs:debug}
}

test {config during loading} {
    start_server [list overrides [list key-load-delay 50 loading-process-events-interval-bytes 1024 rdbcompression no save "900 1"]] {
        # create a big rdb that will take long to load. it is important
        # for keys to be big since the server processes events only once in 2mb.
        # 100mb of rdb, 100k keys will load in more than 5 seconds
        r debug populate 100000 key 1000

        restart_server 0 false false

        # make sure it's still loading
        assert_equal [s loading] 1

        # verify some configs are allowed during loading
        r config set loglevel debug
        assert_equal [lindex [r config get loglevel] 1] debug

        # verify some configs are forbidden during loading
        assert_error {LOADING*} {r config set dir asdf}

        # make sure it's still loading
        assert_equal [s loading] 1

        # no need to keep waiting for loading to complete
        exec kill [srv 0 pid]
    }
} {} {external:skip}

test {CONFIG REWRITE handles rename-command properly} {
    start_server {tags {"introspection"} overrides {rename-command {flushdb badger}}} {
        assert_error {ERR unknown command*} {r flushdb}

        r config rewrite
        restart_server 0 true false

        assert_error {ERR unknown command*} {r flushdb}
    }
} {} {external:skip}

test {CONFIG REWRITE handles alias config properly} {
    start_server {tags {"introspection"} overrides {hash-max-listpack-entries 20 hash-max-ziplist-entries 21}} {
        assert_equal [r config get hash-max-listpack-entries] {hash-max-listpack-entries 21}
        assert_equal [r config get hash-max-ziplist-entries] {hash-max-ziplist-entries 21}
        r config set hash-max-listpack-entries 100

        r config rewrite
        restart_server 0 true false

        assert_equal [r config get hash-max-listpack-entries] {hash-max-listpack-entries 100}
    }
    # test the order doesn't matter
    start_server {tags {"introspection"} overrides {hash-max-ziplist-entries 20 hash-max-listpack-entries 21}} {
        assert_equal [r config get hash-max-listpack-entries] {hash-max-listpack-entries 21}
        assert_equal [r config get hash-max-ziplist-entries] {hash-max-ziplist-entries 21}
        r config set hash-max-listpack-entries 100

        r config rewrite
        restart_server 0 true false

        assert_equal [r config get hash-max-listpack-entries] {hash-max-listpack-entries 100}
    }
} {} {external:skip}

test {IO threads client number} {
    start_server {overrides {io-threads 2} tags {external:skip}} {
        set iothread_clients [get_io_thread_clients 1]
        assert_equal $iothread_clients [s connected_clients]
        assert_equal [get_io_thread_clients 0] 0

        r script debug yes ; # Transfer to main thread
        assert_equal [get_io_thread_clients 0] 1
        assert_equal [get_io_thread_clients 1] [expr $iothread_clients - 1]

        set iothread_clients [get_io_thread_clients 1]
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        assert_equal [get_io_thread_clients 1] [expr $iothread_clients + 2]
        $rd1 close
        $rd2 close
        wait_for_condition 1000 10 {
            [get_io_thread_clients 1] eq $iothread_clients
        } else {
            fail "Fail to close clients of io thread 1"
        }
        assert_equal [get_io_thread_clients 0] 1

        r script debug no ; # Transfer to io thread
        assert_equal [get_io_thread_clients 0] 0
        assert_equal [get_io_thread_clients 1] [expr $iothread_clients + 1]
    }
}

test {Clients are evenly distributed among io threads} {
    start_server {overrides {io-threads 4} tags {external:skip}} {
        set cur_clients [s connected_clients]
        assert_equal $cur_clients 1
        global rdclients
        for {set i 1} {$i < 9} {incr i} {
            set rdclients($i) [redis_deferring_client]
        }
        for {set i 1} {$i <= 3} {incr i} {
            assert_equal [get_io_thread_clients $i] 3
        }

        $rdclients(3) close
        $rdclients(4) close
        wait_for_condition 1000 10 {
            [get_io_thread_clients 1] eq 2 &&
            [get_io_thread_clients 2] eq 2 &&
            [get_io_thread_clients 3] eq 3
        } else {
            fail "Fail to close clients"
        }

        set  $rdclients(3) [redis_deferring_client]
        set  $rdclients(4) [redis_deferring_client]
        for {set i 1} {$i <= 3} {incr i} {
            assert_equal [get_io_thread_clients $i] 3
        }
    }
}
