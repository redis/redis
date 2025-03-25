tags {modules} {
set testmodule [file normalize tests/modules/internalsecret.so]

set modules [list loadmodule $testmodule]
start_cluster 1 0 [list config_lines $modules] {
    set r [srv 0 client]

    test {Internal command without internal connection fails as an unknown command} {
        assert_error {*unknown command*with args beginning with:*} {r internalauth.internalcommand}
    }

    test {Wrong internalsecret fails authentication} {
        assert_error {*WRONGPASS invalid internal password*} {r auth "internal connection" 123}
    }

    test {Internal connection basic flow} {
        # A non-internal connection cannot execute internal commands, and they
        # seem non-existent to it.
        assert_error {*unknown command*} {r internalauth.internalcommand}

        # Authenticate as an internal connection
        assert_equal {OK} [r debug mark-internal-client]

        # Now, internal commands are available.
        assert_equal {OK} [r internalauth.internalcommand]
    }
}

start_server {} {
    r module load $testmodule

    test {Internal secret is not available in non-cluster mode} {
        # On non-cluster mode, the internal secret does not exist, nor is the
        # internal auth command available
        assert_error {*unknown command*} {r internalauth.internalcommand}
        assert_error {*ERR no internal secret available*} {r internalauth.getinternalsecret}
        assert_error {*Cannot authenticate as an internal connection on non-cluster instances*} {r auth "internal connection" somepassword}
    }

    test {marking and un-marking a connection as internal via a debug command} {
        # After marking the connection to an internal one via a debug command,
        # internal commands succeed.
        r debug mark-internal-client
        assert_equal {OK} [r internalauth.internalcommand]

        # After unmarking the connection, internal commands fail.
        r debug mark-internal-client unmark
        assert_error {*unknown command*} {r internalauth.internalcommand}
    }
}

start_server {} {
    r module load $testmodule

    test {Test `COMMAND *` commands with\without internal connections} {
        # ------------------ Non-internal connection ------------------
        # `COMMAND DOCS <cmd>` returns empty response.
        assert_equal {} [r command docs internalauth.internalcommand]

        # `COMMAND INFO <cmd>` should reply with null for the internal command
        assert_equal {{}} [r command info internalauth.internalcommand]

        # `COMMAND GETKEYS/GETKEYSANDFLAGS <cmd> <args>` returns an invalid command error
        assert_error {*Invalid command specified*} {r command getkeys internalauth.internalcommand}
        assert_error {*Invalid command specified*} {r command getkeysandflags internalauth.internalcommand}

        # -------------------- Internal connection --------------------
        # Non-empty response for non-internal connections.
        assert_equal {OK} [r debug mark-internal-client]

        # `COMMAND DOCS <cmd>` returns a correct response.
        assert_match {*internalauth.internalcommand*} [r command docs internalauth.internalcommand]

        # `COMMAND INFO <cmd>` should reply with a full response for the internal command
        assert_match {*internalauth.internalcommand*} [r command info internalauth.internalcommand]

        # `COMMAND GETKEYS/GETKEYSANDFLAGS <cmd> <args>` returns a key error (not related to the internal connection)
        assert_error {*ERR The command has no key arguments*} {r command getkeys internalauth.internalcommand}
        assert_error {*ERR The command has no key arguments*} {r command getkeysandflags internalauth.internalcommand}
    }
}

start_server {} {
    r module load $testmodule

    test {No authentication needed for internal connections} {
        # Authenticate with a user that does not have permissions to any command
        r acl setuser David on >123 &* ~* -@all +auth +internalauth.getinternalsecret +debug +internalauth.internalcommand
        assert_equal {OK} [r auth David 123]

        assert_equal {OK} [r debug mark-internal-client]
        # Execute a command for which David does not have permission
        assert_equal {OK} [r internalauth.internalcommand]
    }
}

start_server {} {
    r module load $testmodule

    test {RM_Call of internal commands without user-flag succeeds only for all connections} {
        # Fail before authenticating as an internal connection.
        assert_equal {OK} [r internalauth.noninternal_rmcall internalauth.internalcommand]
    }

    test {Internal commands via RM_Call succeeds for non-internal connections depending on the user flag} {
        # A non-internal connection that calls rm_call of an internal command
        assert_equal {OK} [r internalauth.noninternal_rmcall internalauth.internalcommand]

        # A non-internal connection that calls rm_call of an internal command
        # with a user flag should fail.
        assert_error {*unknown command*} {r internalauth.noninternal_rmcall_withuser internalauth.internalcommand}
    }

    test {Internal connections override the user flag} {
        # Authenticate as an internal connection
        assert_equal {OK} [r debug mark-internal-client]

        assert_equal {OK} [r internalauth.noninternal_rmcall internalauth.internalcommand]
        assert_equal {OK} [r internalauth.noninternal_rmcall_withuser internalauth.internalcommand]
    }
}

start_server {} {
    r module load $testmodule

    test {RM_Call with the user-flag after setting thread-safe-context from an internal connection should fail} {
        # Authenticate as an internal connection
        assert_equal {OK} [r debug mark-internal-client]

        # New threadSafeContexts do not inherit the internal flag.
        assert_error {*unknown command*} {r internalauth.noninternal_rmcall_detachedcontext_withuser internalauth.internalcommand}
    }
}

start_server {} {
    r module load $testmodule

    r config set appendonly yes
    r config set appendfsync always
    waitForBgrewriteaof r

    test {AOF executes internal commands successfully} {
        # Authenticate as an internal connection
        assert_equal {OK} [r debug mark-internal-client]

        # Call an internal writing command
        assert_equal {OK} [r internalauth.internal_rmcall_replicated set x 5]

        # Reload the server from the AOF
        r debug loadaof

        # Check if the internal command was executed successfully
        assert_equal {5} [r get x]
    }
}

start_server {} {
    r module load $testmodule

    test {Internal commands are not allowed from scripts} {
        # Internal commands are not allowed from scripts
        assert_error {*not allowed from script*} {r eval {redis.call('internalauth.internalcommand')} 0}

        # Even after authenticating as an internal connection
        assert_equal {OK} [r debug mark-internal-client]
        assert_error {*not allowed from script*} {r eval {redis.call('internalauth.internalcommand')} 0}
    }
}

start_cluster 1 1 [list config_lines $modules] {
    set master [srv 0 client]
    set slave [srv -1 client]

    test {Setup master} {
        # Authenticate as an internal connection
        set reply [$master internalauth.getinternalsecret]
        assert_equal {OK} [$master auth "internal connection" $reply]
    }

    test {Slaves successfully execute internal commands from the replication link} {
        assert {[s -1 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -1 master_link_status] eq {up}
        } else {
            fail "Master link status is not up"
        }

        # Execute internal command in master, that will set `x` to `5`.
        assert_equal {OK} [$master internalauth.internal_rmcall_replicated set x 5]

        # Wait for replica to have the key
        $slave readonly
        wait_for_condition 1000 50 {
            [$slave exists x] eq "1"
        } else {
            fail "Test key was not replicated"
        }

        # See that the slave has the same value for `x`.
        assert_equal {5} [$slave get x]
    }
}

start_server {} {
    r module load $testmodule

    test {Internal commands are not reported in the monitor output for non-internal connections when unsuccessful} {
        set rd [redis_deferring_client]
        $rd monitor
        $rd read ; # Discard the OK
        assert_error {*unknown command*} {r internalauth.internalcommand}

        # Assert that the monitor output does not contain the internal command
        r ping
        assert_match {*ping*} [$rd read]
        $rd close
    }

    test {Internal commands are not reported in the monitor output for non-internal connections when successful} {
        # Authenticate as an internal connection
        assert_equal {OK} [r debug mark-internal-client]

        set rd [redis_deferring_client]
        $rd monitor
        $rd read ; # Discard the OK
        assert_equal {OK} [r internalauth.internalcommand]

        # Assert that the monitor output does not contain the internal command
        r ping
        assert_match {*ping*} [$rd read]
        $rd close
    }

    test {Internal commands are reported in the monitor output for internal connections} {
        set rd [redis_deferring_client]
        $rd debug mark-internal-client
        assert_equal {OK} [$rd read]
        $rd monitor
        $rd read ; # Discard the OK
        assert_equal {OK} [r internalauth.internalcommand]

        # Assert that the monitor output contains the internal command
        assert_match {*internalauth.internalcommand*} [$rd read]
        $rd close
    }

    test {Internal commands are reported in the slowlog} {
        # Set up slowlog to log all commands
        r config set slowlog-log-slower-than 0

        # Execute an internal command
        r slowlog reset
        r internalauth.internalcommand

        # The slow-log should contain the internal command
        set log [r slowlog get 1]
        assert_match {*internalauth.internalcommand*} $log
    }

    test {Internal commands are reported in the latency report} {
        # The latency report should contain the internal command
        set report [r latency histogram internalauth.internalcommand]
        assert_match {*internalauth.internalcommand*} $report
    }

    test {Internal commands are reported in the command stats report} {
        # The INFO report should contain the internal command for both the internal
        # and non-internal connections.
        set report [r info commandstats]
        assert_match {*internalauth.internalcommand*} $report

        set report [r info latencystats]
        assert_match {*internalauth.internalcommand*} $report

        # Un-mark the connection as internal
        r debug mark-internal-client unmark
        assert_error {*unknown command*} {r internalauth.internalcommand}

        # We still expect to see the internal command in the report
        set report [r info commandstats]
        assert_match {*internalauth.internalcommand*} $report

        set report [r info latencystats]
        assert_match {*internalauth.internalcommand*} $report
    }
}
}
