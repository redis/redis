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
set ::E_NO_MEMBER   -2
set ::E_NO_TTL      -1
set ::E_FAIL         0
set ::E_OK           1
set ::E_DELETED      2

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
