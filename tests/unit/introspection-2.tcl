proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

proc getlru {key} {
    set objinfo [r debug object $key]
    foreach info $objinfo {
        set kvinfo [split $info ":"]
        if {[string compare [lindex $kvinfo 0] "lru"] == 0} {
            return [lindex $kvinfo 1]
        }
    }
    fail "Can't get LRU info with DEBUG OBJECT"
}

start_server {tags {"introspection"}} {
    test {The microsecond part of the TIME command will not overflow} {
        set now [r time]
        set microseconds [lindex $now 1]
        assert_morethan $microseconds 0
        assert_lessthan $microseconds 1000000
    }

    test {TTL, TYPE and EXISTS do not alter the last access time of a key} {
        r set foo bar
        after 3000
        r ttl foo
        r type foo
        r exists foo
        assert {[r object idletime foo] >= 2}
    }

    test {TOUCH alters the last access time of a key} {
        r set foo bar
        after 3000
        r touch foo
        assert {[r object idletime foo] < 2}
    }

    test {Operations in no-touch mode do not alter the last access time of a key} {
        r set foo bar
        r client no-touch on
        set oldlru [getlru foo]
        after 1100
        r get foo
        set newlru [getlru foo]
        assert_equal $newlru $oldlru
        r client no-touch off
        r get foo
        set newlru [getlru foo]
        assert_morethan $newlru $oldlru
    } {} {needs:debug}

    test {Operations in no-touch mode TOUCH alters the last access time of a key} {
        r set foo bar
        r client no-touch on
        set oldlru [getlru foo]
        after 1100
        r touch foo
        set newlru [getlru foo]
        assert_morethan $newlru $oldlru
    } {} {needs:debug}

    test {Operations in no-touch mode TOUCH from script alters the last access time of a key} {
        r set foo bar
        r client no-touch on
        set oldlru [getlru foo]
        after 1100
        assert_equal {1} [r eval "return redis.call('touch', 'foo')" 0]
        set newlru [getlru foo]
        assert_morethan $newlru $oldlru
    } {} {needs:debug}

    test {TOUCH returns the number of existing keys specified} {
        r flushdb
        r set key1{t} 1
        r set key2{t} 2
        r touch key0{t} key1{t} key2{t} key3{t}
    } 2

    test {command stats for GEOADD} {
        r config resetstat
        r GEOADD foo 0 0 bar
        assert_match {*calls=1,*} [cmdstat geoadd]
        assert_match {} [cmdstat zadd]
    } {} {needs:config-resetstat}

    test {errors stats for GEOADD} {
        r config resetstat
        # make sure geo command will failed
        r set foo 1
        assert_error {WRONGTYPE Operation against a key holding the wrong kind of value*} {r GEOADD foo 0 0 bar}
        assert_match {*calls=1*,rejected_calls=0,failed_calls=1*} [cmdstat geoadd]
        assert_match {} [cmdstat zadd]
    } {} {needs:config-resetstat}

    test {command stats for EXPIRE} {
        r config resetstat
        r SET foo bar
        r EXPIRE foo 0
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {} [cmdstat del]
    } {} {needs:config-resetstat}

    test {command stats for BRPOP} {
        r config resetstat
        r LPUSH list foo
        r BRPOP list 0
        assert_match {*calls=1,*} [cmdstat brpop]
        assert_match {} [cmdstat rpop]
    } {} {needs:config-resetstat}

    test {command stats for MULTI} {
        r config resetstat
        r MULTI
        r set foo{t} bar
        r GEOADD foo2{t} 0 0 bar
        r EXPIRE foo2{t} 0
        r EXEC
        assert_match {*calls=1,*} [cmdstat multi]
        assert_match {*calls=1,*} [cmdstat exec]
        assert_match {*calls=1,*} [cmdstat set]
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {*calls=1,*} [cmdstat geoadd]
    } {} {needs:config-resetstat}

    test {command stats for scripts} {
        r config resetstat
        r set mykey myval
        r eval {
            redis.call('set', KEYS[1], 0)
            redis.call('expire', KEYS[1], 0)
            redis.call('geoadd', KEYS[1], 0, 0, "bar")
        } 1 mykey
        assert_match {*calls=1,*} [cmdstat eval]
        assert_match {*calls=2,*} [cmdstat set]
        assert_match {*calls=1,*} [cmdstat expire]
        assert_match {*calls=1,*} [cmdstat geoadd]
    } {} {needs:config-resetstat}

    test {COMMAND COUNT get total number of Redis commands} {
        assert_morethan [r command count] 0
    }

    test {COMMAND GETKEYS GET} {
        assert_equal {key} [r command getkeys get key]
    }

    test {COMMAND GETKEYSANDFLAGS} {
        assert_equal {{k1 {OW update}}} [r command getkeysandflags set k1 v1]
        assert_equal {{k1 {OW update}} {k2 {OW update}}} [r command getkeysandflags mset k1 v1 k2 v2]
        assert_equal {{k1 {RW access delete}} {k2 {RW insert}}} [r command getkeysandflags LMOVE k1 k2 left right]
        assert_equal {{k1 {RO access}} {k2 {OW update}}} [r command getkeysandflags sort k1 store k2]
    }

    test {COMMAND GETKEYSANDFLAGS invalid args} {
        assert_error "ERR Invalid arguments*" {r command getkeysandflags ZINTERSTORE zz 1443677133621497600 asdf}
    }

    test {COMMAND GETKEYS MEMORY USAGE} {
        assert_equal {key} [r command getkeys memory usage key]
    }

    test {COMMAND GETKEYS XGROUP} {
        assert_equal {key} [r command getkeys xgroup create key groupname $]
    }

    test {COMMAND GETKEYS EVAL with keys} {
        assert_equal {key} [r command getkeys eval "return 1" 1 key]
    }

    test {COMMAND GETKEYS EVAL without keys} {
        assert_equal {} [r command getkeys eval "return 1" 0]
    }

    test {COMMAND GETKEYS LCS} {
        assert_equal {key1 key2} [r command getkeys lcs key1 key2]
    }

    test {COMMAND GETKEYS MORE THAN 256 KEYS} {
        set all_keys [list]
        set numkeys 260
        for {set i 1} {$i <= $numkeys} {incr i} {
            lappend all_keys "key$i"
        }
        set all_keys_with_target [linsert $all_keys 0 target]
        # we are using ZUNIONSTORE command since in order to reproduce allocation of a new buffer in getKeysPrepareResult
        # when numkeys in result > 0
        # we need a command that the final number of keys is not known in the first call to getKeysPrepareResult
        # before the fix in that case data of old buffer was not copied to the new result buffer
        # causing all previous keys (numkeys) data to be uninitialize
        assert_equal $all_keys_with_target [r command getkeys ZUNIONSTORE target $numkeys {*}$all_keys]
    }

    test "COMMAND LIST syntax error" {
        assert_error "ERR syntax error*" {r command list bad_arg}
        assert_error "ERR syntax error*" {r command list filterby bad_arg}
        assert_error "ERR syntax error*" {r command list filterby bad_arg bad_arg2}
    }

    test "COMMAND LIST WITHOUT FILTERBY" {
        set commands [r command list]
        assert_not_equal [lsearch $commands "set"] -1
        assert_not_equal [lsearch $commands "client|list"] -1
    }

    test "COMMAND LIST FILTERBY ACLCAT against non existing category" {
        assert_equal {} [r command list filterby aclcat non_existing_category]
    }

    test "COMMAND LIST FILTERBY ACLCAT - list all commands/subcommands" {
        set commands [r command list filterby aclcat scripting]
        assert_not_equal [lsearch $commands "eval"] -1
        assert_not_equal [lsearch $commands "script|kill"] -1

        # Negative check, a command that should not be here
        assert_equal [lsearch $commands "set"] -1
    }

    test "COMMAND LIST FILTERBY PATTERN - list all commands/subcommands" {
        # Exact command match.
        assert_equal {set} [r command list filterby pattern set]
        assert_equal {get} [r command list filterby pattern get]

        # Return the parent command and all the subcommands below it.
        set commands [r command list filterby pattern config*]
        assert_not_equal [lsearch $commands "config"] -1
        assert_not_equal [lsearch $commands "config|get"] -1

        # We can filter subcommands under a parent command.
        set commands [r command list filterby pattern config|*re*]
        assert_not_equal [lsearch $commands "config|resetstat"] -1
        assert_not_equal [lsearch $commands "config|rewrite"] -1

        # We can filter subcommands across parent commands.
        set commands [r command list filterby pattern cl*help]
        assert_not_equal [lsearch $commands "client|help"] -1
        assert_not_equal [lsearch $commands "cluster|help"] -1

        # Negative check, command that doesn't exist.
        assert_equal {} [r command list filterby pattern non_exists]
        assert_equal {} [r command list filterby pattern non_exists*]
    }

    test "COMMAND LIST FILTERBY MODULE against non existing module" {
        # This should be empty, the real one is in subcommands.tcl
        assert_equal {} [r command list filterby module non_existing_module]
    }

    test {COMMAND INFO of invalid subcommands} {
        assert_equal {{}} [r command info get|key]
        assert_equal {{}} [r command info config|get|key]
    }

    foreach cmd {SET GET MSET BITFIELD LMOVE LPOP BLPOP PING MEMORY MEMORY|USAGE RENAME GEORADIUS_RO} {
        test "$cmd command will not be marked with movablekeys" {
            set info [lindex [r command info $cmd] 0]
            assert_no_match {*movablekeys*} [lindex $info 2]
        }
    }

    foreach cmd {ZUNIONSTORE XREAD EVAL SORT SORT_RO MIGRATE GEORADIUS} {
        test "$cmd command is marked with movablekeys" {
            set info [lindex [r command info $cmd] 0]
            assert_match {*movablekeys*} [lindex $info 2]
        }
    }

}
