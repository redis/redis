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

    test {SLOWLOG - INFO STATS slowlog metrics with no slowlog entries} {
        r config set slowlog-log-slower-than 1000000
        r config resetstat
        r slowlog reset

        r ping
        r get foo

        set info [r info stats]
        assert_equal 0 [getInfoProperty $info slowlog_commands_count]
        assert_equal {0.00} [getInfoProperty $info slowlog_commands_time_ms_max]
        assert_equal {0.00} [getInfoProperty $info slowlog_commands_time_ms_sum]
    }

    test {SLOWLOG - INFO STATS slowlog metrics accumulate with slow commands} {
        r config set slowlog-log-slower-than 100000
        r config resetstat
        r slowlog reset

        r debug sleep 0.2
        set info [r info stats]
        assert_equal 1 [getInfoProperty $info slowlog_commands_count]
        set sum1 [getInfoProperty $info slowlog_commands_time_ms_sum]
        set max1 [getInfoProperty $info slowlog_commands_time_ms_max]
        assert_morethan $sum1 190
        assert_morethan $max1 190

        r debug sleep 0.3
        set info [r info stats]
        assert_equal 2 [getInfoProperty $info slowlog_commands_count]
        set sum2 [getInfoProperty $info slowlog_commands_time_ms_sum]
        set max2 [getInfoProperty $info slowlog_commands_time_ms_max]
        assert_morethan $sum2 490
        assert_morethan $max2 290
    } {} {needs:debug}

    test {SLOWLOG - INFO STATS slowlog metrics survive SLOWLOG RESET} {
        r config set slowlog-log-slower-than 100000
        r config resetstat
        r slowlog reset

        r debug sleep 0.2
        set info [r info stats]
        set count_before [getInfoProperty $info slowlog_commands_count]
        assert_equal 1 $count_before

        r slowlog reset
        assert_equal 0 [r slowlog len]

        set info [r info stats]
        assert_equal 1 [getInfoProperty $info slowlog_commands_count]
        assert_morethan [getInfoProperty $info slowlog_commands_time_ms_sum] 0
    } {} {needs:debug}

    test {SLOWLOG - INFO STATS slowlog metrics reset with CONFIG RESETSTAT} {
        r config set slowlog-log-slower-than 100000
        r config resetstat
        r slowlog reset

        r debug sleep 0.2
        set info [r info stats]
        assert_equal 1 [getInfoProperty $info slowlog_commands_count]

        r config resetstat
        set info [r info stats]
        assert_equal 0 [getInfoProperty $info slowlog_commands_count]
        assert_equal {0.00} [getInfoProperty $info slowlog_commands_time_ms_max]
        assert_equal {0.00} [getInfoProperty $info slowlog_commands_time_ms_sum]
    } {} {needs:debug}

    test {SLOWLOG - INFO COMMANDSTATS shows slowlog metrics for slow commands} {
        r config set slowlog-log-slower-than 100000
        r config resetstat
        r slowlog reset

        r debug sleep 0.2
        r debug sleep 0.3

        set cmdstat [cmdrstat debug r]
        assert_match {*slowlog_count=2*} $cmdstat
        assert_match {*slowlog_time_ms_sum=*} $cmdstat
        assert_match {*slowlog_time_ms_max=*} $cmdstat

        regexp {slowlog_count=(\d+)} $cmdstat -> sl_count
        regexp {slowlog_time_ms_sum=([0-9.]+)} $cmdstat -> sl_sum
        regexp {slowlog_time_ms_max=([0-9.]+)} $cmdstat -> sl_max
        assert_equal 2 $sl_count
        assert_morethan $sl_sum 490
        assert_morethan $sl_max 290
    } {} {needs:debug}

    test {SLOWLOG - INFO COMMANDSTATS slowlog metrics only on commands that are slow} {
        r config set slowlog-log-slower-than 100000
        r config resetstat
        r slowlog reset

        r set mykey myvalue
        r get mykey
        r debug sleep 0.2

        set cmdstat_set [cmdrstat set r]
        assert_no_match {*slowlog_count*} $cmdstat_set

        set cmdstat_get [cmdrstat get r]
        assert_no_match {*slowlog_count*} $cmdstat_get

        set cmdstat_debug [cmdrstat debug r]
        assert_match {*slowlog_count=1*} $cmdstat_debug
        assert_match {*slowlog_time_ms_sum=*} $cmdstat_debug
        assert_match {*slowlog_time_ms_max=*} $cmdstat_debug
    } {} {needs:debug}

    # Helper: return the argv (field index 3) of the most recent slowlog
    # entry whose first token matches $cmd (case-insensitive). Skips entries
    # generated by CONFIG SET / SLOWLOG GET that are interleaved with the
    # command we actually want to inspect.
    proc latest_slowlog_argv_for {cmd} {
        foreach e [r slowlog get] {
            set argv [lindex $e 3]
            if {[string equal -nocase [lindex $argv 0] $cmd]} {
                return $argv
            }
        }
        return {}
    }

    test {SLOWLOG - slowlog-entry-max-argc and slowlog-entry-max-string-len defaults} {
        # Defaults must match the legacy hard-coded constants
        # (SLOWLOG_ENTRY_MAX_ARGC=32, SLOWLOG_ENTRY_MAX_STRING=128).
        assert_equal 32  [lindex [r config get slowlog-entry-max-argc] 1]
        assert_equal 128 [lindex [r config get slowlog-entry-max-string-len] 1]
    }

    test {SLOWLOG - slowlog-entry-max-argc enforces minimum value of 2} {
        assert_error "*argument must be between*" {r config set slowlog-entry-max-argc 1}
        r config set slowlog-entry-max-argc 2
        assert_equal 2 [lindex [r config get slowlog-entry-max-argc] 1]
    }

    test {SLOWLOG - slowlog-entry-max-string-len enforces minimum value of 1} {
        assert_error "*argument must be between*" {r config set slowlog-entry-max-string-len 0}
        r config set slowlog-entry-max-string-len 1
        assert_equal 1 [lindex [r config get slowlog-entry-max-string-len] 1]
    }

    test {SLOWLOG - slowlog-entry-max-argc=2 preserves command name and adds trim marker} {
        r slowlog reset
        r config set slowlog-entry-max-string-len 128

        # The minimum argc of 2 exists so that the command name is preserved
        # and the trim marker can still be written into the last slot.
        r config set slowlog-log-slower-than 0
        r config set slowlog-entry-max-argc 2
        r sadd myset a b c d
        # 6 args total, slargc=2: marker == argc - slargc + 1 == 5.
        assert_equal {sadd {... (5 more arguments)}} [latest_slowlog_argv_for sadd]
    }

    test {SLOWLOG - custom slowlog-entry-max-argc trims correctly} {
        r config set slowlog-log-slower-than 0
        r config set slowlog-entry-max-argc 5

        # argc > limit: trimmed with marker in the last slot.
        r slowlog reset
        r sadd myset a b c d e f g h
        assert_equal {sadd myset a b {... (6 more arguments)}} \
            [latest_slowlog_argv_for sadd]

        # argc == limit: no marker, logged as-is.
        r slowlog reset
        r sadd myset a b c
        assert_equal {sadd myset a b c} [latest_slowlog_argv_for sadd]

        # argc < limit: no marker, logged as-is.
        r slowlog reset
        r sadd myset a
        assert_equal {sadd myset a} [latest_slowlog_argv_for sadd]
    }

    test {SLOWLOG - custom slowlog-entry-max-string-len trims string args} {
        r slowlog reset
        r config set slowlog-log-slower-than 0
        r config set slowlog-entry-max-argc 32
        r config set slowlog-entry-max-string-len 16

        # String longer than limit: trimmed with "... (N more bytes)" suffix.
        r set mykey [string repeat A 20]
        set expected "set mykey {[string repeat A 16]... (4 more bytes)}"
        assert_equal $expected [latest_slowlog_argv_for set]

        # String length == limit: no suffix, logged as-is.
        r slowlog reset
        r set mykey [string repeat B 16]
        assert_equal "set mykey [string repeat B 16]" \
            [latest_slowlog_argv_for set]

        # String shorter than limit: logged as-is.
        r slowlog reset
        r set mykey short
        assert_equal {set mykey short} [latest_slowlog_argv_for set]
    }

    test {SLOWLOG - runtime config change applies only to subsequent entries} {
        r config set slowlog-log-slower-than 0
        r config set slowlog-entry-max-string-len 128
        r slowlog reset

        set arg [string repeat C 50]

        # First SET is logged with the old (default) limit -> not trimmed.
        # Use short key names so the new (smaller) limit cannot trim them
        # when we look for the entry later.
        r set k1 $arg

        set old_entry_argv [latest_slowlog_argv_for set]
        assert_equal "set k1 $arg" $old_entry_argv

        # Lower the limit and log another entry.
        r config set slowlog-entry-max-string-len 8
        r mset k2{x} v1 k3{x} $arg

        # The new entry must be trimmed...
        set new_entry_argv [latest_slowlog_argv_for mset]
        assert_equal "mset k2{x} v1 k3{x} {[string repeat C 8]... (42 more bytes)}" \
            $new_entry_argv

        # ... while the old one remains untouched
        set old_entry_argv_again [latest_slowlog_argv_for set]
        assert_equal "set k1 $arg" $old_entry_argv_again
    }
}
