start_server {tags {"introspection"}} {
    test {CLIENT LIST} {
        r client list
    } {*addr=*:* fd=* age=* idle=* flags=N db=9 sub=0 psub=0 multi=-1 qbuf=26 qbuf-free=* argv-mem=* obl=0 oll=0 omem=0 tot-mem=* events=r cmd=client*}

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
        list [$rd read] [$rd read]
    } {*"set" "foo"*"get" "foo"*}

    test {MONITOR can log commands issued by the scripting engine} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ;# Discard the OK
        r eval {redis.call('set',KEYS[1],ARGV[1])} 1 foo bar
        assert_match {*eval*} [$rd read]
        assert_match {*lua*"set"*"foo"*"bar"*} [$rd read]
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
            port
            tls-port
            io-threads
            logfile
            unixsocketperm
            slaveof
            bind
            requirepass
            server_cpulist
            bio_cpulist
            aof_rewrite_cpulist
            bgsave_cpulist
        }

        if {!$::tls} {
            append skip_configs {
                tls-prefer-server-ciphers
                tls-session-cache-timeout
                tls-session-cache-size
                tls-session-caching
                tls-cert-file
                tls-key-file
                tls-dh-params-file
                tls-ca-cert-file
                tls-ca-cert-dir
                tls-protocols
                tls-ciphers
                tls-ciphersuites
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
        restart_server 0 0
        assert_equal [r ping] "PONG"

        # Verify no changes were introduced
        dict for {k v} $configs {
            assert_equal $v [lindex [r config get $k] 1]
        }
    }

    # Config file at this point is at a wierd state, and includes all
    # known keywords. Might be a good idea to avoid adding tests here.
}
