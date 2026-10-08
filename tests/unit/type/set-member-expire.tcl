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
set ::sme_encodings {listpackex}

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
}

# ---------------------------------------------------------------------------
# Stage 4: the listpack with expirations encoding. Written as a matrix over
# ::sme_encodings, so every encoding added later runs the same tests.
# ---------------------------------------------------------------------------

start_server {tags {"external:skip needs:debug"}} {
    foreach enc $::sme_encodings {
        sme_force_encoding r $enc
        # Members are not numeric, so the sets start as listpacks.

        test "SEXPIRE family - Set the expiration of members, the set gets the expiration encoding ($enc)" {
            sme_create_set r myset {a b c}
            assert_encoding listpack myset
            assert_equal [r sexpire myset 1000 MEMBERS 2 a b] [list $E_OK $E_OK]
            assert_encoding $enc myset
            assert_equal [lsort [r smembers myset]] {a b c}
            assert_equal [r scard myset] 3
        }

        test "SEXPIRE family - All forms set the same kind of expiration ($enc)" {
            sme_create_set r myset {a b c d}
            set now_ms [clock milliseconds]
            assert_equal [r sexpire myset 1000 MEMBERS 1 a] [list $E_OK]
            assert_equal [r spexpire myset 1000000 MEMBERS 1 b] [list $E_OK]
            assert_equal [r sexpireat myset [expr {[clock seconds] + 1000}] MEMBERS 1 c] [list $E_OK]
            assert_equal [r spexpireat myset [expr {$now_ms + 1000000}] MEMBERS 1 d] [list $E_OK]
            foreach m {a b c d} {
                set ms [r spexpiretime myset MEMBERS 1 $m]
                assert {$ms >= $now_ms + 990000 && $ms <= $now_ms + 1010000}
                set sec [r sexpiretime myset MEMBERS 1 $m]
                assert {$sec >= $now_ms/1000 + 990 && $sec <= $now_ms/1000 + 1010}
                set pttl [r spttl myset MEMBERS 1 $m]
                assert {$pttl > 900000 && $pttl <= 1000000}
                set ttl [r sttl myset MEMBERS 1 $m]
                assert {$ttl > 900 && $ttl <= 1000}
            }
        }

        test "SEXPIRE family and the getters - Missing members and members without expiration ($enc)" {
            sme_create_set r myset {a b c}
            r sexpire myset 1000 MEMBERS 1 a
            assert_equal [r sexpire myset 1000 MEMBERS 3 nosuch b nosuch2] [list $E_NO_MEMBER $E_OK $E_NO_MEMBER]
            assert_equal [r sttl myset MEMBERS 3 c nosuch a] [list $E_NO_TTL $E_NO_MEMBER [r sttl myset MEMBERS 1 a]]
            foreach cmd $::sme_ttl_cmds {
                set res [r $cmd myset MEMBERS 3 c nosuch a]
                assert_equal [lrange $res 0 1] [list $E_NO_TTL $E_NO_MEMBER]
                assert {[lindex $res 2] > 0}
            }
        }

        test "SEXPIRE family - NX, XX, GT and LT are evaluated per member ($enc)" {
            sme_create_set r myset {a b c}
            set base [expr {[clock milliseconds] + 100000000}]
            r spexpireat myset $base MEMBERS 1 b
            # NX: only members without an expiration.
            assert_equal [r spexpireat myset [expr {$base + 5}] NX MEMBERS 3 a b nosuch] [list $E_OK $E_FAIL $E_NO_MEMBER]
            assert_equal [r spexpiretime myset MEMBERS 2 a b] [list [expr {$base + 5}] $base]
            # XX: only members that have an expiration.
            sme_create_set r myset {a b c}
            r spexpireat myset $base MEMBERS 1 b
            assert_equal [r spexpireat myset [expr {$base + 5}] XX MEMBERS 2 a b] [list $E_FAIL $E_OK]
            assert_equal [r spexpiretime myset MEMBERS 2 a b] [list $E_NO_TTL [expr {$base + 5}]]
            # GT: the new expiration is greater. No expiration counts as infinite, so it never is.
            sme_create_set r myset {a b c}
            r spexpireat myset $base MEMBERS 1 b
            assert_equal [r spexpireat myset [expr {$base + 1}] GT MEMBERS 2 a b] [list $E_FAIL $E_OK]
            assert_equal [r spexpireat myset [expr {$base + 1}] GT MEMBERS 1 b] [list $E_FAIL]
            assert_equal [r spexpireat myset [expr {$base - 1}] GT MEMBERS 1 b] [list $E_FAIL]
            # LT: the new expiration is less. No expiration counts as infinite, so it always is.
            sme_create_set r myset {a b c}
            r spexpireat myset $base MEMBERS 1 b
            assert_equal [r spexpireat myset [expr {$base - 1}] LT MEMBERS 2 a b] [list $E_OK $E_OK]
            assert_equal [r spexpireat myset [expr {$base - 1}] LT MEMBERS 1 b] [list $E_FAIL]
            assert_equal [r spexpireat myset [expr {$base + 1}] LT MEMBERS 1 b] [list $E_FAIL]
        }

        test "SEXPIRE family - An expiration in the past deletes the members ($enc)" {
            sme_create_set r myset {a b c}
            r sexpire myset 1000 MEMBERS 1 a
            assert_equal [r sexpireat myset 1 MEMBERS 2 a nosuch] [list $E_DELETED $E_NO_MEMBER]
            assert_equal [lsort [r smembers myset]] {b c}
            assert_equal [r spexpireat myset 1 MEMBERS 2 b c] [list $E_DELETED $E_DELETED]
            assert_equal [r exists myset] 0
        }

        test "SEXPIRE family - The set keeps its own key expiration ($enc)" {
            sme_create_set r myset {a b}
            r expire myset 1000
            r sexpire myset 100 MEMBERS 1 a
            assert {[r ttl myset] > 900}
            assert {[r ttl myset] <= 1000}
            r persist myset
            assert_equal [r ttl myset] -1
            assert {[r sttl myset MEMBERS 1 a] > 0}
        }

        test "SPERSIST - Removes the expiration of members ($enc)" {
            sme_create_set r myset {a b c}
            r sexpire myset 1000 MEMBERS 2 a b
            assert_equal [r spersist myset MEMBERS 4 a c nosuch b] [list $E_OK $E_NO_TTL $E_NO_MEMBER $E_OK]
            assert_equal [r sttl myset MEMBERS 3 a b c] [list $E_NO_TTL $E_NO_TTL $E_NO_TTL]
            # The set stays in the expiration encoding.
            assert_encoding $enc myset
            assert_equal [lsort [r smembers myset]] {a b c}
        }

        test "Members that are logically expired are treated as missing by the TTL commands ($enc)" {
            r debug set-active-expire 0
            sme_create_set r myset {a b c}
            r sexpire myset 1000 MEMBERS 1 c
            sme_make_expired r myset {a}
            # The expired member is still stored, and counted by SCARD.
            assert_equal [r scard myset] 3
            assert_equal [r sttl myset MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r spexpiretime myset MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r spersist myset MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r sexpire myset 1000 MEMBERS 1 a] [list $E_NO_MEMBER]
            assert_equal [r sexpire myset 1000 NX MEMBERS 1 a] [list $E_NO_MEMBER]
            # It was not renewed.
            assert_equal [r sttl myset MEMBERS 1 a] [list $E_NO_MEMBER]
            r debug set-active-expire 1
        }

        test "Members are ordered by expiration, whatever the order they were set in ($enc)" {
            sme_create_set r myset {a b c d e}
            set base [expr {[clock milliseconds] + 100000000}]
            r spexpireat myset [expr {$base + 50}] MEMBERS 1 c
            r spexpireat myset [expr {$base + 10}] MEMBERS 1 a
            r spexpireat myset [expr {$base + 30}] MEMBERS 1 e
            r spexpireat myset [expr {$base + 20}] MEMBERS 1 b
            assert_equal [r spexpiretime myset MEMBERS 5 a b c d e] \
                [list [expr {$base + 10}] [expr {$base + 20}] [expr {$base + 50}] $E_NO_TTL [expr {$base + 30}]]
            # Changing an expiration moves the member.
            r spexpireat myset [expr {$base + 5}] MEMBERS 1 c
            assert_equal [r spexpiretime myset MEMBERS 1 c] [expr {$base + 5}]
            r spersist myset MEMBERS 1 a
            assert_equal [lsort [r smembers myset]] {a b c d e}
        }

        test "SADDEX - Adds members with an expiration ($enc)" {
            r del myset
            assert_equal [r saddex myset EX 1000 MEMBERS 3 a b c] 1
            assert_encoding $enc myset
            assert_equal [lsort [r smembers myset]] {a b c}
            foreach m {a b c} {
                assert {[r sttl myset MEMBERS 1 $m] > 900}
            }
            # PX, EXAT and PXAT.
            r del myset
            r saddex myset PX 1000000 MEMBERS 1 a
            r saddex myset EXAT [expr {[clock seconds] + 1000}] MEMBERS 1 b
            r saddex myset PXAT [expr {[clock milliseconds] + 1000000}] MEMBERS 1 c
            foreach m {a b c} {
                assert {[r sttl myset MEMBERS 1 $m] > 900}
            }
        }

        test "SADDEX - An existing member gets the new expiration, or loses it without an option ($enc)" {
            sme_create_set r myset {a b c}
            r sexpire myset 1000 MEMBERS 3 a b c
            assert_equal [r saddex myset EX 5000 MEMBERS 1 a] 1
            assert {[r sttl myset MEMBERS 1 a] > 4000}
            # No expiration option discards the expiration...
            assert_equal [r saddex myset MEMBERS 1 b] 1
            assert_equal [r sttl myset MEMBERS 1 b] [list $E_NO_TTL]
            # ...unless KEEPTTL is given.
            assert_equal [r saddex myset KEEPTTL MEMBERS 2 c newone] 1
            assert {[r sttl myset MEMBERS 1 c] > 900}
            assert_equal [r sttl myset MEMBERS 1 newone] [list $E_NO_TTL]
        }

        test "SADDEX - MNX and MXX apply to the whole command ($enc)" {
            sme_create_set r myset {a b}
            assert_equal [r saddex myset MNX EX 1000 MEMBERS 2 a x] 0
            assert_equal [r sismember myset x] 0
            assert_equal [r saddex myset MNX EX 1000 MEMBERS 2 x y] 1
            assert_equal [lsort [r smembers myset]] {a b x y}
            assert_equal [r saddex myset MXX EX 1000 MEMBERS 2 a nosuch] 0
            assert_equal [r sismember myset nosuch] 0
            assert_equal [r saddex myset MXX EX 1000 MEMBERS 2 a b] 1
            assert {[r sttl myset MEMBERS 1 b] > 900}
        }

        test "SADDEX - An expiration in the past deletes existing members and adds none ($enc)" {
            sme_create_set r myset {a b c}
            assert_equal [r saddex myset PXAT 1 MEMBERS 2 a new] 1
            assert_equal [lsort [r smembers myset]] {b c}
            # A missing key is not created.
            r del myset
            assert_equal [r saddex myset PXAT 1 MEMBERS 2 a b] 1
            assert_equal [r exists myset] 0
            # Deleting every member deletes the key.
            sme_create_set r myset {a b}
            assert_equal [r saddex myset EXAT 1 MEMBERS 2 a b] 1
            assert_equal [r exists myset] 0
        }

        test "SADDEX - Without an expiration option it is SADD for sets that do not expire ($enc)" {
            r del myset
            assert_equal [r saddex myset MEMBERS 3 a b c] 1
            assert_equal [lsort [r smembers myset]] {a b c}
            assert_encoding listpack myset
        }

        test "A logically expired member is treated as new by SADDEX ($enc)" {
            r debug set-active-expire 0
            sme_create_set r myset {a b}
            r sexpire myset 1000 MEMBERS 1 b
            sme_make_expired r myset {a}
            assert_equal [r saddex myset MXX MEMBERS 2 a b] 0
            assert_equal [r saddex myset MNX MEMBERS 1 a] 1
            assert_equal [r sttl myset MEMBERS 1 a] [list $E_NO_TTL]
            r debug set-active-expire 1
        }

        test "Sets that would need a hashtable with expirations are not supported yet ($enc)" {
            sme_create_set r bigset {}
            for {set i 0} {$i < 200} {incr i} {r sadd bigset m$i}
            assert_encoding hashtable bigset
            assert_error {*not supported yet*} {r sexpire bigset 100 MEMBERS 1 m1}
            assert_error {*not supported yet*} {r saddex bigset EX 100 MEMBERS 1 new}
            # The set was not changed.
            assert_equal [r scard bigset] 200
            assert_encoding hashtable bigset
        }
    }
}

# Intset-encoded sets convert to the expiration encoding on their first expiration.
start_server {tags {"external:skip needs:debug"}} {
    test "An intset converts to the listpack with expirations on the first expiration" {
        r del myset
        r sadd myset 1 2 3 40
        assert_encoding intset myset
        assert_equal [r sexpire myset 1000 MEMBERS 2 2 40] [list $E_OK $E_OK]
        assert_encoding listpackex myset
        assert_equal [lsort -integer [r smembers myset]] {1 2 3 40}
        assert_equal [r sttl myset MEMBERS 2 1 40] [list $E_NO_TTL [r sttl myset MEMBERS 1 40]]
        assert {[r sttl myset MEMBERS 1 40] > 900}
    }

    test "A first expiration in the past or one that is not set does not convert the set" {
        r del myset
        r sadd myset a b c
        assert_equal [r sexpire myset 1000 XX MEMBERS 1 a] [list $E_FAIL]
        assert_equal [r sexpire myset 1000 MEMBERS 1 nosuch] [list $E_NO_MEMBER]
        assert_encoding listpack myset
        assert_equal [r sexpireat myset 1 MEMBERS 1 a] [list $E_DELETED]
        assert_encoding listpack myset
    }
}

start_server {tags {"external:skip needs:debug"}} {
    test "Keyspace notifications of the SME commands" {
        set db 9
        r config set notify-keyspace-events Ksg
        r del myset
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        r sadd myset a b c d
        r sexpire myset 1000 MEMBERS 2 a b
        r sexpire myset 1000 NX MEMBERS 1 a         ;# no member set: no event
        r spersist myset MEMBERS 2 a b
        r spersist myset MEMBERS 1 c                ;# nothing persisted: no event
        r sttl myset MEMBERS 1 a                    ;# reads fire nothing
        r sexpireat myset 1 MEMBERS 2 a b           ;# deleted by the command
        r saddex myset EX 1000 MEMBERS 1 x
        r saddex myset PXAT 1 MEMBERS 3 c d x       ;# deletes them all
        assert_equal "pmessage * __keyspace@${db}__:myset sadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset sexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset spersist" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset srem" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset sadd" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset sexpire" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset srem" [$rd1 read]
        assert_equal "pmessage * __keyspace@${db}__:myset del" [$rd1 read]
        $rd1 close
    }
}

start_server {tags {"external:skip needs:repl needs:debug"}} {
    test "SME commands propagate in canonical form" {
        set repl [attach_to_replication_stream]
        r sadd s1 a b c
        r sexpire s1 100 MEMBERS 2 a b
        r spexpireat s1 [expr {[clock milliseconds] + 100000}] NX MEMBERS 1 c
        r spersist s1 MEMBERS 2 a nosuch
        r spersist s1 MEMBERS 1 nosuch              ;# nothing changed: not propagated
        r sexpire s1 100 MEMBERS 3 a nosuch b       ;# only the members that were set
        r sexpireat s1 1 MEMBERS 1 c                ;# deleted: propagated as SREM
        r saddex s1 EX 100 MEMBERS 2 x y
        r saddex s1 PXAT [expr {[clock milliseconds] + 100000}] MEMBERS 1 z
        r saddex s1 PXAT 1 MEMBERS 2 x nosuch       ;# deletes: propagated as SREM
        r saddex s1 MNX MEMBERS 1 a                 ;# condition not met: nothing changes
        assert_replication_stream $repl {
            {select *}
            {sadd s1 a b c}
            {spexpireat s1 * MEMBERS 2 a b}
            {spexpireat s1 * NX MEMBERS 1 c}
            {spersist s1 MEMBERS 2 a nosuch}
            {spexpireat s1 * MEMBERS 2 a b}
            {srem s1 c}
            {saddex s1 PXAT * MEMBERS 2 x y}
            {saddex s1 PXAT * MEMBERS 1 z}
            {srem s1 x}
        }
        close_replication_stream $repl
    } {} {needs:repl}
}
