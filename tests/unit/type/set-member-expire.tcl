# Set member expiration (SME) tests.
#
# Mirrors tests/unit/type/hash-field-expire.tcl. The behavior tests are written
# once and run over an encoding matrix (see sme_encodings below), so a new
# encoding only has to be added to that list.
#
# Tests are of two kinds:
#  - permanent tests (errors, introspection, behavior);
#  - scaffolding tests that depend on temporary stubs (log lines or the
#    simulated-expired-member DEBUG switch). Scaffolding tests are marked with
#    "SCAFFOLDING" in their name and are deleted together with the stub they
#    cover (see the stub registry in the implementation plan).

# Encodings the behavior tests run over. Entries are added as the encodings are
# implemented (listpackex first, then hashtable).
set ::sme_encodings {}

# Reply codes shared by the SEXPIRE family, STTL family and SPERSIST.
set E_NO_MEMBER   -2
set E_NO_TTL      -1
set E_FAIL         0
set E_OK           1
set E_DELETED      2

# Force the encoding a freshly created set will get. "listpackex" relies on the
# default limits and small non-numeric members; "hashtable" is forced by making
# the listpack limit zero (as the set tests do).
proc sme_force_encoding {r enc} {
    if {$enc eq "hashtable"} {
        $r config set set-max-listpack-entries 0
    } else {
        $r config set set-max-listpack-entries 128
    }
}

# Create (or replace) a set with the given members.
proc sme_create_set {r key members} {
    $r del $key
    if {[llength $members] > 0} {
        $r sadd $key {*}$members
    }
}

# Make the given members logically expired but not yet reclaimed: give them a
# short TTL while active expiry is off, then let the server itself sleep so no
# client-side delay is involved. Live members must always use long TTLs, and
# tests must never assert that a short-TTL member is still alive.
# The caller is responsible for DEBUG SET-ACTIVE-EXPIRE 0 (and for restoring 1).
proc sme_make_expired {r key members {ttl_ms 20}} {
    $r spexpire $key $ttl_ms MEMBERS [llength $members] {*}$members
    $r debug sleep [expr {($ttl_ms * 2) / 1000.0 + 0.01}]
}

# ---------------------------------------------------------------------------
# Stage 2: the command layer. Parsing, validation and the replies that do not
# depend on stored expirations are real; what happens to an existing set is
# stubbed (see the SCAFFOLDING tests at the end).
# ---------------------------------------------------------------------------

set ::sme_set_expire_cmds {SEXPIRE SPEXPIRE SEXPIREAT SPEXPIREAT}
set ::sme_ttl_cmds {STTL SPTTL SEXPIRETIME SPEXPIRETIME}

start_server {tags {"external:skip needs:debug"}} {

    test "SEXPIRE family - Returns an array of -2 if the key does not exist" {
        r del myset
        foreach cmd $::sme_set_expire_cmds {
            assert_equal [r $cmd myset 1000 MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r $cmd myset 1000 MEMBERS 2 a b] [list $E_NO_MEMBER $E_NO_MEMBER]
            assert_equal [r $cmd myset 1000 NX MEMBERS 2 a b] [list $E_NO_MEMBER $E_NO_MEMBER]
        }
    }

    test "STTL family and SPERSIST - Return an array of -2 if the key does not exist" {
        r del myset
        foreach cmd [concat $::sme_ttl_cmds SPERSIST] {
            assert_equal [r $cmd myset MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r $cmd myset MEMBERS 3 a b c] [list $E_NO_MEMBER $E_NO_MEMBER $E_NO_MEMBER]
        }
    }

    test "SADDEX - MXX on a missing key replies 0 and does not create the key" {
        r del myset
        assert_equal [r saddex myset MXX MEMBERS 1 a] 0
        assert_equal [r exists myset] 0
    }

    test "All SME commands reply WRONGTYPE for keys of other types" {
        r flushall
        r set mystring v
        r rpush mylist a
        r hset myhash f v
        foreach key {mystring mylist myhash} {
            foreach cmd $::sme_set_expire_cmds {
                assert_error "WRONGTYPE*" {r $cmd $key 100 MEMBERS 1 a}
            }
            foreach cmd [concat $::sme_ttl_cmds SPERSIST] {
                assert_error "WRONGTYPE*" {r $cmd $key MEMBERS 1 a}
            }
            assert_error "WRONGTYPE*" {r saddex $key MEMBERS 1 a}
        }
    }

    test "SEXPIRE family - Verify the expire time does not overflow" {
        r del myset
        r sadd myset a
        # The expire time can't be negative.
        assert_error {ERR invalid expire time, must be >= 0} {r SEXPIRE myset -1 MEMBERS 1 a}
        assert_error {ERR invalid expire time, must be >= 0} {r SEXPIRE myset -9223372036854775808 MEMBERS 1 a}
        # The expire time can't be greater than the cap, the same cap as the hash fields.
        assert_error {ERR invalid expire time in 'sexpire' command} {r SEXPIRE myset [expr (1<<48) / 1000] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'sexpireat' command} {r SEXPIREAT myset [expr (1<<48) / 1000 + [clock seconds] + 100] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'spexpire' command} {r SPEXPIRE myset [expr (1<<48)] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'spexpireat' command} {r SPEXPIREAT myset [expr (1<<48) + [clock milliseconds] + 100] MEMBERS 1 a}
        assert_error {ERR value is not an integer or out of range} {r SEXPIRE myset notanumber MEMBERS 1 a}
    }

    test "SEXPIRE family - Argument errors" {
        r del myset
        r sadd myset a b c
        foreach cmd $::sme_set_expire_cmds {
            # Arity: key, time, MEMBERS, nummembers and one member are required.
            assert_error {*wrong number of arguments*} {r $cmd myset}
            assert_error {*wrong number of arguments*} {r $cmd myset 100}
            assert_error {*wrong number of arguments*} {r $cmd myset 100 MEMBERS}
            assert_error {*wrong number of arguments*} {r $cmd myset 100 MEMBERS 1}
            # The MEMBERS keyword is mandatory.
            assert_error {ERR missing MEMBERS argument} {r $cmd myset 100 NX NX NX}
            assert_error {ERR unknown argument: a} {r $cmd myset 100 a b c d}
            # Number of members.
            assert_error {ERR Parameter `numMembers` should be greater than 0} {r $cmd myset 100 MEMBERS 0 a}
            assert_error {ERR Parameter `numMembers` should be greater than 0} {r $cmd myset 100 MEMBERS -1 a}
            assert_error {ERR Parameter `numMembers` should be greater than 0} {r $cmd myset 100 MEMBERS x a}
            assert_error {ERR wrong number of arguments} {r $cmd myset 100 MEMBERS 3 a b}
            assert_error {ERR unknown argument: c} {r $cmd myset 100 MEMBERS 1 a c}
            # Repeated or conflicting keywords.
            assert_error {ERR MEMBERS keyword specified multiple times} {r $cmd myset 100 MEMBERS 1 a MEMBERS 1 b}
            assert_error {ERR Multiple condition flags specified} {r $cmd myset 100 NX XX MEMBERS 1 a}
            assert_error {ERR Multiple condition flags specified} {r $cmd myset 100 GT MEMBERS 1 a LT}
            assert_error {ERR unknown argument: BADOPT} {r $cmd myset 100 BADOPT MEMBERS 1 a}
        }
    }

    test "SEXPIRE family - Keywords can come in any order, as in the HEXPIRE family" {
        r del myset
        # A missing key makes the replies independent of the stored expirations.
        foreach cmd $::sme_set_expire_cmds {
            assert_equal [r $cmd myset 1000 NX MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r $cmd myset 1000 MEMBERS 1 a NX] [list $E_NO_MEMBER]
            assert_equal [r $cmd myset 1000 nx members 1 a] [list $E_NO_MEMBER]
        }
    }

    test "STTL family and SPERSIST - Argument errors" {
        r del myset
        r sadd myset a b
        foreach cmd [concat $::sme_ttl_cmds SPERSIST] {
            assert_error {*wrong number of arguments*} {r $cmd myset}
            assert_error {*wrong number of arguments*} {r $cmd myset MEMBERS}
            assert_error {*wrong number of arguments*} {r $cmd myset MEMBERS 1}
            assert_error {ERR Mandatory argument MEMBERS is missing or not at the right position} {r $cmd myset a 1 a}
            assert_error {ERR Number of members must be a positive integer} {r $cmd myset MEMBERS 0 a}
            assert_error {ERR Number of members must be a positive integer} {r $cmd myset MEMBERS -1 a}
            assert_error {ERR Number of members must be a positive integer} {r $cmd myset MEMBERS x a}
            assert_error {ERR The `nummembers` parameter must match the number of arguments} {r $cmd myset MEMBERS 2 a}
            assert_error {ERR The `nummembers` parameter must match the number of arguments} {r $cmd myset MEMBERS 1 a b}
        }
    }

    test "SADDEX - Minimum valid command and arity" {
        r del myset
        # The minimum command has 5 tokens (there is no value, unlike HSETEX).
        r saddex myset MXX MEMBERS 1 a
        r saddex myset MEMBERS 1 a
        assert_error {*wrong number of arguments for 'saddex' command*} {r saddex myset MEMBERS 1}
        assert_error {*wrong number of arguments for 'saddex' command*} {r saddex myset}
        assert_error {*wrong number of arguments for 'saddex' command*} {r saddex}
    }

    test "SADDEX - Argument errors" {
        r del myset
        r sadd myset a b
        assert_error {ERR missing MEMBERS argument} {r saddex myset EX 100 MXX}
        assert_error {ERR invalid number of members} {r saddex myset MEMBERS 0 a}
        assert_error {ERR invalid number of members} {r saddex myset MEMBERS x a}
        assert_error {ERR wrong number of arguments} {r saddex myset MEMBERS 3 a b}
        assert_error {ERR MEMBERS keyword specified multiple times} {r saddex myset MEMBERS 1 a MEMBERS 1 b}
        assert_error {ERR unknown argument: BAD} {r saddex myset BAD MEMBERS 1 a}
        assert_error {ERR unknown argument: c} {r saddex myset MEMBERS 1 a c}
        # Conflicting options.
        assert_error {ERR Only one of MXX or MNX arguments can be specified} {r saddex myset MXX MNX MEMBERS 1 a}
        assert_error {ERR Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified} {r saddex myset EX 100 PX 100 MEMBERS 1 a}
        assert_error {ERR Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified} {r saddex myset EX 100 KEEPTTL MEMBERS 1 a}
        assert_error {ERR Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified} {r saddex myset KEEPTTL EXAT 100 MEMBERS 1 a}
        assert_error {ERR missing expire time} {r saddex myset MEMBERS 1 a EX}
        # The old hash option names are not accepted.
        assert_error {ERR unknown argument: FNX} {r saddex myset FNX MEMBERS 1 a}
        assert_error {ERR unknown argument: FXX} {r saddex myset FXX MEMBERS 1 a}
    }

    test "SADDEX - Verify the expire time does not overflow" {
        r del myset
        assert_error {ERR invalid expire time, must be >= 0} {r saddex myset EX -1 MEMBERS 1 a}
        assert_error {ERR invalid expire time, must be >= 0} {r saddex myset PXAT -1 MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'saddex' command} {r saddex myset EX [expr (1<<48) / 1000] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'saddex' command} {r saddex myset PX [expr (1<<48)] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'saddex' command} {r saddex myset EXAT [expr (1<<48) / 1000 + [clock seconds] + 100] MEMBERS 1 a}
        assert_error {ERR invalid expire time in 'saddex' command} {r saddex myset PXAT [expr (1<<48) + [clock milliseconds] + 100] MEMBERS 1 a}
        assert_error {ERR value is not an integer or out of range} {r saddex myset EX notanumber MEMBERS 1 a}
        assert_equal [r exists myset] 0
    }

    test "SADDEX - Keywords can come in any order" {
        r del myset
        assert_equal [r saddex myset MXX EX 100 MEMBERS 1 a] 0
        assert_equal [r saddex myset EX 100 MXX MEMBERS 1 a] 0
        assert_equal [r saddex myset MEMBERS 1 a MXX EX 100] 0
        assert_equal [r exists myset] 0
    }

    test "SME commands - COMMAND INFO: arity, flags and key specs" {
        # name arity flags
        foreach {cmd arity flags} {
            sexpire -6 {write fast}
            spexpire -6 {write fast}
            sexpireat -6 {write fast}
            spexpireat -6 {write fast}
            spersist -5 {write fast}
            sttl -5 {readonly fast}
            spttl -5 {readonly fast}
            sexpiretime -5 {readonly fast}
            spexpiretime -5 {readonly fast}
            saddex -5 {write denyoom fast}
        } {
            set info [lindex [r command info $cmd] 0]
            assert_equal [lindex $info 0] $cmd
            assert_equal [lindex $info 1] $arity
            assert_equal [lsort [lindex $info 2]] [lsort $flags]
            # first key, last key, step
            assert_equal [lrange $info 3 5] {1 1 1}
        }
    }

    test "SME commands - COMMAND GETKEYS" {
        assert_equal [r command getkeys sexpire myset 100 MEMBERS 2 a b] {myset}
        assert_equal [r command getkeys sttl myset MEMBERS 1 a] {myset}
        assert_equal [r command getkeys spersist myset MEMBERS 1 a] {myset}
        assert_equal [r command getkeys saddex myset EX 100 MEMBERS 1 a] {myset}
    }

    test "SME commands - COMMAND DOCS: group, since and tips" {
        foreach cmd {sexpire spexpire sexpireat spexpireat spersist sttl spttl sexpiretime spexpiretime saddex} {
            set docs [dict get [r command docs $cmd] $cmd]
            assert_equal [dict get $docs group] set
            assert_equal [dict get $docs since] 8.12.0
        }
        # Only the relative remaining time getters are nondeterministic.
        foreach {cmd nondet} {sttl 1 spttl 1 sexpiretime 0 spexpiretime 0} {
            set info [lindex [r command info $cmd] 0]
            set tips [lindex $info 7]
            assert_equal [expr {[lsearch -exact $tips nondeterministic_output] >= 0}] $nondet
        }
    }

    test "SME commands - ACL categories" {
        set set_cmds [r acl cat set]
        set fast_cmds [r acl cat fast]
        foreach cmd {sexpire spexpire sexpireat spexpireat spersist sttl spttl sexpiretime spexpiretime saddex} {
            assert {[lsearch -exact $set_cmds $cmd] >= 0}
            assert {[lsearch -exact $fast_cmds $cmd] >= 0}
        }
        set write_cmds [r acl cat write]
        set read_cmds [r acl cat read]
        foreach cmd {sexpire spexpire sexpireat spexpireat spersist saddex} {
            assert {[lsearch -exact $write_cmds $cmd] >= 0}
        }
        foreach cmd {sttl spttl sexpiretime spexpiretime} {
            assert {[lsearch -exact $read_cmds $cmd] >= 0}
        }
    }

    test "SME commands - ACL permissions by command and by category" {
        r acl setuser smeuser on nopass +@read ~* +select
        r acl setuser smewriter on nopass +@set ~* +select
        set rd [redis_client]
        $rd auth smeuser pass
        assert_equal [$rd sttl nokey MEMBERS 1 a] [list $E_NO_MEMBER]
        assert_error {*NOPERM*} {$rd sexpire nokey 100 MEMBERS 1 a}
        assert_error {*NOPERM*} {$rd saddex nokey MEMBERS 1 a}
        $rd close
        set wr [redis_client]
        $wr auth smewriter pass
        assert_equal [$wr sexpire nokey 100 MEMBERS 1 a] [list $E_NO_MEMBER]
        $wr close
        r acl deluser smeuser smewriter
    }

    test "SME commands are listed in the set group" {
        set cmds [r command list filterby aclcat set]
        foreach cmd {sexpire spexpire sexpireat spexpireat spersist sttl spttl sexpiretime spexpiretime saddex} {
            assert {[lsearch -exact $cmds $cmd] >= 0}
        }
    }

    # ----------------------------------------------------------------------
    # SCAFFOLDING: tests that read the stub log lines. They are deleted when the
    # stubs are replaced by the real implementation (Stage 4).
    # ----------------------------------------------------------------------

    test "SCAFFOLDING - SEXPIRE family passes members, time and condition to the stub" {
        r del myset
        r sadd myset a b c
        set from [count_log_lines 0]
        set before [clock milliseconds]
        r sexpire myset 100 NX MEMBERS 2 b a
        set after [clock milliseconds]
        wait_for_log_messages 0 {"*SME-STUB setExpire key=myset member=b *flags=1*" "*SME-STUB setExpire key=myset member=a *flags=1*"} $from 10 100
        # The time is absolute, in milliseconds.
        set lines [exec grep "SME-STUB setExpire key=myset member=b " [srv 0 stdout]]
        regexp {expire_ms=(\d+)} [lindex [split $lines "\n"] end] -> ms
        assert {$ms >= $before + 100000 && $ms <= $after + 100000}
    }

    test "SCAFFOLDING - SEXPIREAT and SPEXPIREAT pass the absolute time unchanged" {
        r del myset
        r sadd myset a
        set from [count_log_lines 0]
        r sexpireat myset 2000000000 MEMBERS 1 a
        r spexpireat myset 2000000000123 XX MEMBERS 1 a
        wait_for_log_messages 0 {"*SME-STUB setExpire key=myset member=a expire_ms=2000000000000 flags=0*" "*SME-STUB setExpire key=myset member=a expire_ms=2000000000123 flags=2*"} $from 10 100
    }

    test "SCAFFOLDING - Conditions are passed as flags" {
        r del myset
        r sadd myset a
        set from [count_log_lines 0]
        r spexpireat myset 2000000000000 GT MEMBERS 1 a
        r spexpireat myset 2000000000000 LT MEMBERS 1 a
        wait_for_log_messages 0 {"*SME-STUB setExpire key=myset member=a expire_ms=2000000000000 flags=4*" "*SME-STUB setExpire key=myset member=a expire_ms=2000000000000 flags=8*"} $from 10 100
    }

    test "SCAFFOLDING - STTL family and SPERSIST reach the stub once per member, in order" {
        r del myset
        r sadd myset a b
        set from [count_log_lines 0]
        r sttl myset MEMBERS 2 a b
        r spexpiretime myset MEMBERS 1 b
        r spersist myset MEMBERS 2 b a
        wait_for_log_messages 0 {
            "*SME-STUB getTtlSeconds key=myset member=a *"
            "*SME-STUB getTtlSeconds key=myset member=b *"
            "*SME-STUB getExpireTimeMilliseconds key=myset member=b *"
            "*SME-STUB persist key=myset member=b *"
            "*SME-STUB persist key=myset member=a *"
        } $from 10 100
    }

    test "SCAFFOLDING - SADDEX passes members, time and option flags to the stub" {
        r del myset
        r sadd myset a
        set from [count_log_lines 0]
        r saddex myset MNX PXAT 2000000000000 MEMBERS 2 x y
        wait_for_log_messages 0 {"*SME-STUB add key=myset member=x expire_ms=2000000000000 flags=72*" "*SME-STUB add key=myset member=y expire_ms=2000000000000 flags=72*"} $from 10 100
    }

    test "SCAFFOLDING - Nothing reaches the stub for missing keys or parse errors" {
        r del myset
        set from [count_log_lines 0]
        set stubs_before [count_message_lines [srv 0 stdout] "SME-STUB.*key=myset member=a"]
        r sexpire myset 100 MEMBERS 1 a
        catch {r sexpire myset 100 MEMBERS 2 a}
        r saddex myset MXX MEMBERS 1 a
        # Sentinel: a later stub line proves the log was flushed up to here.
        r sadd other x
        r sttl other MEMBERS 1 x
        wait_for_log_messages 0 {"*SME-STUB getTtlSeconds key=other*"} $from 10 100
        assert_equal [count_message_lines [srv 0 stdout] "SME-STUB.*key=myset member=a"] $stubs_before
    }
}
