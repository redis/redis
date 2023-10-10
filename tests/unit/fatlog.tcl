start_server {tags {"fatlog"}} {

    test {FATLOG - check that it starts with an empty log} {
        if {$::external} {
            r fatlog reset
        }
        r fatlog len
    } {0}

    test {FATLOG - only logs commands having bigger response than threshold} {
        r config set fatlog-log-bigger-than 10
        r get a
        assert_equal [r fatlog len] 0
        r set a abcdeabcde1
        r get a
        assert_equal [r fatlog len] 1
    }

    test {FATLOG - max entries is correctly handled} {
        r config set fatlog-log-bigger-than 0
        r config set fatlog-max-len 10
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }
        r fatlog len
    } {10}

    test {FATLOG - GET optional argument to limit output len works} {    
        assert_equal 5  [llength [r fatlog get 5]]
        assert_equal 10 [llength [r fatlog get -1]]
        assert_equal 10 [llength [r fatlog get 20]]
    }

    test {FATLOG - RESET subcommand works} {
        r config set fatlog-log-bigger-than 100000
        r fatlog reset
        r fatlog len
    } {0}

    test {FATLOG - logged entry sanity check} {
        r config set fatlog-log-bigger-than 10
        r client setname foobar
        r get a
        set e [lindex [r fatlog get] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 103
        }
        assert_equal [expr {[lindex $e 2] > 10}] 1
        assert_equal [lindex $e 3] {get a}
        assert_equal {foobar} [lindex $e 5]
    }

    test {FATLOG - Certain commands are omitted that contain sensitive information} {
        r config set fatlog-log-bigger-than 0
        r fatlog reset
        catch {r acl setuser "fatlog test user" +get +set} _
        r config set masterauth ""
        r acl setuser fatlog-test-user +get +set
        r config set fatlog-log-bigger-than 0
        r config set fatlog-log-bigger-than -1
        set fatlog_resp [r fatlog get]

        # Make sure normal configs work, but the two sensitive
        # commands are omitted or redacted
        assert_equal 4 [llength $fatlog_resp]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $fatlog_resp 3] 3]
        assert_equal {config set masterauth (redacted)} [lindex [lindex $fatlog_resp 2] 3]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $fatlog_resp 1] 3]
        assert_equal {config set fatlog-log-bigger-than 0} [lindex [lindex $fatlog_resp 0] 3]
    }                                               
             
    test {FATLOG - Some commands can redact sensitive fields} {
        r config set fatlog-log-bigger-than 0
        r fatlog reset
        r migrate [srv 0 host] [srv 0 port] key 9 5000
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH user
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH2 user password
        r config set fatlog-log-bigger-than -1
        set fatlog_resp [r fatlog get]

        # Make sure all 3 commands were logged, but the sensitive fields are omitted
        assert_equal 3 [llength $fatlog_resp]
        assert_match {* key 9 5000} [lindex [lindex $fatlog_resp 2] 3]
        assert_match {* key 9 5000 AUTH (redacted)} [lindex [lindex $fatlog_resp 1] 3]
        assert_match {* key 9 5000 AUTH2 (redacted) (redacted)} [lindex [lindex $fatlog_resp 0] 3]
    } {} {needs:repl}

    test {FATLOG - Rewritten commands are logged as their original command} {
        r config set fatlog-log-bigger-than 0

        # Test rewriting client arguments
        r sadd set a b c d e
        r fatlog reset

        # SPOP is rewritten as DEL when all keys are removed
        r spop set 10
        assert_equal {spop set 10} [lindex [lindex [r fatlog get] 0] 3]

        # Test replacing client arguments
        r fatlog reset

        # GEOADD is replicated as ZADD
        r geoadd cool-cities -122.33207 47.60621 Seattle
        assert_equal {geoadd cool-cities -122.33207 47.60621 Seattle} [lindex [lindex [r fatlog get] 0] 3]

        # Test replacing a single command argument
        r set A 5
        r fatlog reset
        
        # GETSET is replicated as SET
        r getset a 5
        assert_equal {getset a 5} [lindex [lindex [r fatlog  get] 0] 3]

        # INCRBYFLOAT calls rewrite multiple times, so it's a special case
        r set A 0
        r fatlog reset
        
        # INCRBYFLOAT is repli cated as SET
        r INCRBYFLOAT A 1.0
        assert_equal {INCRBYFLOAT A 1.0} [lindex [lindex [r fatlog get] 0] 3]

        # blocked BLPOP is replicated as LPOP
        set rd [redis_deferring_client]
        $rd blpop l 0
        wait_for_blocked_clients_count 1 50 100
        r multi
        r lpush l foo 
        r fatlog reset
        r exec
        $rd read
        $rd close
        assert_equal {blpop l 0} [lindex [lindex [r fatlog get] 0] 3]
    }

    test {FATLOG - commands with too many arguments are trimmed} {
        r config set fatlog-log-bigger-than 0
        r fatlog reset
        r sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33
        set e [lindex [r fatlog get] 0]
        lindex $e 3
    } {sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 {... (2 more arguments)}}

    test {FATLOG - too long arguments are trimmed} {
        r config set fatlog-log-bigger-than 0
        r fatlog reset
        set arg [string repeat A 129]
        r sadd set foo $arg
        set e [lindex [r fatlog get] 0]
        lindex $e 3
    } {sadd set foo {AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA... (1 more bytes)}}

    test {FATLOG - EXEC is not logged, just executed commands} {
        r config set fatlog-log-bigger-than 0
        r fatlog reset
        r multi
        r get a
        r exec
        assert_equal [r fatlog len] 1
        assert_equal [lindex [lindex [r fatlog get] 0] 3] {get a}
    }

    test {FATLOG - can clean older entries} {
        r client setname lastentry_client
        r config set fatlog-max-len 1
        r get b
        assert {[llength [r fatlog get]] == 1}
        set e [lindex [r fatlog get] 0]
        assert_equal {lastentry_client} [lindex $e 5]
    }

    test {FATLOG - can be disabled} {
        r config set fatlog-max-len 1
        r config set fatlog-log-bigger-than 10
        r set a abcdeabcde1
        r fatlog reset
        r get a
        assert_equal [r fatlog len] 1
        r config set fatlog-log-bigger-than -1
        r fatlog reset
        r get a
        assert_equal [r fatlog len] 0
    }

    test {FATLOG - count must be >= -1} {
       assert_error "ERR count should be greater than or equal to -1" {r fatlog get -2}
       assert_error "ERR count should be greater than or equal to -1" {r fatlog get -222}
    }

    test {FATLOG - get all fat logs} {
        r config set fatlog-log-bigger-than 0
        r config set fatlog-max-len 3
        r fatlog reset

        r set key test
        r sadd set a b c
        r incr num
        r lpush list a

        assert_equal [r fatlog len] 3
        assert_equal 0 [llength [r fatlog get 0]]
        assert_equal 1 [llength [r fatlog get 1]]
        assert_equal 3 [llength [r fatlog get -1]]
        assert_equal 3 [llength [r fatlog get 3]]
    }
    
     test {FATLOG - blocking command is reported only after unblocked} {
        # Cleanup first
        r del mylist
        # create a test client
        set rd [redis_deferring_client]
        
        # config the fatlog and reset
        r config set fatlog-log-bigger-than 0
        r config set fatlog-max-len 110
        r fatlog reset
        
        $rd BLPOP mylist 0
        wait_for_blocked_clients_count 1 50 20
        assert_equal 0 [llength [regexp -all -inline (?=BLPOP) [r fatlog get]]]
        
        r LPUSH mylist 1
        wait_for_blocked_clients_count 0 50 20
        assert_equal 1 [llength [regexp -all -inline (?=BLPOP) [r fatlog get]]]
        
        $rd close
    }
}
                                                                                                                                                           