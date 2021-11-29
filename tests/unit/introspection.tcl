start_server {tags {"introspection"}} {
    test {CLIENT LIST} {
        r client list
    } {*addr=*:* fd=* age=* idle=* flags=N db=* sub=0 psub=0 multi=-1 qbuf=26 qbuf-free=* argv-mem=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client*}

    test {CLIENT LIST with IDs} {
        set myid [r client id]
        set cl [split [r client list id $myid] "\r\n"]
        assert_match "id=$myid*" [lindex $cl 0]
    }

    test {CLIENT INFO} {
        r client info
    } {*addr=*:* fd=* age=* idle=* flags=N db=* sub=0 psub=0 multi=-1 qbuf=26 qbuf-free=* argv-mem=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client*}

    test {CLIENT KILL with illegal arguments} {
        assert_error "ERR wrong number*" {r client kill}
        assert_error "ERR syntax error*" {r client kill id 10 wrong_arg}

        assert_error "ERR*greater than 0*" {r client kill id str}
        assert_error "ERR*greater than 0*" {r client kill id -1}
        assert_error "ERR*greater than 0*" {r client kill id 0}

        assert_error "ERR Unknown client type*" {r client kill type wrong_type}

        assert_error "ERR No such user*" {r client kill user wrong_user}

        assert_error "ERR syntax error*" {r client kill skipme yes_or_no}
    }

    test {CLIENT KILL SKIPME YES/NO will kill all clients} {
        # Kill all clients except `me`
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set connected_clients [s connected_clients]
        assert {$connected_clients >= 3}
        set res [r client kill skipme yes]
        assert {$res == $connected_clients - 1}

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

    test {MONITOR supports redacting command arguments} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ; # Discard the OK

        r migrate [srv 0 host] [srv 0 port] key 9 5000
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH user
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH2 user password
        catch {r auth not-real} _
        catch {r auth not-real not-a-password} _
        catch {r hello 2 AUTH not-real not-a-password} _

        assert_match {*"key"*"9"*"5000"*} [$rd read]
        assert_match {*"key"*"9"*"5000"*"(redacted)"*} [$rd read]
        assert_match {*"key"*"9"*"5000"*"(redacted)"*"(redacted)"*} [$rd read]
        assert_match {*"auth"*"(redacted)"*} [$rd read]
        assert_match {*"auth"*"(redacted)"*"(redacted)"*} [$rd read]
        assert_match {*"hello"*"2"*"AUTH"*"(redacted)"*"(redacted)"*} [$rd read]
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

    test {CLIENT GETNAME should return NIL if name is not assigned} {
        r client getname
    } {}

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

    test {CONFIG save params special case handled properly} {
        # No "save" keyword - defaults should apply
        start_server {config "minimal.conf"} {
            assert_match [r config get save] {save {3600 1 300 100 60 10000}}
        }

        # First "save" keyword overrides defaults
        start_server {config "minimal.conf" overrides {save {100 100}}} {
            # Defaults
            assert_match [r config get save] {save {100 100}}
        }
    } {} {external:skip}

    test {CONFIG sanity} {
        # Do CONFIG GET, CONFIG SET and then CONFIG GET again
        # Skip immutable configs, one with no get, and other complicated configs
        set skip_configs {
            rdbchecksum
            daemonize
            io-threads-do-reads
            tcp-backlog
            always-show-logo
            syslog-enabled
            cluster-enabled
            aclfile
            unixsocket
            pidfile
            syslog-ident
            appendfilename
            supervised
            syslog-facility
            databases
            io-threads
            logfile
            unixsocketperm
            replicaof
            slaveof
            requirepass
            server_cpulist
            bio_cpulist
            aof_rewrite_cpulist
            bgsave_cpulist
            set-proc-title
            cluster-config-file
            cluster-port
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

    test {CONFIG REWRITE handles save properly} {
        r config set save "3600 1 300 100 60 10000"
        r config rewrite
        restart_server 0 true false
        assert_equal [r config get save] {save {3600 1 300 100 60 10000}}

        r config set save ""
        r config rewrite
        restart_server 0 true false
        assert_equal [r config get save] {save {}}

        start_server {config "minimal.conf"} {
            assert_equal [r config get save] {save {3600 1 300 100 60 10000}}
            r config set save ""
            r config rewrite
            restart_server 0 true false
            assert_equal [r config get save] {save {}}
        }
    } {} {external:skip}

    # Config file at this point is at a weird state, and includes all
    # known keywords. Might be a good idea to avoid adding tests here.
}
