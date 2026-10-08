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
set ::sme_encodings {listpackex hashtable}

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
    r debug keysizes-hist-assert 1
    foreach enc $::sme_encodings {
        sme_force_encoding r $enc
        # Members are not numeric, so the sets start as listpacks, or as hashtables
        # when the listpack limit is zero.
        set plain_enc [expr {$enc eq "hashtable" ? "hashtable" : "listpack"}]

        test "SEXPIRE family - Set the expiration of members, the set gets the expiration encoding ($enc)" {
            sme_create_set r myset {a b c}
            assert_encoding $plain_enc myset
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
            assert_encoding $plain_enc myset
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

# ---------------------------------------------------------------------------
# Stage 4: lazy expiration of the members, by the existing set commands
# ---------------------------------------------------------------------------

start_server {tags {"external:skip needs:debug"}} {
    r debug keysizes-hist-assert 1
    foreach enc $::sme_encodings {
        sme_force_encoding r $enc
        set db 9

        # A set with members x1 x2 x3 (live, long expiration), e1 e2 (expired, not removed).
        proc sme_mixed_set {r {live {x1 x2 x3}} {expired {e1 e2}}} {
            $r debug set-active-expire 0
            $r del myset
            $r sadd myset {*}$live {*}$expired
            $r sexpire myset 1000 MEMBERS [llength $live] {*}$live
            sme_make_expired $r myset $expired
        }

        test "SISMEMBER and SMISMEMBER - A logically expired member is not a member, and is removed ($enc)" {
            sme_mixed_set r
            assert_equal [r scard myset] 5
            assert_equal [r sismember myset x1] 1
            assert_equal [r sismember myset e1] 0
            assert_equal [r scard myset] 4
            assert_equal [r smismember myset e2 x2 nosuch] {0 1 0}
            assert_equal [r scard myset] 3
            assert_equal [lsort [r smembers myset]] {x1 x2 x3}
            r debug set-active-expire 1
        }

        test "SISMEMBER - Removing the last member, expired, deletes the key ($enc)" {
            r debug set-active-expire 0
            sme_create_set r myset {a}
            sme_make_expired r myset {a}
            assert_equal [r exists myset] 1
            assert_equal [r sismember myset a] 0
            assert_equal [r exists myset] 0
            r debug set-active-expire 1
        }

        test "SMEMBERS and SSCAN - Skip the expired members and do not remove them ($enc)" {
            sme_mixed_set r
            assert_equal [lsort [r smembers myset]] {x1 x2 x3}
            lassign [r sscan myset 0] cursor members
            assert_equal $cursor 0
            assert_equal [lsort $members] {x1 x2 x3}
            lassign [r sscan myset 0 match e*] cursor members
            assert_equal $members {}
            lassign [r sscan myset 0 match x*] cursor members
            assert_equal [lsort $members] {x1 x2 x3}
            # Nothing was removed.
            assert_equal [r scard myset] 5
            r debug set-active-expire 1
        }

        test "SCARD is approximate: it counts the expired members that are not removed yet ($enc)" {
            sme_mixed_set r
            assert_equal [r scard myset] 5
            r debug set-active-expire 1
        }

        test "SADD - A logically expired member is added as a new member ($enc)" {
            sme_mixed_set r
            assert_equal [r sadd myset e1 x1 new] 2
            assert_equal [r sttl myset MEMBERS 3 e1 x1 new] [list $E_NO_TTL [r sttl myset MEMBERS 1 x1] $E_NO_TTL]
            assert_equal [r scard myset] 6
            r debug set-active-expire 1
        }

        test "SREM - Removes a logically expired member and counts it ($enc)" {
            sme_mixed_set r
            assert_equal [r srem myset e1 nosuch x1] 2
            assert_equal [r scard myset] 3
            r debug set-active-expire 1
        }

        test "SPOP - Never returns an expired member and removes them first ($enc)" {
            foreach cmd {{spop myset} {spop myset 2} {spop myset 3} {spop myset 100}} {
                sme_mixed_set r
                set res [r {*}$cmd]
                foreach m $res {
                    assert {[lsearch {x1 x2 x3} $m] >= 0}
                }
                # The expired members are gone, whatever was popped.
                assert_equal [llength [r sinter myset myset]] [expr {3 - [llength $res]}]
                if {[r exists myset]} {assert {[r scard myset] <= 3}}
            }
            r debug set-active-expire 1
        }

        test "SPOP - A set with only expired members is deleted ($enc)" {
            r debug set-active-expire 0
            sme_create_set r myset {a b}
            sme_make_expired r myset {a b}
            assert_equal [r spop myset] {}
            assert_equal [r exists myset] 0
            sme_create_set r myset {a b}
            sme_make_expired r myset {a b}
            assert_equal [r spop myset 2] {}
            assert_equal [r exists myset] 0
            r debug set-active-expire 1
        }

        test "SPOP with a count keeps the expirations of the members that remain ($enc)" {
            # CASE 3 (the move strategy) builds a new set with the remaining members.
            r debug set-active-expire 0
            r del myset
            for {set i 0} {$i < 20} {incr i} {r sadd myset m$i}
            for {set i 0} {$i < 20} {incr i} {r spexpireat myset [expr {[clock milliseconds] + 100000 + $i}] MEMBERS 1 m$i}
            set popped [r spop myset 18]
            assert_equal [llength $popped] 18
            assert_equal [r scard myset] 2
            foreach m [r smembers myset] {
                assert {[r sttl myset MEMBERS 1 $m] > 0}
            }
            assert_encoding $enc myset
            r debug set-active-expire 1
        }

        test "SRANDMEMBER - Never returns an expired member, in every form ($enc)" {
            sme_mixed_set r
            for {set i 0} {$i < 20} {incr i} {
                foreach m [concat [r srandmember myset 3] [r srandmember myset -10] [r srandmember myset 100] [r srandmember myset 1] [list [r srandmember myset]]] {
                    assert {[lsearch {x1 x2 x3} $m] >= 0}
                }
                sme_mixed_set r
            }
            r debug set-active-expire 1
        }

        test "SRANDMEMBER - A set with only expired members is deleted ($enc)" {
            r debug set-active-expire 0
            sme_create_set r myset {a b}
            sme_make_expired r myset {a b}
            assert_equal [r srandmember myset] {}
            assert_equal [r exists myset] 0
            sme_create_set r myset {a b}
            sme_make_expired r myset {a b}
            assert_equal [r srandmember myset 5] {}
            assert_equal [r exists myset] 0
            r debug set-active-expire 1
        }

        test "SINTER, SUNION, SDIFF and their variants - Ignore the expired members ($enc)" {
            sme_mixed_set r
            r del other
            r sadd other x1 e1 e2 y
            # e1 and e2 are expired in myset, so they are not members of it.
            assert_equal [lsort [r sinter myset other]] {x1}
            assert_equal [lsort [r sunion myset other]] {e1 e2 x1 x2 x3 y}
            assert_equal [lsort [r sdiff myset other]] {x2 x3}
            assert_equal [lsort [r sdiff other myset]] {e1 e2 y}
            assert_equal [r sintercard 2 myset other] 1
            assert_equal [r sunioncard 2 myset other] 6
            assert_equal [r sdiffcard 2 myset other] 2
            assert_equal [r sinterstore dst myset other] 1
            assert_equal [r sunionstore dst myset other] 6
            assert_equal [r sdiffstore dst myset other] 2
            assert_equal [lsort [r smembers dst]] {x2 x3}
            # The sources were not changed.
            assert_equal [r scard myset] 5
            r debug set-active-expire 1
        }

        test "SMOVE - The member moves with its expiration ($enc)" {
            r del src dst
            r sadd src a b
            r sadd dst z
            r sexpire src 1000 MEMBERS 1 a
            assert_equal [r smove src dst a] 1
            assert {[r sttl dst MEMBERS 1 a] > 900}
            assert_equal [r sttl dst MEMBERS 1 z] $E_NO_TTL
            assert_equal [r sismember src a] 0
            # A member without an expiration overwrites the expiration of the one at the destination.
            r sadd src c
            r sexpire dst 1000 MEMBERS 1 z
            r sadd src z
            assert_equal [r smove src dst z] 1
            assert_equal [r sttl dst MEMBERS 1 z] $E_NO_TTL
            # A member with an expiration overwrites the expiration of the one at the destination.
            r sadd src a
            r sexpire src 5000 MEMBERS 1 a
            assert_equal [r smove src dst a] 1
            assert {[r sttl dst MEMBERS 1 a] > 4000}
        }

        test "SMOVE - A logically expired source member is not moved, and a destination one is replaced ($enc)" {
            r debug set-active-expire 0
            r del src dst
            sme_create_set r src {a b}
            sme_create_set r dst {a z}
            sme_make_expired r src {a}
            assert_equal [r smove src dst a] 0
            assert_equal [r sismember dst a] 1
            assert_equal [r scard src] 1
            # An expired destination member is treated as new.
            sme_create_set r src {a b}
            r sexpire src 1000 MEMBERS 1 a
            sme_create_set r dst {a z}
            sme_make_expired r dst {a}
            assert_equal [r smove src dst a] 1
            assert {[r sttl dst MEMBERS 1 a] > 900}
            r debug set-active-expire 1
        }

        test "SMOVE - Events for the expiration of the moved member ($enc)" {
            r config set notify-keyspace-events Ksg
            set rd1 [redis_deferring_client]
            assert_equal {1} [psubscribe $rd1 *]
            # Reads the events of one move: the ones of the keys src and dst, up to a
            # sentinel event that is fired right after the move.
            proc drainEvents {r rd} {
                $r sadd sentinel x
                while {1} {
                    set ev [$rd read]
                    if {[string match "*:sentinel *" $ev]} break
                }
                $r del sentinel
                $rd read ;# the del of the sentinel
            }
            proc moveEvents {r rd cmd} {
                drainEvents $r $rd
                $r {*}$cmd
                $r sadd sentinel x
                set events {}
                while {1} {
                    set ev [$rd read]   ;# pmessage pattern __keyspace@9__:key event
                    set key [lindex [split [lindex $ev 2] :] 1]
                    set e [lindex $ev 3]
                    if {$key eq "sentinel"} break
                    lappend events "$key $e"
                }
                $r del sentinel
                $rd read ;# the del of the sentinel
                return $events
            }
            r del src dst
            r sadd src a b c d
            r sadd dst z y
            r sexpire src 1000 MEMBERS 2 a b
            r sexpire dst 1000 MEMBERS 1 y
            assert_equal [moveEvents r $rd1 {smove src dst a}] {{src srem} {dst sadd} {dst sexpire}}
            assert_equal [moveEvents r $rd1 {smove src dst c}] {{src srem} {dst sadd}}
            # Existing at the destination: the expiration of the source member replaces it.
            r sadd src y z
            r sexpire src 3000 MEMBERS 1 y
            assert_equal [moveEvents r $rd1 {smove src dst y}] {{src srem} {dst sexpire}}
            # Existing at the destination with an expiration, and none at the source: spersist.
            r sadd dst w
            r sexpire dst 2000 MEMBERS 1 w
            r sadd src w
            assert_equal [moveEvents r $rd1 {smove src dst w}] {{src srem} {dst spersist}}
            # Existing at the destination, with no expiration anywhere: nothing.
            assert_equal [moveEvents r $rd1 {smove src dst z}] {{src srem}}
            $rd1 close
        }
    }
}

# A replica reports the expired members as missing and never removes them itself.
start_server {tags {"external:skip needs:repl needs:debug"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]
        $replica replicaof $master_host $master_port
        wait_for_condition 50 100 {[lindex [$replica role] 0] eq {slave} && [string match {*connected*} [$replica role]]} else {fail "no sync"}

        test "A replica hides the expired members but only the master removes them" {
            $master debug set-active-expire 0
            $replica debug set-active-expire 0
            $master del myset
            $master sadd myset a b c d
            $master sexpire myset 1000 MEMBERS 2 c d
            $master spexpire myset 30 MEMBERS 2 a b
            wait_for_ofs_sync $master $replica
            after 100
            # The expiration is decided by the master: the replica only hides them.
            assert_equal [$replica sismember myset a] 0
            assert_equal [$replica smismember myset a c] {0 1}
            assert_equal [lsort [$replica smembers myset]] {c d}
            assert_equal [$replica scard myset] 4
            for {set i 0} {$i < 30} {incr i} {
                foreach m [concat [$replica srandmember myset 2] [$replica srandmember myset -5] [list [$replica srandmember myset]]] {
                    assert {$m eq "c" || $m eq "d"}
                }
            }
            assert_equal [lsort [$replica srandmember myset 10]] {c d}
            assert_equal [$replica sttl myset MEMBERS 2 a c] [list $E_NO_MEMBER [$replica sttl myset MEMBERS 1 c]]
            # Still there.
            assert_equal [$replica scard myset] 4
            # The master removes the member when it finds it, and the replica follows.
            assert_equal [$master sismember myset a] 0
            wait_for_ofs_sync $master $replica
            assert_equal [$replica scard myset] 3
            $master debug set-active-expire 1
            $replica debug set-active-expire 1
        }
    }
}

# The allocation size accounting must be exact (this run enables its assertion).
start_server {tags {"external:skip needs:debug"} overrides {key-memory-histograms yes}} {
    r debug keysizes-hist-assert 1
    r debug allocsize-slots-assert 1

    foreach enc $::sme_encodings {
    sme_force_encoding r $enc
    test "SME commands keep the allocation size accounting exact ($enc)" {
        r debug set-active-expire 0
        r flushall
        r sadd s1 a b c d e f
        r sexpire s1 1000 MEMBERS 3 a b c
        r spersist s1 MEMBERS 1 a
        r saddex s1 EX 1000 MEMBERS 2 g h
        r saddex s1 MEMBERS 1 g
        r sexpireat s1 1 MEMBERS 1 h
        sme_make_expired r s1 {b}
        r sismember s1 b
        r sadd s1 c
        r sadd src x y z
        r sexpire src 1000 MEMBERS 1 x
        r smove src s1 x
        r spop s1 2
        r srandmember s1 3
        r sadd s2 1 2 3
        r sexpire s2 1000 MEMBERS 1 2
        r del s1 s2 src
        assert_equal [r dbsize] 0
        r debug set-active-expire 1
    }
    }
}

start_server {tags {"external:skip needs:repl needs:debug"}} {
    r debug keysizes-hist-assert 1
    foreach enc $::sme_encodings {
        sme_force_encoding r $enc

        test "Lazy expiration propagates an explicit SREM and counts the expired members ($enc)" {
            r debug set-active-expire 0
            r del myset other
            set before [s expired_subkeys]
            # The stream is attached before the sets exist: the sets with expirations
            # cannot be saved to the RDB of the full sync yet.
            set repl [attach_to_replication_stream]
            r sadd myset a b c d
            r sadd other x y
            r sexpire myset 1000 MEMBERS 1 d
            r sexpire other 1000 MEMBERS 1 y
            r spexpire myset 10 MEMBERS 1 a
            r spexpire myset 20 MEMBERS 1 b
            r spexpire myset 30 MEMBERS 1 c
            r spexpire other 10 MEMBERS 1 x
            r debug sleep 0.1
            r sismember myset a                 ;# lazy removal of a single member
            r spop myset                        ;# removes b and c first, then pops d
            r srandmember other                 ;# removes x
            assert_replication_stream $repl {
                {select *}
                {sadd myset a b c d}
                {sadd other x y}
                {spexpireat myset * MEMBERS 1 d}
                {spexpireat other * MEMBERS 1 y}
                {spexpireat myset * MEMBERS 1 a}
                {spexpireat myset * MEMBERS 1 b}
                {spexpireat myset * MEMBERS 1 c}
                {spexpireat other * MEMBERS 1 x}
                {srem myset a}
                {multi}
                {srem myset b}
                {srem myset c}
                {srem myset d}
                {exec}
                {srem other x}
            }
            r debug set-active-expire 1
            assert_equal [expr {[s expired_subkeys] - $before}] 4
            assert_equal [r exists myset] 0
            close_replication_stream $repl
        } {} {needs:repl}
    }
}

# ---------------------------------------------------------------------------
# Stage 5: active expiration and the key lifecycle
# ---------------------------------------------------------------------------

# The number of objects (hashes and sets) registered in subexpires, in all the databases.
proc sme_subexpiry {r} {
    set total 0
    foreach line [split [$r info keyspace] \n] {
        if {[regexp {subexpiry=(\d+)} $line -> value]} {incr total $value}
    }
    return $total
}

start_server {tags {"external:skip needs:debug"}} {
    r debug keysizes-hist-assert 1
    foreach enc $::sme_encodings {
        sme_force_encoding r $enc

        test "Active expiration removes the expired members without any command touching the set ($enc)" {
            r flushall
            r debug set-active-expire 1
            set before [s expired_subkeys_active]
            sme_create_set r myset {a b c d}
            r spexpire myset 50 MEMBERS 2 a b
            assert_equal [sme_subexpiry r] 1
            wait_for_condition 100 20 {[r scard myset] == 2} else {fail "the members did not expire"}
            assert_equal [lsort [r smembers myset]] {c d}
            assert_equal [expr {[s expired_subkeys_active] - $before}] 2
            # There is nothing left to expire: the set is not registered anymore.
            wait_for_condition 50 20 {[sme_subexpiry r] == 0} else {fail "still registered"}
            assert_encoding $enc myset
        }

        test "Active expiration deletes the key when the last member expires ($enc)" {
            r flushall
            sme_create_set r myset {a b}
            r spexpire myset 50 MEMBERS 2 a b
            wait_for_condition 100 20 {[r exists myset] == 0} else {fail "the key did not expire"}
            assert_equal [sme_subexpiry r] 0
        }

        test "The registration follows the earliest member expiration ($enc)" {
            r flushall
            r debug set-active-expire 0
            sme_create_set r myset {a b c}
            assert_equal [sme_subexpiry r] 0
            r sexpire myset 1000 MEMBERS 1 a
            assert_equal [sme_subexpiry r] 1
            # An earlier expiration moves it earlier, and the set expires on time.
            r spexpire myset 50 MEMBERS 1 b
            assert_equal [sme_subexpiry r] 1
            r debug set-active-expire 1
            wait_for_condition 100 20 {[r scard myset] == 2} else {fail "b did not expire"}
            assert_equal [lsort [r smembers myset]] {a c}
            # Removing the last expiration unregisters the set.
            r spersist myset MEMBERS 1 a
            assert_equal [sme_subexpiry r] 0
            r sexpire myset 1000 MEMBERS 1 a
            assert_equal [sme_subexpiry r] 1
            r saddex myset EX 1000 MEMBERS 1 z
            assert_equal [sme_subexpiry r] 1
        }

        test "Active expiration sends the events and propagates one SREM per member ($enc)" {
            r flushall
            r config set notify-keyspace-events Ksg
            set rd1 [redis_deferring_client]
            assert_equal {1} [psubscribe $rd1 *]
            set repl [attach_to_replication_stream]
            r debug set-active-expire 1
            r sadd myset a b c
            r spexpire myset 50 MEMBERS 1 a
            r spexpire myset 55 MEMBERS 1 b
            r spexpire myset 60 MEMBERS 1 c
            # sadd, sexpire (three times), then the members expire, and the key is deleted.
            assert_equal "pmessage * __keyspace@9__:myset sadd" [$rd1 read]
            assert_equal "pmessage * __keyspace@9__:myset sexpire" [$rd1 read]
            assert_equal "pmessage * __keyspace@9__:myset sexpire" [$rd1 read]
            assert_equal "pmessage * __keyspace@9__:myset sexpire" [$rd1 read]
            assert_equal "pmessage * __keyspace@9__:myset sexpired" [$rd1 read]
            set ev [$rd1 read]
            if {[string match "*sexpired" $ev]} {set ev [$rd1 read]}
            assert_equal "pmessage * __keyspace@9__:myset del" $ev
            $rd1 close
            assert_replication_stream $repl {
                {select *}
                {sadd myset a b c}
                {spexpireat myset * MEMBERS 1 a}
                {spexpireat myset * MEMBERS 1 b}
                {spexpireat myset * MEMBERS 1 c}
                {srem myset a}
                {srem myset b}
                {srem myset c}
            }
            close_replication_stream $repl
        } {} {needs:repl}

        test "Active expiration handles many sets, hashes and sets together ($enc)" {
            r flushall
            r debug set-active-expire 1
            for {set i 0} {$i < 100} {incr i} {
                r sadd set$i a b c
                r spexpire set$i [expr {50 + $i}] MEMBERS 2 a b
                r hset hash$i f1 v1 f2 v2
                r hpexpire hash$i [expr {50 + $i}] FIELDS 1 f1
            }
            assert_equal [sme_subexpiry r] 200
            wait_for_condition 200 20 {[sme_subexpiry r] == 0} else {fail "not all expired"}
            for {set i 0} {$i < 100} {incr i} {
                assert_equal [r smembers set$i] c
                assert_equal [r hlen hash$i] 1
            }
        }

        test "A set with a near and a far expiration only loses the near one ($enc)" {
            r flushall
            sme_create_set r myset {a b}
            r spexpire myset 50 MEMBERS 1 a
            r sexpire myset 1000 MEMBERS 1 b
            wait_for_condition 100 20 {[r scard myset] == 1} else {fail "a did not expire"}
            assert_equal [r smembers myset] b
            assert_equal [sme_subexpiry r] 1
        }

        test "RENAME, MOVE and COPY keep the expirations of the members and the registration ($enc)" {
            r flushall
            r debug set-active-expire 0
            sme_create_set r myset {a b}
            r sexpire myset 1000 MEMBERS 1 a
            assert_equal [sme_subexpiry r] 1
            r rename myset renamed
            assert_equal [sme_subexpiry r] 1
            assert {[r sttl renamed MEMBERS 1 a] > 900}
            r copy renamed copied
            assert_equal [sme_subexpiry r] 2
            assert {[r sttl copied MEMBERS 1 a] > 900}
            assert_encoding $enc copied
            r move renamed 10
            assert_equal [sme_subexpiry r] 2
            r select 10
            assert {[r sttl renamed MEMBERS 1 a] > 900}
            r select 9
            # They all still expire, in both databases.
            r select 10
            r spexpire renamed 50 MEMBERS 1 a
            r select 9
            r spexpire copied 50 MEMBERS 1 a
            r debug set-active-expire 1
            wait_for_condition 100 20 {[r sismember copied a] == 0 && [r scard copied] == 1} else {fail "copied did not expire"}
            r select 10
            wait_for_condition 100 20 {[r scard renamed] == 1} else {fail "renamed did not expire"}
            r select 9
            wait_for_condition 50 20 {[sme_subexpiry r] == 0} else {fail "still registered"}
        }

        test "DEL, UNLINK, overwriting and flushing unregister the sets ($enc)" {
            r flushall
            foreach i {1 2 3 4 5} {
                sme_create_set r s$i {a b}
                r sexpire s$i 1000 MEMBERS 1 a
            }
            assert_equal [sme_subexpiry r] 5
            r del s1
            r unlink s2
            assert_equal [sme_subexpiry r] 3
            r set s3 string
            assert_equal [sme_subexpiry r] 2
            r sadd plain x
        r sunionstore s4 plain   ;# overwritten by a result without expirations
            assert_equal [sme_subexpiry r] 1
            r flushdb
            assert_equal [sme_subexpiry r] 0
            sme_create_set r s1 {a}
            r sexpire s1 1000 MEMBERS 1 a
            r flushall async
            assert_equal [sme_subexpiry r] 0
            sme_create_set r s1 {a}
            r sexpire s1 1000 MEMBERS 1 a
            r select 10
            sme_create_set r s2 {a}
            r sexpire s2 1000 MEMBERS 1 a
            r swapdb 9 10
            assert_equal [sme_subexpiry r] 2
            r select 9
            assert {[r sttl s2 MEMBERS 1 a] > 900}
            r flushall
        }

        test "The key expiration and the member expirations are independent ($enc)" {
            r flushall
            sme_create_set r myset {a b}
            r sexpire myset 1000 MEMBERS 1 a
            r expire myset 2000
            assert_equal [sme_subexpiry r] 1
            assert {[r sttl myset MEMBERS 1 a] > 900}
            r persist myset
            assert_equal [sme_subexpiry r] 1
            r expire myset 1
            r pexpire myset 50
            wait_for_condition 100 20 {[r exists myset] == 0} else {fail "the key did not expire"}
            assert_equal [sme_subexpiry r] 0
        }

        test "SADDEX, SMOVE and SPOP register the sets they create ($enc)" {
            r flushall
            r debug set-active-expire 0
            r saddex s1 EX 1000 MEMBERS 2 a b
            assert_equal [sme_subexpiry r] 1
            r sadd src x y
            r sexpire src 1000 MEMBERS 2 x y
            r smove src s2 x              ;# creates the destination
            assert_equal [sme_subexpiry r] 3
            assert {[r sttl s2 MEMBERS 1 x] > 900}
            # SPOP with a count that rebuilds the set (the move strategy) keeps it registered.
            set members {}
            for {set i 0} {$i < 30} {incr i} {lappend members m$i}
            r sadd big {*}$members
            r sexpire big 1000 MEMBERS 30 {*}$members
            assert_equal [sme_subexpiry r] 4
            assert_equal [llength [r spop big 28]] 28
            assert_equal [sme_subexpiry r] 4
            assert_equal [r scard big] 2
            foreach m [r smembers big] {
                assert {[r sttl big MEMBERS 1 $m] > 900}
            }
            assert_encoding $enc big
            r debug set-active-expire 1
        }
    }
}

# Active expiration in a cluster node: the sets live in different slots.
start_cluster 1 0 {tags {external:skip cluster needs:debug}} {
    test "Active expiration works for sets in different slots of a cluster node" {
        R 0 debug set-active-expire 1
        for {set i 0} {$i < 50} {incr i} {
            R 0 sadd "{slot$i}set" a b c
            R 0 spexpire "{slot$i}set" [expr {50 + $i}] MEMBERS 2 a b
        }
        assert_equal [sme_subexpiry [Rn 0]] 50
        # The active expiration of a cluster node is slower: it goes over the slots (the
        # same is true for hash fields).
        wait_for_condition 750 20 {[sme_subexpiry [Rn 0]] == 0} else {fail "not all expired"}
        for {set i 0} {$i < 50} {incr i} {
            assert_equal [R 0 smembers "{slot$i}set"] c
        }
    }
}

# ---------------------------------------------------------------------------
# Stage 6 and 7: the hashtable with expirations, and the conversions
# ---------------------------------------------------------------------------

start_server {tags {"external:skip needs:debug"}} {
    r debug keysizes-hist-assert 1

    test "A big hashtable set gets expirations: only the members that get one change" {
        r flushall
        r config set set-max-listpack-entries 128
        set members {}
        for {set i 0} {$i < 5000} {incr i} {lappend members member:$i}
        r sadd big {*}$members
        assert_encoding hashtable big
        assert_equal [r sexpire big 1000 MEMBERS 3 member:7 member:4999 member:2500] [list $E_OK $E_OK $E_OK]
        assert_encoding hashtable big
        assert_equal [r scard big] 5000
        assert_equal [lsort [r smembers big]] [lsort $members]
        assert_equal [sme_subexpiry r] 1
        assert {[r sttl big MEMBERS 1 member:7] > 900}
        assert_equal [r sttl big MEMBERS 2 member:8 nosuch] [list $E_NO_TTL $E_NO_MEMBER]
        # Removing the expirations keeps the encoding (a set does not give them up).
        assert_equal [r spersist big MEMBERS 3 member:7 member:4999 member:2500] [list $E_OK $E_OK $E_OK]
        assert_encoding hashtable big
        assert_equal [sme_subexpiry r] 0
        assert_equal [lsort [r smembers big]] [lsort $members]
    }

    test "Hashtable members of every length keep their content and expiration" {
        r flushall
        r config set set-max-listpack-entries 0
        set lens {1 5 30 31 32 100 254 255 256 1000 65535 65536 70000}
        set members {}
        foreach len $lens {lappend members [string repeat x $len]:[expr {$len % 7}]}
        # Binary safe members too.
        lappend members "a\x00b" "\xff\xfe\x00" ""
        r sadd myset {*}$members
        assert_encoding hashtable myset
        set with {}
        set i 0
        foreach m $members {
            if {$i % 2 == 0} {lappend with $m}
            incr i
        }
        set base [expr {[clock milliseconds] + 100000000}]
        set n 0
        foreach m $with {
            r spexpireat myset [expr {$base + $n}] MEMBERS 1 $m
            incr n
        }
        assert_equal [lsort [r smembers myset]] [lsort $members]
        set n 0
        foreach m $with {
            assert_equal [r spexpiretime myset MEMBERS 1 $m] [expr {$base + $n}]
            incr n
        }
        foreach m $members {
            assert_equal [r sismember myset $m] 1
        }
        # Remove and add expirations in a different order.
        foreach m $with {r spersist myset MEMBERS 1 $m}
        foreach m [lreverse $members] {r spexpireat myset [expr {$base + 5}] MEMBERS 1 $m}
        assert_equal [lsort [r smembers myset]] [lsort $members]
        foreach m $members {
            assert_equal [r spexpiretime myset MEMBERS 1 $m] [expr {$base + 5}]
        }
        assert_equal [r srem myset {*}$members] [llength $members]
        assert_equal [r exists myset] 0
        assert_equal [sme_subexpiry r] 0
        r config set set-max-listpack-entries 128
    }

    test "Hashtable with many expirations in random order expire completely" {
        r flushall
        r config set set-max-listpack-entries 0
        r debug set-active-expire 0
        set members {}
        for {set i 0} {$i < 2000} {incr i} {lappend members m$i}
        r sadd myset {*}$members
        set now [clock milliseconds]
        foreach m [lshuffle $members] {
            r spexpireat myset [expr {$now + 3000 + int(rand() * 2000)}] MEMBERS 1 $m
        }
        # Add more members while the dict grows (it rehashes).
        for {set i 2000} {$i < 6000} {incr i} {r sadd myset m$i}
        assert_equal [r scard myset] 6000
        for {set i 0} {$i < 100} {incr i} {
            assert {[r sttl myset MEMBERS 1 m$i] > 0}
        }
        r debug set-active-expire 1
        wait_for_condition 300 50 {[r scard myset] == 4000} else {fail "the members did not expire"}
        assert_equal [r scard myset] 4000
        assert_equal [sme_subexpiry r] 0
        assert_equal [r sismember myset m5] 0
        assert_equal [r sismember myset m5000] 1
        r config set set-max-listpack-entries 128
    }

    test "A set that grows past the listpack limits converts to a hashtable and keeps the expirations" {
        r flushall
        r config set set-max-listpack-entries 128
        r debug set-active-expire 0
        set base [expr {[clock milliseconds] + 100000000}]
        for {set i 0} {$i < 128} {incr i} {r sadd myset m$i}
        for {set i 0} {$i < 128} {incr i 2} {r spexpireat myset [expr {$base + $i}] MEMBERS 1 m$i}
        # Exactly at the limit: a listpack with expirations (the limit counts members).
        assert_encoding listpackex myset
        assert_equal [sme_subexpiry r] 1
        assert_equal [r sadd myset one-more] 1
        assert_encoding hashtable myset
        assert_equal [r scard myset] 129
        assert_equal [sme_subexpiry r] 1
        for {set i 0} {$i < 128} {incr i} {
            if {$i % 2 == 0} {
                assert_equal [r spexpiretime myset MEMBERS 1 m$i] [expr {$base + $i}]
            } else {
                assert_equal [r sttl myset MEMBERS 1 m$i] $E_NO_TTL
            }
        }
        assert_equal [r sttl myset MEMBERS 1 one-more] $E_NO_TTL
        # The registration moved with the set: it still expires.
        r spexpire myset 50 MEMBERS 1 one-more
        r debug set-active-expire 1
        wait_for_condition 100 20 {[r scard myset] == 128} else {fail "did not expire after the conversion"}
        assert_equal [sme_subexpiry r] 1
    }

    test "A member longer than the listpack value limit converts a set with expirations" {
        r flushall
        r config set set-max-listpack-value 64
        r sadd myset a b c
        r sexpire myset 1000 MEMBERS 2 a b
        assert_encoding listpackex myset
        r sadd myset [string repeat x 65]
        assert_encoding hashtable myset
        assert {[r sttl myset MEMBERS 1 a] > 900}
        assert_equal [sme_subexpiry r] 1
    }

    test "SADDEX and the first expiration on big sets use a hashtable" {
        r flushall
        r config set set-max-listpack-entries 128
        set members {}
        for {set i 0} {$i < 300} {incr i} {lappend members n$i}
        assert_equal [r saddex fresh EX 1000 MEMBERS 300 {*}$members] 1
        assert_encoding hashtable fresh
        assert {[r sttl fresh MEMBERS 1 n299] > 900}
        # An existing intset that is too big for a listpack.
        set ints {}
        for {set i 0} {$i < 200} {incr i} {lappend ints $i}
        r sadd ints {*}$ints
        assert_encoding intset ints
        assert_equal [r sexpire ints 1000 MEMBERS 2 5 150] [list $E_OK $E_OK]
        assert_encoding hashtable ints
        assert_equal [lsort -integer [r smembers ints]] $ints
        assert_equal [r sttl ints MEMBERS 2 6 5] [list $E_NO_TTL [r sttl ints MEMBERS 1 5]]
        # A small one becomes a listpack with expirations.
        r sadd smallints 1 2 3
        r sexpire smallints 1000 MEMBERS 1 2
        assert_encoding listpackex smallints
        assert_equal [lsort -integer [r smembers smallints]] {1 2 3}
    }

    test "SMOVE into a full listpack with expirations converts the destination" {
        r flushall
        r config set set-max-listpack-entries 4
        r sadd dst a b c d
        r sexpire dst 1000 MEMBERS 1 a
        assert_encoding listpackex dst
        r sadd src x y
        r sexpire src 1000 MEMBERS 1 x
        assert_equal [r smove src dst x] 1
        assert_encoding hashtable dst
        assert {[r sttl dst MEMBERS 1 a] > 900}
        assert {[r sttl dst MEMBERS 1 x] > 900}
        assert_equal [sme_subexpiry r] 2
        r config set set-max-listpack-entries 128
    }

    test "Changing the limits does not convert the sets that exist" {
        r flushall
        r sadd myset a b c
        r sexpire myset 1000 MEMBERS 1 a
        r config set set-max-listpack-entries 1
        assert_encoding listpackex myset
        r config set set-max-listpack-entries 128
    }

    test "A copy of a hashtable with expirations keeps them, and is independent of the original" {
        r flushall
        r config set set-max-listpack-entries 0
        r sadd myset a b c
        r sexpire myset 1000 MEMBERS 2 a b
        r copy myset copied
        assert_encoding hashtable copied
        assert {[r sttl copied MEMBERS 1 a] > 900}
        assert_equal [r sttl copied MEMBERS 1 c] $E_NO_TTL
        assert_equal [sme_subexpiry r] 2
        r spersist myset MEMBERS 1 a
        assert {[r sttl copied MEMBERS 1 a] > 900}
        r del myset
        assert {[r sttl copied MEMBERS 1 a] > 900}
        assert_equal [sme_subexpiry r] 1
        r config set set-max-listpack-entries 128
    }
}

# Stage 8: the STORE variants. The stored member gets the nearest expiration of
# the sources that have it; a source without an expiration counts as infinite.
start_server {tags {"external:skip needs:debug"}} {
    r debug keysizes-hist-assert 1
    r debug allocsize-slots-assert 1

    foreach enc $::sme_encodings {
    sme_force_encoding r $enc

    test "SUNIONSTORE - The nearest expiration wins, none counts as infinite ($enc)" {
        r flushall
        r sadd a m1 m2 m3 m4
        r sadd b m2 m3 m5
        r sexpire a 1000 MEMBERS 2 m1 m2
        r sexpire a 5000 MEMBERS 1 m3
        r sexpire b 3000 MEMBERS 1 m2
        r sexpire b 100 MEMBERS 1 m3
        assert_equal [r sunionstore d a b] 5
        assert {[r sttl d MEMBERS 1 m1] > 900 && [r sttl d MEMBERS 1 m1] <= 1000}
        assert {[r sttl d MEMBERS 1 m2] > 900 && [r sttl d MEMBERS 1 m2] <= 1000}
        assert {[r sttl d MEMBERS 1 m3] > 50 && [r sttl d MEMBERS 1 m3] <= 100}
        assert_equal [r sttl d MEMBERS 2 m4 m5] [list $E_NO_TTL $E_NO_TTL]
        assert_equal [sme_subexpiry r] 3
    }

    test "SUNIONSTORE - A member with an expiration in one source and none in the other ($enc)" {
        r flushall
        r sadd a x
        r sadd b x
        r sexpire b 1000 MEMBERS 1 x
        r sunionstore d a b
        assert {[r sttl d MEMBERS 1 x] > 900}
        r sunionstore d b a
        assert {[r sttl d MEMBERS 1 x] > 900}
    }

    test "SINTERSTORE - The nearest expiration of all the sources ($enc)" {
        r flushall
        r sadd a m1 m2 m3
        r sadd b m1 m2 m4
        r sadd c m1 m2
        r sexpire a 3000 MEMBERS 1 m1
        r sexpire b 1000 MEMBERS 2 m1 m2
        r sexpire c 2000 MEMBERS 1 m1
        assert_equal [r sinterstore d a b c] 2
        assert {[r sttl d MEMBERS 1 m1] > 900 && [r sttl d MEMBERS 1 m1] <= 1000}
        assert {[r sttl d MEMBERS 1 m2] > 900 && [r sttl d MEMBERS 1 m2] <= 1000}
        r sexpire a 100 MEMBERS 1 m2
        r sinterstore d b a
        assert {[r sttl d MEMBERS 1 m2] <= 100}
        assert_equal [sme_subexpiry r] 4  ;# a, b, c and d
    }

    test "SINTERSTORE - Members that are integers keep their expiration ($enc)" {
        r flushall
        r sadd a 1 2 3
        r sadd b 1 2 4
        r sexpire a 1000 MEMBERS 1 1
        r sinterstore d a b
        assert {[r sttl d MEMBERS 1 1] > 900}
        assert_equal [r sttl d MEMBERS 1 2] $E_NO_TTL
        assert_equal [lsort [r smembers d]] {1 2}
    }

    test "SDIFFSTORE - The members keep the expiration of the first set ($enc)" {
        r flushall
        r sadd a m1 m2 m3
        r sadd b m3 m4
        r sexpire a 1000 MEMBERS 1 m1
        r sexpire b 50 MEMBERS 1 m4
        assert_equal [r sdiffstore d a b] 2
        assert {[r sttl d MEMBERS 1 m1] > 900}
        assert_equal [r sttl d MEMBERS 1 m2] $E_NO_TTL
        assert_equal [sme_subexpiry r] 3  ;# a, b and d
        # many sources and a big first set exercise the other algorithm
        r sadd e m1 m2 m3 m4 m5 m6 m7 m8
        r sexpire e 1000 MEMBERS 1 m5
        r sadd f m1
        r sadd g m2
        r sdiffstore d e f g
        assert {[r sttl d MEMBERS 1 m5] > 900}
        assert_equal [r sttl d MEMBERS 1 m1] $E_NO_MEMBER
    }

    test "STORE variants - Expired members are not stored, and the sources are unchanged ($enc)" {
        r debug set-active-expire 0
        r flushall
        r sadd a m1 m2 m3
        r sadd b m2 m3
        sme_make_expired r a {m2}
        assert_equal [r sunionstore d a b] 3
        assert_equal [r sinterstore d a b] 1
        assert_equal [r sdiffstore d a b] 1
        assert_equal [lsort [r smembers d]] {m1}
        assert_equal [r sttl a MEMBERS 1 m2] $E_NO_MEMBER
        r debug set-active-expire 1
    }

    test "STORE variants - The destination replaces its previous expirations and unregisters ($enc)" {
        r flushall
        r sadd d x y
        r sexpire d 1000 MEMBERS 2 x y
        r sadd a p q
        r sunionstore d a
        assert_equal [r sttl d MEMBERS 1 p] $E_NO_TTL
        assert_equal [sme_subexpiry r] 0
        r sexpire a 1000 MEMBERS 1 p
        r sunionstore d a
        assert_equal [sme_subexpiry r] 2  ;# a and d
        r sinterstore d a nosuchkey
        assert_equal [r exists d] 0
        assert_equal [sme_subexpiry r] 1
    }

    test "STORE variants - The stored members expire actively and propagate ($enc)" {
        r flushall
        r sadd a m1 m2
        r spexpire a 100 MEMBERS 1 m1
        r sunionstore d a
        wait_for_condition 50 100 {[r smembers d] eq {m2}} else {fail "member did not expire"}
        r sdiffstore d a a
        assert_equal [r exists d] 0
    }

    test "STORE variants - Big sets over the listpack limit ($enc)" {
        r flushall
        for {set i 0} {$i < 300} {incr i} {r sadd a m$i; r sadd b m$i}
        r sexpire a 1000 MEMBERS 2 m1 m299
        r sexpire b 500 MEMBERS 1 m299
        r sinterstore d a b
        assert_equal [r scard d] 300
        assert_encoding hashtable d
        assert {[r sttl d MEMBERS 1 m299] <= 500}
        assert {[r sttl d MEMBERS 1 m1] > 900}
        r sunionstore d a b
        assert_equal [r scard d] 300
    }
    }
    r config set set-max-listpack-entries 128

    test "STORE variants replicate the command and the replica gets the same expirations" {
        set repl [attach_to_replication_stream]
        r flushall
        r sadd a m1 m2
        r sexpire a 1000 MEMBERS 1 m1
        r sunionstore d a
        assert_replication_stream $repl {
            {select *}
            {flushall}
            {sadd a m1 m2}
            {spexpireat a * MEMBERS 1 m1}
            {sunionstore d a}
        }
        close_replication_stream $repl
    } {} {needs:repl}
}
