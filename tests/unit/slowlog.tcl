start_server {tags {"slowlog"} overrides {slowlog-log-slower-than 1000000}} {
    test {SLOWLOG - check that it starts with an empty log} {
        if {$::external} {
            r slowlog reset
        }
        r slowlog len
    } {0}

    test {SLOWLOG - only logs commands taking more time than specified} {
        r config set slowlog-log-slower-than 100000
        r ping
        assert_equal [r slowlog len] 0
        r debug sleep 0.2
        assert_equal [r slowlog len] 1
    } {} {needs:debug}

    test {SLOWLOG - zero max length is correctly handled} {
        r SLOWLOG reset
        r config set slowlog-max-len 0
        r config set slowlog-log-slower-than 0
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }
        r slowlog len
    } {0}

    test {SLOWLOG - max entries is correctly handled} {
        r config set slowlog-log-slower-than 0
        r config set slowlog-max-len 10
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }
        r slowlog len
    } {10}

    test {SLOWLOG - GET optional argument to limit output len works} {

        assert_equal 5  [llength [r slowlog get 5]]
        assert_equal 10 [llength [r slowlog get -1]]
        assert_equal 10 [llength [r slowlog get 20]]
    }

    test {SLOWLOG - RESET subcommand works} {
        r config set slowlog-log-slower-than 100000
        r slowlog reset
        r slowlog len
    } {0}

    test {SLOWLOG - logged entry sanity check} {
        r client setname foobar
        r debug sleep 0.2
        set e [lindex [r slowlog get] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 106
        }
        assert_equal [expr {[lindex $e 2] > 100000}] 1
        assert_equal [lindex $e 3] {debug sleep 0.2}
        assert_equal {foobar} [lindex $e 5]
    } {} {needs:debug}

    test {SLOWLOG - Certain commands are omitted that contain sensitive information} {
        r config set slowlog-max-len 100
        r config set slowlog-log-slower-than 0
        r slowlog reset
        catch {r acl setuser "slowlog test user" +get +set} _
        r config set masteruser ""
        r config set masterauth ""
        r config set requirepass ""
        r config set tls-key-file-pass ""
        r config set tls-client-key-file-pass ""
        r acl setuser slowlog-test-user +get +set
        r acl getuser slowlog-test-user
        r acl deluser slowlog-test-user non-existing-user
        r config set slowlog-log-slower-than 0
        r config set slowlog-log-slower-than -1
        set slowlog_resp [r slowlog get -1]

        # Make sure normal configs work, but the two sensitive
        # commands are omitted or redacted
        assert_equal 11 [llength $slowlog_resp]
        assert_equal {slowlog reset} [lindex [lindex $slowlog_resp 10] 3]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $slowlog_resp 9] 3]
        assert_equal {config set masteruser (redacted)} [lindex [lindex $slowlog_resp 8] 3]
        assert_equal {config set masterauth (redacted)} [lindex [lindex $slowlog_resp 7] 3]
        assert_equal {config set requirepass (redacted)} [lindex [lindex $slowlog_resp 6] 3]
        assert_equal {config set tls-key-file-pass (redacted)} [lindex [lindex $slowlog_resp 5] 3]
        assert_equal {config set tls-client-key-file-pass (redacted)} [lindex [lindex $slowlog_resp 4] 3]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $slowlog_resp 3] 3]
        assert_equal {acl getuser (redacted)} [lindex [lindex $slowlog_resp 2] 3]
        assert_equal {acl deluser (redacted) (redacted)} [lindex [lindex $slowlog_resp 1] 3]
        assert_equal {config set slowlog-log-slower-than 0} [lindex [lindex $slowlog_resp 0] 3]
    } {} {needs:repl}

    test {SLOWLOG - Some commands can redact sensitive fields} {
        r config set slowlog-log-slower-than 0
        r slowlog reset
        r migrate [srv 0 host] [srv 0 port] key 9 5000
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH user
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH2 user password
        r config set slowlog-log-slower-than -1
        set slowlog_resp [r slowlog get]

        # Make sure all 3 commands were logged, but the sensitive fields are omitted
        assert_equal 4 [llength $slowlog_resp]
        assert_match {* key 9 5000} [lindex [lindex $slowlog_resp 2] 3]
        assert_match {* key 9 5000 AUTH (redacted)} [lindex [lindex $slowlog_resp 1] 3]
        assert_match {* key 9 5000 AUTH2 (redacted) (redacted)} [lindex [lindex $slowlog_resp 0] 3]
    } {} {needs:repl}

    test {SLOWLOG - Rewritten commands are logged as their original command} {
        r config set slowlog-log-slower-than 0

        # Test rewriting client arguments
        r sadd set a b c d e
        r slowlog reset

        # SPOP is rewritten as DEL when all keys are removed
        r spop set 10
        assert_equal {spop set 10} [lindex [lindex [r slowlog get] 0] 3]

        # Test replacing client arguments
        r slowlog reset

        # GEOADD is replicated as ZADD
        r geoadd cool-cities -122.33207 47.60621 Seattle
        assert_equal {geoadd cool-cities -122.33207 47.60621 Seattle} [lindex [lindex [r slowlog get] 0] 3]

        # Test replacing a single command argument
        r set A 5
        r slowlog reset
        
        # GETSET is replicated as SET
        r getset a 5
        assert_equal {getset a 5} [lindex [lindex [r slowlog get] 0] 3]

        # INCRBYFLOAT calls rewrite multiple times, so it's a special case
        r set A 0
        r slowlog reset
        
        # INCRBYFLOAT is replicated as SET
        r INCRBYFLOAT A 1.0
        assert_equal {INCRBYFLOAT A 1.0} [lindex [lindex [r slowlog get] 0] 3]

        # blocked BLPOP is replicated as LPOP
        set rd [redis_deferring_client]
        $rd blpop l 0
        wait_for_blocked_clients_count 1 50 100
        r multi
        r lpush l foo
        r slowlog reset
        r exec
        $rd read
        $rd close
        assert_equal {blpop l 0} [lindex [lindex [r slowlog get] 0] 3]
    }

    test {SLOWLOG - commands with too many arguments are trimmed} {
        r config set slowlog-log-slower-than 0
        r slowlog reset
        r sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33
        set e [lindex [r slowlog get] end-1]
        lindex $e 3
    } {sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 {... (2 more arguments)}}

    test {SLOWLOG - too long arguments are trimmed} {
        r config set slowlog-log-slower-than 0
        r slowlog reset
        set arg [string repeat A 129]
        r sadd set foo $arg
        set e [lindex [r slowlog get] end-1]
        lindex $e 3
    } {sadd set foo {AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA... (1 more bytes)}}

    test {SLOWLOG - EXEC is not logged, just executed commands} {
        r config set slowlog-log-slower-than 100000
        r slowlog reset
        assert_equal [r slowlog len] 0
        r multi
        r debug sleep 0.2
        r exec
        assert_equal [r slowlog len] 1
        set e [lindex [r slowlog get] 0]
        assert_equal [lindex $e 3] {debug sleep 0.2}
    } {} {needs:debug}

    test {SLOWLOG - can clean older entries} {
        r client setname lastentry_client
        r config set slowlog-max-len 1
        r debug sleep 0.2
        assert {[llength [r slowlog get]] == 1}
        set e [lindex [r slowlog get] 0]
        assert_equal {lastentry_client} [lindex $e 5]
    } {} {needs:debug}

    test {SLOWLOG - can be disabled} {
        r config set slowlog-max-len 1
        r config set slowlog-log-slower-than 1
        r slowlog reset
        r debug sleep 0.2
        assert_equal [r slowlog len] 1
        r config set slowlog-log-slower-than -1
        r slowlog reset
        r debug sleep 0.2
        assert_equal [r slowlog len] 0
    } {} {needs:debug}

    test {SLOWLOG - count must be >= -1} {
       assert_error "ERR count should be greater than or equal to -1" {r slowlog get -2}
       assert_error "ERR count should be greater than or equal to -1" {r slowlog get -222}
    }

    test {SLOWLOG - get all slow logs} {
        r config set slowlog-log-slower-than 0
        r config set slowlog-max-len 3
        r slowlog reset

        r set key test
        r sadd set a b c
        r incr num
        r lpush list a

        assert_equal [r slowlog len] 3
        assert_equal 0 [llength [r slowlog get 0]]
        assert_equal 1 [llength [r slowlog get 1]]
        assert_equal 3 [llength [r slowlog get -1]]
        assert_equal 3 [llength [r slowlog get 3]]
    }
    
     test {SLOWLOG - blocking command is reported only after unblocked} {
        # Cleanup first
        r del mylist
        # create a test client
        set rd [redis_deferring_client]
        
        # config the slowlog and reset
        r config set slowlog-log-slower-than 0
        r config set slowlog-max-len 110
        r slowlog reset
        
        $rd BLPOP mylist 0
        wait_for_blocked_clients_count 1 50 20
        assert_equal 0 [llength [regexp -all -inline (?=BLPOP) [r slowlog get]]]
        
        r LPUSH mylist 1
        wait_for_blocked_clients_count 0 50 20
        assert_equal 1 [llength [regexp -all -inline (?=BLPOP) [r slowlog get]]]
        
        $rd close
    }
}
