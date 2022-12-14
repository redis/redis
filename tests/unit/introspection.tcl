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
    }

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
            io-threads
            logfile
            unixsocketperm
            slaveof
            requirepass
            server_cpulist
            bio_cpulist
            aof_rewrite_cpulist
            bgsave_cpulist
            set-proc-title
            swap-mode
            jemalloc-max-bg-threads
            swap-threads
            gtid-enabled
            rocksdb.cache_index_and_filter_blocks
            rocksdb.data.cache_index_and_filter_blocks
            rocksdb.meta.cache_index_and_filter_blocks
            rocksdb.enable_pipelined_write
            rocksdb.disable_auto_compactions
            rocksdb.data.disable_auto_compactions
            rocksdb.meta.disable_auto_compactions
            rocksdb.compression
            rocksdb.data.compression
            rocksdb.meta.compression
            rocksdb.max_open_files
            rocksdb.max_write_buffer_number
            rocksdb.data.max_write_buffer_number
            rocksdb.meta.max_write_buffer_number
            rocksdb.max_background_compactions
            rocksdb.max_background_flushes
            rocksdb.max_subcompactions
            rocksdb.block_size
            rocksdb.data.block_size
            rocksdb.meta.block_size
            rocksdb.level0_slowdown_writes_trigger
            rocksdb.data.level0_slowdown_writes_trigger
            rocksdb.meta.level0_slowdown_writes_trigger
            rocksdb.block_cache_size
            rocksdb.data.block_cache_size
            rocksdb.meta.block_cache_size
            rocksdb.write_buffer_size
            rocksdb.data.write_buffer_size
            rocksdb.meta.write_buffer_size
            rocksdb.target_file_size_base
            rocksdb.data.target_file_size_base
            rocksdb.meta.target_file_size_base
            rocksdb.ratelimiter.rate_per_sec
            rocksdb.bytes_per_sync
            rocksdb.max_bytes_for_level_base
            rocksdb.data.max_bytes_for_level_base
            rocksdb.meta.max_bytes_for_level_base
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
        assert_equal [r ping] "PONG"

        # Verify no changes were introduced
        dict for {k v} $configs {
            assert_equal $v [lindex [r config get $k] 1]
        }
    }

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
    }

    # Config file at this point is at a wierd state, and includes all
    # known keywords. Might be a good idea to avoid adding tests here.
}
