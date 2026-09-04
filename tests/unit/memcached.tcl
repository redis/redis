# Tests for the memcached text protocol listener (see src/memcached.c).
#
# These drive a raw socket rather than a Redis client, since the point is the
# wire format. Where memcached and Redis disagree the tests assert memcached's
# behaviour, because that is what a memcached client will expect.

# ---------------------------------------------------------------------------
# Raw protocol helpers
# ---------------------------------------------------------------------------

proc mc_connect {port} {
    set fd [socket 127.0.0.1 $port]
    fconfigure $fd -translation binary -blocking 1
    return $fd
}

proc mc_gets {fd} {
    set line [gets $fd]
    return [string trimright $line "\r"]
}

# Send a command and collect the whole response.
#
# Returns a list of lines. A VALUE header is followed by its data block as a
# separate element, so `get foo` on a 3 byte item yields
# {{VALUE foo 0 3} bar END}.
proc mc {fd args} {
    puts -nonewline $fd "[join $args { }]\r\n"
    flush $fd
    return [mc_read $fd]
}

# Like `mc`, but for the storage commands, which carry a data block.
proc mc_store {fd header data} {
    puts -nonewline $fd "$header\r\n$data\r\n"
    flush $fd
    return [mc_read $fd]
}

# Send raw bytes verbatim, for the malformed-input tests.
proc mc_raw {fd bytes} {
    puts -nonewline $fd $bytes
    flush $fd
    return [mc_read $fd]
}

proc mc_read {fd} {
    set out {}
    while 1 {
        set line [mc_gets $fd]
        lappend out $line
        # VALUE and STAT lines are continuations; anything else ends the
        # response (END terminates a retrieval or a stats section).
        if {[string match "VALUE *" $line]} {
            set len [lindex [split $line " "] 3]
            lappend out [read $fd $len]
            read $fd 2 ;# the CRLF after the data block
            continue
        }
        if {[string match "STAT *" $line]} continue
        break
    }
    return $out
}

# Wrappers for the commands whose response is a single line, so that tests can
# compare against a plain string instead of a one element Tcl list.
proc mc1 {fd args} {
    return [lindex [mc $fd {*}$args] 0]
}

proc mc_store1 {fd header data} {
    return [lindex [mc_store $fd $header $data] 0]
}

proc mc_raw1 {fd bytes} {
    return [lindex [mc_raw $fd $bytes] 0]
}

# Read whatever is already buffered without blocking, for noreply tests.
proc mc_read_nonblocking {fd} {
    fconfigure $fd -blocking 0
    after 100
    set data [read $fd]
    fconfigure $fd -blocking 1
    return $data
}

set mcport [find_available_port $::baseport $::portcount]

start_server [list tags {"memcached" "external:skip"} \
                   overrides [list memcached-port $mcport]] {

    set fd [mc_connect $mcport]

    # The memcached port is pinned to database 0, so the Redis client used to
    # cross-check the keyspace has to look there rather than at the test
    # suite's usual database 9.
    r select 0

    test {MEMCACHED: version and unknown commands} {
        assert_equal {VERSION 1.6.0} [mc1 $fd version]
        assert_equal {ERROR} [mc1 $fd frobnicate]
    }

    test {MEMCACHED: set and get, with flags} {
        assert_equal {STORED} [mc_store1 $fd "set foo 42 0 3" "bar"]
        assert_equal {{VALUE foo 42 3} bar END} [mc $fd get foo]
    }

    test {MEMCACHED: get of a missing key is just END} {
        assert_equal {END} [mc1 $fd get nosuchkey]
    }

    test {MEMCACHED: multi-get returns only the keys that exist} {
        mc_store $fd "set mg1 1 0 1" "a"
        mc_store $fd "set mg2 2 0 1" "b"
        assert_equal {{VALUE mg1 1 1} a {VALUE mg2 2 1} b END} \
            [mc $fd get mg1 missing mg2]
    }

    test {MEMCACHED: gets reports a cas token of 0} {
        assert_equal {{VALUE foo 42 3 0} bar END} [mc $fd gets foo]
    }

    test {MEMCACHED: cas is rejected rather than silently ignored} {
        assert_equal {CLIENT_ERROR cas is not supported} \
            [mc_store1 $fd "cas foo 0 0 1 12345" "z"]
        # The data block was consumed, so the stream is still in sync.
        assert_equal {VERSION 1.6.0} [mc1 $fd version]
    }

    test {MEMCACHED: add only stores when the key is absent} {
        assert_equal {NOT_STORED} [mc_store1 $fd "add foo 0 0 1" "x"]
        assert_equal {STORED} [mc_store1 $fd "add fresh 7 0 5" "hello"]
        assert_equal {{VALUE fresh 7 5} hello END} [mc $fd get fresh]
    }

    test {MEMCACHED: replace only stores when the key is present} {
        assert_equal {NOT_STORED} [mc_store1 $fd "replace absent 0 0 1" "z"]
        assert_equal {STORED} [mc_store1 $fd "replace fresh 9 0 2" "hi"]
        assert_equal {{VALUE fresh 9 2} hi END} [mc $fd get fresh]
    }

    test {MEMCACHED: append and prepend refuse to create a key} {
        assert_equal {NOT_STORED} [mc_store1 $fd "append absent 0 0 1" "z"]
        assert_equal {NOT_STORED} [mc_store1 $fd "prepend absent 0 0 1" "z"]
    }

    test {MEMCACHED: append and prepend keep the existing flags} {
        mc_store $fd "set cat 77 0 3" "mid"
        assert_equal {STORED} [mc_store1 $fd "append cat 0 0 3" "end"]
        assert_equal {STORED} [mc_store1 $fd "prepend cat 0 0 3" "beg"]
        assert_equal {{VALUE cat 77 9} begmidend END} [mc $fd get cat]
    }

    test {MEMCACHED: delete reports whether the key was there} {
        assert_equal {DELETED} [mc1 $fd delete cat]
        assert_equal {NOT_FOUND} [mc1 $fd delete cat]
    }

    test {MEMCACHED: delete accepts the legacy zero time argument} {
        mc_store $fd "set legacy 0 0 1" "a"
        assert_equal {DELETED} [mc1 $fd delete legacy 0]
    }

    # ---------------- incr / decr ----------------

    test {MEMCACHED: incr and decr use unsigned 64 bit arithmetic} {
        mc_store $fd "set n 0 0 2" "10"
        assert_equal {15} [mc1 $fd incr n 5]
        assert_equal {12} [mc1 $fd decr n 3]
    }

    test {MEMCACHED: decr saturates at zero instead of going negative} {
        mc_store $fd "set n 0 0 1" "5"
        assert_equal {0} [mc1 $fd decr n 100]
    }

    test {MEMCACHED: incr wraps around at 64 bits} {
        mc_store $fd "set n 0 0 20" "18446744073709551615"
        assert_equal {0} [mc1 $fd incr n 1]
        assert_equal {18446744073709551615} [mc1 $fd incr n 18446744073709551615]
    }

    test {MEMCACHED: incr on a missing key is NOT_FOUND, not a create} {
        assert_equal {NOT_FOUND} [mc1 $fd incr neverseen 1]
        assert_equal {NOT_FOUND} [mc1 $fd decr neverseen 1]
        assert_equal {END} [mc1 $fd get neverseen]
    }

    test {MEMCACHED: incr on a non-numeric value is a client error} {
        mc_store $fd "set word 0 0 3" "abc"
        assert_equal {CLIENT_ERROR cannot increment or decrement non-numeric value} \
            [mc1 $fd incr word 1]
    }

    test {MEMCACHED: a bad delta is reported separately from a bad value} {
        assert_equal {CLIENT_ERROR invalid numeric delta argument} \
            [mc1 $fd incr n xyz]
        assert_equal {CLIENT_ERROR invalid numeric delta argument} \
            [mc1 $fd incr n -1]
    }

    test {MEMCACHED: incr keeps the item flags and its expiry} {
        mc_store $fd "set ctr 55 100 1" "1"
        assert_equal {6} [mc1 $fd incr ctr 5]
        assert_equal {{VALUE ctr 55 1} 6 END} [mc $fd get ctr]
        assert_range [r ttl ctr] 90 100
    }

    # ---------------- expiry ----------------

    test {MEMCACHED: exptime 0 means no expiry} {
        mc_store $fd "set noexp 0 0 1" "a"
        assert_equal -1 [r ttl noexp]
    }

    test {MEMCACHED: a small exptime is a delta in seconds} {
        mc_store $fd "set rel 0 100 1" "a"
        assert_range [r ttl rel] 90 100
    }

    test {MEMCACHED: exptime above 30 days is an absolute unix time} {
        # 2592000 is the last value treated as a delta.
        mc_store $fd "set boundary 0 2592000 1" "a"
        assert_range [r ttl boundary] 2591000 2592000

        set future [expr {[clock seconds] + 1000}]
        mc_store $fd "set abs 0 $future 1" "a"
        assert_range [r ttl abs] 900 1000

        # 2592001 seconds since the epoch is in 1970, so already expired.
        assert_equal {STORED} [mc_store1 $fd "set past 0 2592001 1" "a"]
        assert_equal 0 [r exists past]
    }

    test {MEMCACHED: a negative exptime stores an already expired item} {
        mc_store $fd "set gone 0 0 1" "a"
        assert_equal {STORED} [mc_store1 $fd "set gone 0 -1 1" "b"]
        assert_equal 0 [r exists gone]
        assert_equal {END} [mc1 $fd get gone]
    }

    # ---------------- touch / gat ----------------

    test {MEMCACHED: touch sets the expiry and reports hit or miss} {
        mc_store $fd "set t1 0 0 1" "a"
        assert_equal {TOUCHED} [mc1 $fd touch t1 100]
        assert_range [r ttl t1] 90 100
        assert_equal {NOT_FOUND} [mc1 $fd touch nosuchkey 100]
    }

    test {MEMCACHED: touch with 0 clears an existing expiry} {
        mc_store $fd "set t2 0 100 1" "a"
        assert_equal {TOUCHED} [mc1 $fd touch t2 0]
        assert_equal -1 [r ttl t2]
    }

    test {MEMCACHED: touch with a negative time removes the item} {
        mc_store $fd "set t3 0 0 1" "a"
        assert_equal {TOUCHED} [mc1 $fd touch t3 -1]
        assert_equal 0 [r exists t3]
    }

    test {MEMCACHED: gat returns the value and re-expires it} {
        mc_store $fd "set g1 3 0 2" "hi"
        assert_equal {{VALUE g1 3 2} hi END} [mc $fd gat 100 g1]
        assert_range [r ttl g1] 90 100
        assert_equal {{VALUE g1 3 2 0} hi END} [mc $fd gats 0 g1]
        assert_equal -1 [r ttl g1]
        assert_equal {END} [mc1 $fd gat 100 nosuchkey]
    }

    # ---------------- shared keyspace ----------------

    test {MEMCACHED: items are ordinary Redis strings} {
        mc_store $fd "set shared 0 0 5" "value"
        assert_equal {value} [r get shared]
        assert_equal {string} [r type shared]
    }

    test {MEMCACHED: Redis strings are readable as items, with flags 0} {
        r set fromredis "written by redis"
        assert_equal {{VALUE fromredis 0 16} {written by redis} END} \
            [mc $fd get fromredis]
    }

    test {MEMCACHED: a Redis overwrite drops the item flags} {
        mc_store $fd "set flagged 123 0 1" "a"
        assert_equal {{VALUE flagged 123 1} a END} [mc $fd get flagged]
        r set flagged b
        assert_equal {{VALUE flagged 0 1} b END} [mc $fd get flagged]
    }

    test {MEMCACHED: an in-place Redis update keeps the item flags} {
        # APPEND and INCR update the value of an existing item rather than
        # replacing the item, so the flags still describe it.
        mc_store1 $fd "set inplace 33 0 1" "1"
        r append inplace 2
        assert_equal {{VALUE inplace 33 2} 12 END} [mc $fd get inplace]
        r incr inplace
        assert_equal {{VALUE inplace 33 2} 13 END} [mc $fd get inplace]
    }

    test {MEMCACHED: an expired item drops its flags} {
        mc_store1 $fd "set expflags 44 0 1" "a"
        r pexpire expflags 1
        wait_for_condition 50 20 {
            [mc1 $fd get expflags] eq {END}
        } else {
            fail "the item did not expire"
        }
        r set expflags b
        assert_equal {{VALUE expflags 0 1} b END} [mc $fd get expflags]
    }

    test {MEMCACHED: deleting a key drops its flags, so a reuse starts at 0} {
        mc_store $fd "set reused 9 0 1" "a"
        mc $fd delete reused
        r set reused b
        assert_equal {{VALUE reused 0 1} b END} [mc $fd get reused]
    }

    test {MEMCACHED: the port is pinned to database 0} {
        mc_store $fd "set db0only 0 0 1" "a"
        r select 1
        assert_equal 0 [r exists db0only]
        r select 0
        assert_equal 1 [r exists db0only]
    }

    test {MEMCACHED: non-string keys are refused, not asserted on} {
        r del alist
        r rpush alist a b c
        assert_equal {SERVER_ERROR key holds a value that is not a string} \
            [mc1 $fd get alist]
        assert_equal {SERVER_ERROR key holds a value that is not a string} \
            [mc_store1 $fd "set alist 0 0 1" "x"]
        assert_equal {SERVER_ERROR key holds a value that is not a string} \
            [mc1 $fd incr alist 1]
        assert_equal {SERVER_ERROR key holds a value that is not a string} \
            [mc1 $fd delete alist]
        assert_equal {SERVER_ERROR key holds a value that is not a string} \
            [mc1 $fd touch alist 10]
        assert_equal 3 [r llen alist]
    }

    # ---------------- protocol handling ----------------

    test {MEMCACHED: noreply suppresses the response} {
        puts -nonewline $fd "set quiet 0 0 2 noreply\r\nok\r\n"
        flush $fd
        assert_equal "" [mc_read_nonblocking $fd]
        assert_equal {{VALUE quiet 0 2} ok END} [mc $fd get quiet]
    }

    test {MEMCACHED: noreply suppresses errors too} {
        puts -nonewline $fd "incr nosuchkey 1 noreply\r\n"
        flush $fd
        assert_equal "" [mc_read_nonblocking $fd]
        assert_equal {VERSION 1.6.0} [mc1 $fd version]
    }

    test {MEMCACHED: commands can be pipelined in a single write} {
        puts -nonewline $fd "set p1 1 0 1\r\na\r\nset p2 2 0 1\r\nb\r\nversion\r\n"
        flush $fd
        assert_equal {STORED} [lindex [mc_read $fd] 0]
        assert_equal {STORED} [lindex [mc_read $fd] 0]
        assert_equal {VERSION 1.6.0} [lindex [mc_read $fd] 0]
    }

    test {MEMCACHED: a mis-declared data block length is a bad data chunk} {
        # Declares 3 bytes, so the terminator is looked for at offset 3 and
        # found to be "de". Exactly bytes+2 are consumed either way, which is
        # what keeps the stream aligned with the client's own framing.
        assert_equal {CLIENT_ERROR bad data chunk} \
            [mc_raw1 $fd "set wrong 0 0 3\r\nabcde"]
        assert_equal 0 [r exists wrong]
        assert_equal {VERSION 1.6.0} [mc1 $fd version]
    }

    test {MEMCACHED: a malformed storage header is a client error} {
        assert_equal {CLIENT_ERROR bad command line format} \
            [mc_raw1 $fd "set nobytes 0 0\r\n"]
        assert_equal {CLIENT_ERROR bad command line format} \
            [mc_raw1 $fd "set badflags notanumber 0 1\r\n"]
    }

    test {MEMCACHED: keys longer than 250 bytes are rejected} {
        set longkey [string repeat k 251]
        assert_equal {CLIENT_ERROR bad command line format} [mc1 $fd get $longkey]
        set maxkey [string repeat k 250]
        assert_equal {END} [mc1 $fd get $maxkey]
    }

    test {MEMCACHED: items larger than 1MB are refused and the body discarded} {
        set big [string repeat x [expr {1024*1024 + 1}]]
        puts -nonewline $fd "set toobig 0 0 [string length $big]\r\n"
        flush $fd
        assert_equal {SERVER_ERROR object too large for cache} [lindex [mc_read $fd] 0]
        puts -nonewline $fd "$big\r\n"
        flush $fd
        # The discarded body must not be mistaken for commands.
        assert_equal {VERSION 1.6.0} [mc1 $fd version]
        assert_equal 0 [r exists toobig]
    }

    test {MEMCACHED: an item of exactly 1MB is accepted} {
        set ok [string repeat y [expr {1024*1024}]]
        assert_equal {STORED} \
            [mc_store1 $fd "set justright 0 0 [string length $ok]" $ok]
        assert_equal [string length $ok] [r strlen justright]
        r del justright
    }

    test {MEMCACHED: a value may contain CRLF and NUL bytes} {
        # Built piecewise because Tcl's \x escape is greedy about hex digits.
        set payload "a\r\nb"
        append payload [format %c 0]
        append payload "c"
        assert_equal {STORED} \
            [mc_store1 $fd "set binary 0 0 [string length $payload]" $payload]
        assert_equal {{VALUE binary 0 6}} [lrange [mc $fd get binary] 0 0]
        assert_equal $payload [r get binary]
    }

    # ---------------- stats, flush_all ----------------

    test {MEMCACHED: stats reports memcached counters} {
        set lines [mc $fd stats]
        assert_equal {END} [lindex $lines end]
        set fields {}
        foreach line [lrange $lines 0 end-1] {
            assert_match "STAT *" $line
            lappend fields [lindex [split $line " "] 1]
        }
        foreach expected {pid uptime version curr_connections cmd_get cmd_set
                          get_hits get_misses delete_hits delete_misses
                          incr_hits decr_hits touch_hits curr_items
                          limit_maxbytes evictions} {
            assert {[lsearch -exact $fields $expected] != -1}
        }
    }

    test {MEMCACHED: stats reset clears the counters} {
        mc $fd get somekey
        assert_equal {RESET} [mc1 $fd stats reset]
        set lines [mc $fd stats]
        foreach line $lines {
            if {[string match "STAT cmd_get *" $line]} {
                assert_equal 0 [lindex [split $line " "] 2]
            }
        }
    }

    test {MEMCACHED: stats subcommands about slabs answer with an empty section} {
        assert_equal {END} [mc1 $fd stats slabs]
        assert_equal {END} [mc1 $fd stats items]
    }

    test {MEMCACHED: a delayed flush_all is refused rather than done now} {
        mc_store $fd "set keepme 0 0 1" "a"
        assert_equal {CLIENT_ERROR delayed flush_all is not supported} \
            [mc1 $fd flush_all 5]
        assert_equal 1 [r exists keepme]
    }

    test {MEMCACHED: flush_all empties database 0, Redis keys included} {
        mc_store $fd "set willgo 0 0 1" "a"
        r set redistoo b
        assert_equal {OK} [mc1 $fd flush_all]
        assert_equal {END} [mc1 $fd get willgo]
        assert_equal 0 [r dbsize]
    }

    test {MEMCACHED: flush_all 0 is accepted as an immediate flush} {
        mc_store $fd "set willgo2 0 0 1" "a"
        assert_equal {OK} [mc1 $fd flush_all 0]
        assert_equal 0 [r dbsize]
    }

    # ---------------- propagation ----------------

    test {MEMCACHED: writes propagate as equivalent Redis commands} {
        set repl [attach_to_replication_stream]
        mc_store $fd "set prop 1 0 1" "a"
        mc_store $fd "set propttl 1 100 1" "b"
        mc_store $fd "set num 0 0 1" "1"
        mc $fd incr num 4
        mc_store $fd "append num 0 0 1" "9"
        mc $fd touch prop 100
        mc $fd touch prop 0
        mc $fd delete prop
        assert_replication_stream $repl {
            {select 0}
            {set prop a}
            {set propttl b PXAT *}
            {set num 1}
            {set num 5}
            {set num 59}
            {pexpireat prop *}
            {persist prop}
            {del prop}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {MEMCACHED: reads and misses propagate nothing} {
        set repl [attach_to_replication_stream]
        mc $fd get num
        mc $fd get nosuchkey
        mc $fd incr nosuchkey 1
        mc $fd delete nosuchkey
        r set trailing 1
        assert_replication_stream $repl {
            {select *}
            {set trailing 1}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {MEMCACHED: flush_all propagates a FLUSHDB} {
        set repl [attach_to_replication_stream]
        mc $fd flush_all
        assert_replication_stream $repl {
            {select *}
            {flushdb}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    # ---------------- keyspace notifications and WATCH ----------------

    test {MEMCACHED: writes fire keyspace notifications} {
        r config set notify-keyspace-events KEA
        set rd [redis_deferring_client]
        $rd psubscribe __keyevent@0__:*
        $rd read

        mc_store $fd "set notif 0 0 1" "a"
        assert_equal {__keyevent@0__:set notif} [lrange [$rd read] 2 3]

        $rd close
    }

    test {MEMCACHED: writes invalidate WATCH on the same key} {
        set rd [redis_client]
        $rd select 0
        $rd watch watched
        $rd multi
        $rd get watched
        mc_store $fd "set watched 0 0 1" "a"
        assert_equal {} [$rd exec]
        $rd close
    }

    test {MEMCACHED: quit closes the connection} {
        set fd2 [mc_connect $mcport]
        puts -nonewline $fd2 "quit\r\n"
        flush $fd2
        assert_equal "" [read $fd2]
        close $fd2
    }

    close $fd
}

# ---------------------------------------------------------------------------
# Startup guards
# ---------------------------------------------------------------------------

start_server {tags {"memcached" "external:skip"}} {
    test {MEMCACHED: the port defaults to disabled} {
        assert_equal {memcached-port 0} [r config get memcached-port]
        assert_equal {memcached-insecure-allow-noauth no} \
            [r config get memcached-insecure-allow-noauth]
    }

    test {MEMCACHED: memcached-port cannot be changed at runtime} {
        assert_error "*can't set immutable config*" \
            {r config set memcached-port 11211}
    }
}

test {MEMCACHED: the server refuses to start with cluster mode enabled} {
    set port [find_available_port $::baseport $::portcount]
    set mcport [find_available_port $::baseport $::portcount]
    set stdout [format "%s/%s" [tmpdir server.memcached] "cluster.stdout"]
    exec src/redis-server --port $port --memcached-port $mcport \
        --cluster-enabled yes --dir [tmpdir server.memcached] > $stdout 2>@1 &
    wait_for_condition 50 100 {
        [string match "*cluster mode is enabled*" [exec cat $stdout]]
    } else {
        fail "The server did not refuse memcached-port under cluster mode"
    }
}

test {MEMCACHED: the server refuses an unauthenticated port when a password is set} {
    set port [find_available_port $::baseport $::portcount]
    set mcport [find_available_port $::baseport $::portcount]
    set stdout [format "%s/%s" [tmpdir server.memcached] "auth.stdout"]
    exec src/redis-server --port $port --memcached-port $mcport \
        --requirepass hunter2 --dir [tmpdir server.memcached] > $stdout 2>@1 &
    wait_for_condition 50 100 {
        [string match "*memcached-insecure-allow-noauth*" [exec cat $stdout]]
    } else {
        fail "The server did not refuse memcached-port alongside requirepass"
    }
}

set mcport [find_available_port $::baseport $::portcount]
start_server [list tags {"memcached" "external:skip"} \
                   overrides [list memcached-port $mcport \
                                   requirepass "hunter2" \
                                   memcached-insecure-allow-noauth "yes"]] {
    test {MEMCACHED: the insecure opt-in allows the port alongside requirepass} {
        r auth hunter2
        set fd [mc_connect $mcport]
        assert_equal {STORED} [mc_store1 $fd "set noauth 0 0 2" "hi"]
        assert_equal {{VALUE noauth 0 2} hi END} [mc $fd get noauth]
        close $fd
    }
}
