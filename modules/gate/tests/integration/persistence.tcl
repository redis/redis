# Gate module: persistence.
#
# RDB (DEBUG RELOAD, SAVE and restart, BGSAVE), AOF (DEBUG LOADAOF, rewrite,
# restart) and DUMP / RESTORE, including damaged payloads.
#
# What SPEC.md promises and these tests check:
#
#   - a gate is one key: whatever Redis does to persist a key keeps the gate
#     exactly as it was (same DEBUG DIGEST-VALUE);
#   - the AOF receives what actually happened, never the command;
#   - after a restart, a reload or a RESTORE the gate keeps its configuration
#     and its model, drops its proof and its waiting requests, and proves
#     itself again by itself. Until then it does not answer, ids issued
#     before are refused (GLEARN replies 0), and snapshot numbers are never
#     reused.
#
# Proof needs many requests at realistic settings, so the gates here use
# ALPHA 0.2 DELTA 0.2 (a threshold is proven after a few dozen right
# answers) and a small snapshot step. Every scenario seeds the module's test
# sequence (gate.test-seed), so runs repeat exactly; the assertions do not
# depend on what is drawn.
#
# Each test builds the gates it needs. The exceptions are the tests around a
# restart, which follow from the test that saved the dataset.

set gatemodule [file normalize modules/gate/gate.so]

# ------------------------------- Helpers ----------------------------------
# All helpers are prefixed gp_ (gate persistence).

# Like "test", but an error that is not an assertion (an error reply nobody
# expected, a server that went away) fails this test instead of aborting
# the whole file.
proc gp_test {name body} {
    uplevel 1 [list test $name [list gp_guard $body]]
}

proc gp_guard {body} {
    if {[catch {uplevel 1 $body} result]} {
        if {[string match "assertion:*" $result]} { error $result }
        set where [string range $::errorInfo 0 400]
        error "assertion:unexpected error: $result\n$where"
    }
    return $result
}

# Start a scenario: seed the module's test sequence and restart the
# numbering of the synthetic requests.
proc gp_begin {seed} {
    r config set gate.test-seed $seed
    set ::gp_n 0
}

# GINFO as a dict, or one field of it.
proc gp_info {key {field {}}} {
    set info [r ginfo $key]
    if {$field eq {}} { return $info }
    dict get $info $field
}

# A classifier gate over two topics. The snapshot step is a server setting
# read when the gate is created. It is put back to its default at once, so
# every test also shows that a gate keeps the value it was created with.
proc gp_classifier {key {snapshot 20} {audit 0.2}} {
    r config set gate.snapshot-examples $snapshot
    assert_equal OK [r gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT $audit CLASSES 2 spam ham]
    r config set gate.snapshot-examples 1000
}

proc gp_score {key {audit 0.2}} {
    assert_equal OK [r gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT $audit SCORE]
}

# Request number $n of the synthetic traffic, as the arguments of GPREDICT,
# and what the teacher says about it, as the arguments of GLEARN.
# Classifier: two topics that are easy to tell apart. Score gate: three
# requests out of four have a high score and a right cheap answer, the
# fourth has a low score and a wrong one, so the proven threshold lies
# between the two scores.
proc gp_request {kind n} {
    if {$kind eq "classifier"} {
        if {$n % 2} {
            return [list [list TEXT "buy cheap pills now offer $n"] {LABEL spam}]
        }
        return [list [list TEXT "meeting notes project schedule $n"] {LABEL ham}]
    }
    if {$n % 4 == 0} { return [list {SCORE 0.3} {CORRECT 0}] }
    return [list {SCORE 0.9} {CORRECT 1}]
}

# Teach a classifier $count labelled examples directly.
proc gp_teach {key count} {
    for {set i 0} {$i < $count} {incr i} {
        lassign [gp_request classifier [incr ::gp_n]] question answer
        r gadd $key {*}$question {*}$answer
    }
}

# One request, and the teacher's answer if the gate asks for it. Returns
# the action.
proc gp_ask {key {kind {}}} {
    if {$kind eq {}} { set kind [gp_info $key kind] }
    lassign [gp_request $kind [incr ::gp_n]] question answer
    lassign [r gpredict $key {*}$question] action id
    if {$action ne "ACCEPT" && $id ne {}} {
        assert_equal 1 [r glearn $key $id {*}$answer] "feedback for a fresh id on $key"
    }
    return $action
}

# 1 if a snapshot answers (or would, if the gate is paused) for every label.
proc gp_proven {key} {
    set info [gp_info $key]
    if {[dict get $info generation] == 0} { return 0 }
    foreach threshold [dict get $info thresholds] {
        if {$threshold eq {}} { return 0 }
    }
    return 1
}

# Ask and answer until the gate has proven itself. Returns the number of
# requests it took.
proc gp_prove {key {max 600}} {
    set kind [gp_info $key kind]
    for {set n 1} {$n <= $max} {incr n} {
        gp_ask $key $kind
        if {[gp_proven $key]} { return $n }
    }
    fail "$key did not prove itself in $max requests: [gp_info $key]"
}

# Ask (and answer) until the gate answers a request by itself. Returns that
# request as a pair: the arguments of GPREDICT, and what the teacher would
# have said as the arguments of GLEARN.
proc gp_until_accept {key {max 200}} {
    set kind [gp_info $key kind]
    for {set i 0} {$i < $max} {incr i} {
        lassign [gp_request $kind [incr ::gp_n]] question answer
        lassign [r gpredict $key {*}$question] action arg
        if {$action eq "ACCEPT"} {
            if {$kind eq "classifier"} {
                assert_equal [lindex $answer 1] $arg "the label $key answers with"
            } else {
                assert_equal {} $arg "a score gate answers without a label"
            }
            return [list $question $answer]
        }
        if {$arg ne {}} { assert_equal 1 [r glearn $key $arg {*}$answer] }
    }
    fail "$key never answered by itself in $max requests: [gp_info $key]"
}

# Send requests until $count of them wait for feedback, and leave those
# unanswered. Returns them as {id answer} pairs.
proc gp_collect {key count {max 400}} {
    set kind [gp_info $key kind]
    set waiting {}
    for {set i 0} {$i < $max && [llength $waiting] < $count} {incr i} {
        lassign [gp_request $kind [incr ::gp_n]] question answer
        lassign [r gpredict $key {*}$question] action id
        if {$action ne "ACCEPT" && $id ne {}} { lappend waiting [list $id $answer] }
    }
    assert_equal $count [llength $waiting] "requests of $key waiting for feedback"
    return $waiting
}

# GLEARN for one {id answer} pair of gp_collect. Returns the reply.
proc gp_answer {key pair} {
    r glearn $key [lindex $pair 0] {*}[lindex $pair 1]
}

# The gate must send this request (a pair returned by gp_until_accept) to
# the teacher. The feedback is given, so that the request does not keep
# younger proof waiting. Returns the id.
proc gp_not_answered {key asked} {
    lassign $asked question answer
    lassign [r gpredict $key {*}$question] action id
    if {$action eq "ACCEPT"} { fail "$key answered by itself: $question -> $action $id" }
    if {$id eq {}} { fail "$key did not ask for feedback: $question -> $action" }
    assert_equal 1 [r glearn $key $id {*}$answer] "feedback for a fresh id on $key"
    assert_equal 0 [r glearn $key $id {*}$answer] "feedback for $key sent twice"
    return $id
}

# The largest snapshot number GINFO shows for the key.
proc gp_max_number {key} {
    set info [gp_info $key]
    expr {max([dict get $info generation], [dict get $info candidate])}
}

# Fail unless two binary strings (DUMP payloads) are the same.
proc gp_assert_same_bytes {a b what} {
    if {$a ne $b} {
        fail "$what: [string length $a] and [string length $b] bytes that differ"
    }
}

proc gp_digests {keys} {
    set digests {}
    foreach key $keys { lappend digests [debug_digest_value $key] }
    return $digests
}

# 1 if two lists of numbers are equal (replies carry doubles as strings in
# RESP2 and as doubles in RESP3).
proc gp_same_numbers {a b} {
    if {[llength $a] != [llength $b]} { return 0 }
    foreach x $a y $b {
        if {$x != $y} { return 0 }
    }
    return 1
}

# What GINFO must show for a gate that came back (from an RDB file, the
# AOF or a DUMP payload) and was not written to since, given what it showed
# before: configuration and model kept, proof and waiting requests gone.
proc gp_assert_came_back {key before} {
    set now [gp_info $key]
    foreach field {kind classes} {
        assert_equal [dict get $before $field] [dict get $now $field] "$field of $key"
    }
    foreach field {alpha delta audit} {
        assert {[dict get $before $field] == [dict get $now $field]}
    }
    assert {[gp_same_numbers [dict get $before alphas] [dict get $now alphas]]}
    assert_equal [dict get $before learned] [dict get $now learned] "learned of $key"
    assert_equal 0 [dict get $now generation] "generation of $key"
    assert_equal 0 [dict get $now pending] "pending of $key"
    assert_equal [llength [dict get $before thresholds]] [llength [dict get $now thresholds]]
    foreach threshold [dict get $now thresholds] {
        assert_equal {} $threshold "a threshold of $key"
    }
    foreach field {audit-samples audit-errors calibration-samples} {
        assert_equal 0 [dict get $now $field] "$field of $key"
    }
    assert {[dict get $now coverage] == 0}
    # A paused gate stays paused; any other is not answering.
    if {[dict get $before state] eq "PAUSED"} {
        assert_equal PAUSED [dict get $now state] "state of $key"
    } else {
        assert {[dict get $now state] in {COLD CALIBRATING}}
    }
    # Counters of what went wrong never go back.
    foreach field {missing revocations} {
        assert {[dict get $now $field] >= [dict get $before $field]}
    }
}

# DEBUG RELOAD, then check every key: same digest, same type, same memory,
# and GINFO shows a gate that came back.
proc gp_reload_check {keys} {
    foreach key $keys {
        set digest($key) [debug_digest_value $key]
        set info($key) [gp_info $key]
        set memory($key) [memory_usage $key]
    }
    assert_equal OK [r debug reload]
    foreach key $keys {
        assert_equal $digest($key) [debug_digest_value $key] "digest of $key after DEBUG RELOAD"
        assert_equal gate-type [r type $key]
        assert_equal $memory($key) [memory_usage $key] "MEMORY USAGE of $key after DEBUG RELOAD"
        gp_assert_came_back $key $info($key)
    }
}

# Names of the commands in an AOF file (not an RDB preamble), with how often
# each occurs.
proc gp_aof_commands {path} {
    set fp [open $path r]
    fconfigure $fp -translation binary
    set names {}
    while {1} {
        set cmd [read_from_aof $fp]
        if {$cmd eq {}} break
        dict incr names [lindex $cmd 0]
    }
    close $fp
    return $names
}

# The AOF must hold what happened, never a gate command.
proc gp_assert_realized {names} {
    assert {[dict exists $names _gate.apply]}
    foreach cmd {gcreate gpredict glearn gadd gpause gresume greset} {
        assert {![dict exists $names $cmd]}
    }
}

# ------------------------ DUMP payloads, by hand ---------------------------
# A DUMP payload of a gate taken with rdbcompression off is:
#
#   1 byte    RDB type (7: a module value)
#   9 bytes   module type id
#   1 byte    module opcode (5: a string follows)
#   length    of the string, in the RDB length encoding
#   string    the serialized gate
#   1 byte    module opcode (0: end of the value)
#   2 bytes   RDB version, little endian
#   8 bytes   CRC64 of everything before it, little endian

# CRC-64/Jones as in src/crc64.c: reflected, initial value 0, no final xor.
proc gp_crc64 {data} {
    if {![info exists ::gp_crc64_table]} {
        set poly [expr {wide(0x95ac9329ac4bc9b5)}]
        for {set i 0} {$i < 256} {incr i} {
            set c $i
            for {set k 0} {$k < 8} {incr k} {
                if {$c & 1} {
                    set c [expr {(($c >> 1) & 0x7fffffffffffffff) ^ $poly}]
                } else {
                    set c [expr {($c >> 1) & 0x7fffffffffffffff}]
                }
            }
            lappend ::gp_crc64_table $c
        }
    }
    set table $::gp_crc64_table
    set crc 0
    binary scan $data cu* bytes
    foreach byte $bytes {
        set crc [expr {[lindex $table [expr {($crc ^ $byte) & 0xff}]] ^ (($crc >> 8) & 0x00ffffffffffffff)}]
    }
    return $crc
}

proc gp_rdb_length {n} {
    if {$n < 64} { return [binary format c $n] }
    if {$n < 16384} { return [binary format S [expr {0x4000 | $n}]] }
    return [binary format cI 0x80 $n]
}

# Split a payload into what comes before the length of the serialized gate,
# the serialized gate, and the RDB version.
proc gp_dump_split {payload} {
    binary scan $payload cu type
    binary scan $payload @10cu opcode
    binary scan $payload @11cu first
    if {$type != 7 || $opcode != 5} {
        fail "not the DUMP layout of a module value: type $type, opcode $opcode"
    }
    if {($first & 0xc0) == 0} {
        set length [expr {$first & 0x3f}]
        set start 12
    } elseif {($first & 0xc0) == 0x40} {
        binary scan $payload @12cu second
        set length [expr {(($first & 0x3f) << 8) | $second}]
        set start 13
    } elseif {$first == 0x80} {
        binary scan $payload @12Iu length
        set start 16
    } else {
        fail "the serialized gate is not a plain string (is rdbcompression on?)"
    }
    if {$start + $length + 11 != [string length $payload]} {
        fail "unexpected DUMP layout: a string of $length bytes at $start in [string length $payload] bytes"
    }
    list [string range $payload 0 10] \
         [string range $payload $start [expr {$start + $length - 1}]] \
         [string range $payload end-9 end-8]
}

# The payload for a (damaged) serialized gate, with a valid checksum.
proc gp_dump_join {head blob version} {
    set payload $head
    append payload [gp_rdb_length [string length $blob]] $blob "\x00" $version
    gp_dump_seal $payload
}

# Append the checksum to everything that comes before it.
proc gp_dump_seal {body} {
    append body [binary format w [gp_crc64 $body]]
    return $body
}

# RESTORE a damaged payload into gp:damaged. It must be refused with an
# error that leaves no key behind. If the damage happens to leave a
# well-formed gate, the gate must be a gate: one that came back without
# proof, can be used, does not answer by itself, and can be dumped and
# restored again. Returns "refused" or "accepted".
proc gp_restore_damaged {payload what {canonical 1}} {
    r del gp:damaged gp:again
    if {[catch {r restore gp:damaged 0 $payload} reply]} {
        if {![string match "*Bad data format*" $reply]} {
            fail "$what: RESTORE replied '$reply'"
        }
        if {[r exists gp:damaged]} { fail "$what: refused, but the key exists" }
        return refused
    }
    if {$reply ne "OK"} { fail "$what: RESTORE replied '$reply'" }
    if {[r type gp:damaged] ne "gate-type"} { fail "$what: restored something that is not a gate" }
    # What was accepted is what is stored.
    if {$canonical} {
        gp_assert_same_bytes $payload [r dump gp:damaged] "$what: the payload that was restored and DUMP of the gate"
    }
    set info [gp_info gp:damaged]
    set kind [dict get $info kind]
    if {$kind ni {classifier score}} { fail "$what: kind '$kind'" }
    # It is a gate GCREATE could have made ...
    foreach {field low high} {alpha 0 1 delta 0 1 audit 0 1.0000001} {
        set value [dict get $info $field]
        if {!($value > $low && $value < $high)} { fail "$what: $field is $value" }
    }
    set classes [llength [dict get $info classes]]
    if {$kind eq "score" ? $classes != 0 : ($classes < 2 || $classes > 64)} {
        fail "$what: a $kind gate with $classes classes"
    }
    # ... that came back without proof ...
    if {[dict get $info generation] != 0 || [dict get $info pending] != 0 ||
        [dict get $info state] eq "ACTIVE"} {
        fail "$what: restored with proof: $info"
    }
    foreach threshold [dict get $info thresholds] {
        if {$threshold ne {}} { fail "$what: restored with a threshold: $info" }
    }
    # ... and works: it takes requests, feedback and examples, and does not
    # answer by itself.
    for {set i 0} {$i < 3} {incr i} {
        lassign [gp_request $kind $i] question answer
        lassign [r gpredict gp:damaged {*}$question] action id
        if {$action eq "ACCEPT"} { fail "$what: answered by itself right after RESTORE" }
        if {$id ne {} && [r glearn gp:damaged $id {*}$answer] != 1} {
            fail "$what: fresh feedback was not used"
        }
    }
    if {$kind eq "classifier"} {
        r gadd gp:damaged TEXT "an example" LABEL [lindex [dict get $info classes] 0]
    }
    r gpause gp:damaged
    r gresume gp:damaged
    gp_info gp:damaged
    # Whatever it is now, Redis must be able to save it and load it again.
    if {[catch {r restore gp:again 0 [r dump gp:damaged]} reply]} {
        fail "$what: after RESTORE and a few commands the gate cannot be restored from its own DUMP: $reply"
    }
    r del gp:damaged gp:again
    return accepted
}

# Six small gates in different states, each with three requests waiting.
# Returns a dict: key -> the {id answer} pairs that wait.
proc gp_zoo {p} {
    foreach key [list $p:cold $p:cal $p:reset] {
        gp_classifier $key
        gp_teach $key 15
    }
    gp_teach $p:cal 10
    foreach key [list $p:scal $p:sact $p:spaused] { gp_score $key }
    gp_prove $p:sact
    r gpause $p:spaused
    set waiting {}
    foreach key [list $p:cold $p:cal $p:reset $p:scal $p:sact $p:spaused] {
        dict set waiting $key [gp_collect $key 3]
    }
    return $waiting
}

# A classifier whose answering snapshot took over from an older one. The
# first snapshot is taken from a model that knows little more than one topic,
# so it proves itself for that topic alone. The next one knows both, answers
# more, and takes over. Whether and how soon depends on what the first model
# makes of the other topic, so a few starting points are tried. Returns the
# key.
proc gp_takeover {p seed} {
    foreach ham {1 2 4} {
        gp_begin [expr {$seed + $ham}]
        set key $p$ham
        gp_classifier $key 20 0.5
        for {set i 0} {$i < 20} {incr i} {
            if {$i < $ham} {
                r gadd $key TEXT "meeting notes project schedule [expr {1000 + $i}]" LABEL ham
            } else {
                r gadd $key TEXT "buy cheap pills now offer [expr {1000 + $i}]" LABEL spam
            }
        }
        set first 0
        for {set n 0} {$n < 600} {incr n} {
            gp_ask $key classifier
            set info [gp_info $key]
            set generation [dict get $info generation]
            if {$first == 0} { set first $generation }
            if {$generation > $first || [dict get $info revocations] > 0} break
        }
        if {$first > 0 && $generation > $first && [dict get $info revocations] == 0} {
            return $key
        }
    }
    fail "no snapshot took over from an older one: the scenario of gp_takeover needs new numbers"
}

# ============================ RDB: DEBUG RELOAD =============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    gp_test {DEBUG DIGEST-VALUE follows what a gate holds, and nothing else} {
        gp_begin 100
        gp_classifier t:c
        set digest [debug_digest_value t:c]
        if {$digest ne "dummy-digest-value"} {
            # Whatever changes the gate changes the digest.
            set seen [list $digest]
            set changed {
                set digest [debug_digest_value t:c]
                assert {$digest ni $seen}
                lappend seen $digest
            }
            assert_equal 1 [r gadd t:c TEXT "an example" LABEL spam]
            eval $changed
            lassign [r gpredict t:c TEXT "a request"] action id
            eval $changed
            assert_equal 1 [r glearn t:c $id LABEL ham]
            eval $changed
            assert_equal 0 [r glearn t:c $id LABEL ham]
            assert_equal OK [r gpause t:c]
            eval $changed
            assert_equal OK [r gresume t:c]
            assert {[debug_digest_value t:c] ne $digest}
            lappend seen [debug_digest_value t:c]
            assert_equal OK [r greset t:c]
            eval $changed
            # Reading it does not.
            r ginfo t:c
            r dump t:c
            r memory usage t:c
            assert_equal $digest [debug_digest_value t:c]
            # A request the gate answers by itself changes nothing that is
            # saved.
            assert_equal OK [r gcreate t:s ALPHA 0.2 DELTA 0.2 AUDIT 0.000000001 SCORE]
            gp_prove t:s
            set digest [debug_digest_value t:s]
            set accepted [gp_info t:s accepted]
            for {set i 0} {$i < 20} {incr i} {
                assert_equal {ACCEPT {}} [r gpredict t:s SCORE 0.9]
            }
            assert_equal [expr {$accepted + 20}] [gp_info t:s accepted]
            assert_equal $digest [debug_digest_value t:s]
        }
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, COLD, training requests waiting} {
        gp_begin 101
        gp_classifier g:cold
        gp_teach g:cold 8
        set waiting [gp_collect g:cold 4]
        assert_equal 1 [gp_answer g:cold [lindex $waiting 1]]
        assert_equal COLD [gp_info g:cold state]
        assert_equal 9 [gp_info g:cold learned]
        assert_equal 3 [gp_info g:cold pending]
        gp_reload_check g:cold
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, CALIBRATING, answered and unanswered requests waiting} {
        gp_begin 102
        gp_classifier g:cal
        gp_teach g:cal 20
        assert_equal CALIBRATING [gp_info g:cal state]
        set waiting [gp_collect g:cal 6]
        # Proof is applied in request order: these two answers have to wait
        # for the older requests.
        assert_equal 1 [gp_answer g:cal [lindex $waiting 2]]
        assert_equal 1 [gp_answer g:cal [lindex $waiting 4]]
        assert_equal CALIBRATING [gp_info g:cal state]
        assert_equal 22 [gp_info g:cal learned]
        assert_equal 0 [gp_info g:cal calibration-samples]
        assert {[gp_info g:cal pending] >= 4}
        gp_reload_check g:cal
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, ACTIVE} {
        gp_begin 103
        gp_classifier g:act 100
        gp_teach g:act 100
        gp_prove g:act
        assert_equal ACTIVE [gp_info g:act state]
        assert_equal 0 [gp_info g:act candidate]
        gp_until_accept g:act
        # Spot checks waiting for the teacher, one of them answered out of
        # order.
        set waiting [gp_collect g:act 3]
        assert_equal 1 [gp_answer g:act [lindex $waiting 1]]
        assert {[gp_info g:act accepted] > 0}
        gp_reload_check g:act
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, ACTIVE with a background candidate} {
        gp_begin 104
        gp_classifier g:cand 100
        gp_teach g:cand 100
        gp_prove g:cand
        # Enough new examples for the next snapshot, which is tested behind
        # the one that answers.
        gp_teach g:cand 100
        assert_equal ACTIVE [gp_info g:cand state]
        assert {[gp_info g:cand candidate] > [gp_info g:cand generation]}
        # Only spot checks are proof for it now: give it some.
        for {set i 0} {$i < 300 && [gp_info g:cand calibration-samples] == 0} {incr i} {
            gp_ask g:cand
        }
        assert {[gp_info g:cand calibration-samples] > 0}
        gp_collect g:cand 2
        assert_equal ACTIVE [gp_info g:cand state]
        assert {[gp_info g:cand candidate] > [gp_info g:cand generation]}
        gp_reload_check g:cand
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, ACTIVE with a snapshot that took over from an older one} {
        set key [gp_takeover g:took 130]
        assert_equal ACTIVE [gp_info $key state]
        assert {[gp_info $key generation] >= 2}
        gp_collect $key 3
        gp_reload_check $key
    }

    gp_test {DEBUG RELOAD keeps the digest: classifier, PAUSED} {
        gp_begin 105
        # Paused while answering ...
        gp_classifier g:paused 100
        gp_teach g:paused 100
        gp_prove g:paused
        gp_until_accept g:paused
        assert_equal OK [r gpause g:paused]
        gp_collect g:paused 3
        assert_equal PAUSED [gp_info g:paused state]
        assert {[gp_info g:paused generation] > 0}
        # ... and paused while still being tested.
        gp_classifier g:paused2
        gp_teach g:paused2 20
        assert_equal OK [r gpause g:paused2]
        set waiting [gp_collect g:paused2 4]
        assert_equal 1 [gp_answer g:paused2 [lindex $waiting 1]]
        assert_equal PAUSED [gp_info g:paused2 state]
        gp_reload_check {g:paused g:paused2}
    }

    gp_test {DEBUG RELOAD keeps the digest: score gate, CALIBRATING, answered and unanswered requests waiting} {
        gp_begin 106
        gp_score s:cal
        assert_equal CALIBRATING [gp_info s:cal state]
        set waiting [gp_collect s:cal 8]
        assert_equal 1 [gp_answer s:cal [lindex $waiting 2]]
        assert_equal 1 [gp_answer s:cal [lindex $waiting 5]]
        assert_equal CALIBRATING [gp_info s:cal state]
        assert {[gp_info s:cal pending] >= 6}
        # A score gate that was just created, too.
        gp_score s:new
        gp_reload_check {s:cal s:new}
    }

    gp_test {DEBUG RELOAD keeps the digest: score gate, ACTIVE} {
        gp_begin 107
        gp_score s:act
        gp_prove s:act
        assert_equal ACTIVE [gp_info s:act state]
        # The cheap answer is wrong for the low scores: the gate answers
        # for the high ones only.
        set threshold [lindex [gp_info s:act thresholds] 0]
        assert {$threshold > 0.3 && $threshold <= 0.9}
        gp_until_accept s:act
        set waiting [gp_collect s:act 3]
        assert_equal 1 [gp_answer s:act [lindex $waiting 1]]
        assert {[gp_info s:act accepted] > 0}
        gp_reload_check s:act
    }

    gp_test {DEBUG RELOAD keeps the digest: score gate, PAUSED} {
        gp_begin 108
        gp_score s:paused
        gp_prove s:paused
        gp_until_accept s:paused
        assert_equal OK [r gpause s:paused]
        gp_collect s:paused 2
        assert_equal PAUSED [gp_info s:paused state]
        gp_score s:paused2
        assert_equal OK [r gpause s:paused2]
        gp_collect s:paused2 3
        assert_equal PAUSED [gp_info s:paused2 state]
        gp_reload_check {s:paused s:paused2}
    }

    gp_test {DEBUG RELOAD keeps the digest: after GRESET, after a revocation, with requests that timed out} {
        gp_begin 109
        # GRESET in the middle of a life.
        gp_classifier g:reset
        gp_teach g:reset 20
        gp_collect g:reset 3
        assert_equal OK [r greset g:reset]
        assert_equal 0 [gp_info g:reset learned]
        assert_equal 0 [gp_info g:reset pending]
        gp_teach g:reset 5
        gp_collect g:reset 2
        # A gate that stopped answering by itself. With AUDIT 1 every
        # request it could answer is a spot check.
        gp_score s:revoked 1
        gp_prove s:revoked
        for {set i 0} {$i < 200 && [gp_info s:revoked revocations] == 0} {incr i} {
            lassign [r gpredict s:revoked SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [r glearn s:revoked $id CORRECT 0]
        }
        assert_equal 1 [gp_info s:revoked revocations]
        assert_equal 0 [gp_info s:revoked generation]
        gp_collect s:revoked 2
        # The same for a classifier, which also forgets its model.
        gp_classifier g:revoked 100 1
        gp_teach g:revoked 100
        gp_prove g:revoked
        for {set i 0} {$i < 200 && [gp_info g:revoked revocations] == 0} {incr i} {
            lassign [gp_request classifier [incr ::gp_n]] question answer
            lassign [r gpredict g:revoked {*}$question] action id
            assert_equal AUDIT $action
            set wrong [expr {[lindex $answer 1] eq "spam" ? "ham" : "spam"}]
            assert_equal 1 [r glearn g:revoked $id LABEL $wrong]
        }
        assert_equal 1 [gp_info g:revoked revocations]
        assert_equal 0 [gp_info g:revoked generation]
        gp_teach g:revoked 3
        gp_collect g:revoked 2
        # Requests whose feedback never came.
        r config set gate.feedback-timeout 40
        gp_score s:late
        r config set gate.feedback-timeout 300000
        gp_collect s:late 3
        after 80
        gp_collect s:late 2
        assert {[gp_info s:late missing] >= 3}
        gp_reload_check {g:reset s:revoked g:revoked s:late}
    }

    gp_test {DEBUG RELOAD keeps the digest: requests older than the feedback window that nothing has noticed yet} {
        gp_begin 110
        r config set gate.feedback-timeout 40
        gp_score s:stale
        gp_classifier g:stale
        r config set gate.feedback-timeout 300000
        gp_teach g:stale 20
        set old [gp_collect s:stale 3]
        set oldc [gp_collect g:stale 3]
        after 80
        gp_reload_check {s:stale g:stale}
        # Too old, and from before the reload.
        assert_equal 0 [gp_answer s:stale [lindex $old 0]]
        assert_equal 0 [gp_answer g:stale [lindex $oldc 0]]
        assert_equal 0 [gp_info s:stale pending]
        assert_equal 0 [gp_info g:stale pending]
    }

    gp_test {DEBUG RELOAD keeps the digest: every GCREATE option, odd labels, 3 and 64 classes} {
        gp_begin 111
        # Options in an unusual order; a label with a space, one in UTF-8
        # and one that spells an option word.
        set utf8 [encoding convertto utf-8 "h\u00e9h\u00e9 \u2713"]
        r config set gate.snapshot-examples 6
        assert_equal OK [r gcreate o:odd CLASSALPHA 2 "oui merci" 0.1 SCORE 0.25 AUDIT 1 \
            CLASSES 4 "oui merci" non SCORE $utf8 ALWAYSDEFER 1 non ALPHA 0.3 DELTA 0.5]
        assert_equal [list "oui merci" non SCORE $utf8] [gp_info o:odd classes]
        assert {[gp_same_numbers {0.1 0 0.25 0.3} [gp_info o:odd alphas]]}
        set i 0
        foreach label [list "oui merci" non SCORE $utf8 "oui merci" non SCORE $utf8] {
            r gadd o:odd TEXT "topic[incr i] of $label" FEATURES 2 depth 0.5 mentioned 1 LABEL $label
        }
        # Waiting requests with named features and a text that is not text.
        r gpredict o:odd TEXT "bin\x00 ary \xff\xfe\r\n text" FEATURES 1 depth -0.25
        r gpredict o:odd TEXT "" FEATURES 0
        r gpredict o:odd TEXT [string repeat "long text " 1000]
        # As many named features as a request can have.
        set features {}
        for {set i 0} {$i < 64} {incr i} { lappend features "feature-$i" [expr {($i - 32) / 16.0}] }
        r gpredict o:odd TEXT "sixty-four features" FEATURES 64 {*}$features
        # Three classes, extreme settings.
        r config set gate.snapshot-examples 1
        assert_equal OK [r gcreate o:three ALPHA 0.999 DELTA 0.000001 AUDIT 0.000001 CLASSES 3 a b c]
        foreach label {a b c a b c} { r gadd o:three TEXT "word-$label" LABEL $label }
        r gpredict o:three TEXT "word-a"
        # The largest number of classes, with the longest labels.
        set labels {}
        for {set i 0} {$i < 64} {incr i} { lappend labels [format "class-%058d" $i] }
        assert_equal 64 [string length [lindex $labels 0]]
        r config set gate.snapshot-examples 5
        assert_equal OK [r gcreate o:64 ALPHA 0.05 CLASSES 64 {*}$labels]
        for {set i 0} {$i < 8} {incr i} { r gadd o:64 TEXT "word$i" LABEL [lindex $labels [expr {$i * 9}]] }
        for {set i 0} {$i < 3} {incr i} { r gpredict o:64 TEXT "word$i" }
        r config set gate.snapshot-examples 1000
        assert_equal 64 [llength [gp_info o:64 classes]]
        assert {[gp_info o:64 candidate] > 0}
        gp_reload_check {o:odd o:three o:64}
        assert_equal [list "oui merci" non SCORE $utf8] [gp_info o:odd classes]
        assert_equal $labels [gp_info o:64 classes]
    }

    gp_test {DEBUG RELOAD keeps the digest: a gate holding 4,096 waiting requests} {
        gp_begin 112
        gp_score s:full
        set rd [redis_deferring_client]
        for {set i 0} {$i < 4096} {incr i} { $rd gpredict s:full SCORE 0.5 }
        set ids {}
        for {set i 0} {$i < 4096} {incr i} {
            lassign [$rd read] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        $rd close
        assert_equal 4096 [llength [lsort -unique $ids]]
        assert_equal 4096 [gp_info s:full pending]
        # Full: it abstains and wants no feedback.
        assert_equal {ABSTAIN {}} [r gpredict s:full SCORE 0.5]
        gp_reload_check s:full
        # Nothing waits any more, so it takes requests again.
        lassign [r gpredict s:full SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        assert_equal 0 [r glearn s:full [lindex $ids 0] CORRECT 1]
        assert_equal 0 [r glearn s:full [lindex $ids end] CORRECT 1]
        assert_equal 1 [r glearn s:full $id CORRECT 1]
        r del s:full
    }

    gp_test {DEBUG RELOAD keeps the digest: a classifier holding hundreds of long waiting requests} {
        gp_begin 114
        gp_classifier g:long
        gp_teach g:long 20
        set rd [redis_deferring_client]
        fconfigure [$rd channel] -buffersize 1000000
        for {set i 0} {$i < 300} {incr i} {
            # About 4,000 bytes of words the gate has never seen.
            set words {}
            for {set j 0} {$j < 560} {incr j} {
                lappend words [format "w%x" [expr {($i * 7919 + $j * 104729) & 0xffffff}]]
            }
            $rd gpredict g:long TEXT [join $words " "] FEATURES 2 depth 0.5 urgent 1
        }
        for {set i 0} {$i < 300} {incr i} {
            lassign [$rd read] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
        }
        $rd close
        assert_equal 300 [gp_info g:long pending]
        # The gate keeps what it needs to learn from them.
        assert {[gp_info g:long memory] > 1000000}
        gp_reload_check g:long
        # The same through DUMP and RESTORE.
        assert_equal OK [r restore g:long:copy 0 [r dump g:long]]
        assert_equal [debug_digest_value g:long] [debug_digest_value g:long:copy]
        assert_equal [gp_info g:long memory] [gp_info g:long:copy memory]
        r del g:long g:long:copy
    }

    gp_test {DEBUG RELOAD keeps the digest: many gates at once, with and without RDB compression} {
        gp_begin 113
        set keys [r keys *]
        for {set i 0} {$i < 8} {incr i} {
            # Classifiers at different points of their lives ...
            gp_classifier many:c$i [expr {10 + $i}]
            gp_teach many:c$i [expr {4 * $i}]
            gp_collect many:c$i [expr {$i % 4}]
            # ... and score gates: some answer, some are paused.
            gp_score many:s$i [expr {0.1 * ($i + 1)}]
            for {set j 0} {$j < 12 * $i} {incr j} { gp_ask many:s$i score }
            gp_collect many:s$i [expr {$i % 3}]
            if {$i % 4 == 3} { r gpause many:s$i }
            lappend keys many:c$i many:s$i
        }
        assert {[llength $keys] >= 16}
        # A key with a time to live, other types of keys, another database.
        r pexpire many:s0 1000000
        r set many:string "not a gate"
        r rpush many:list a b c
        if {!$::singledb} {
            r select 0
            gp_score other:db
            gp_collect other:db 2
            set other [debug_digest_value other:db]
            r select 9
        }
        set states {}
        foreach key $keys { dict incr states [gp_info $key state] }
        foreach state {COLD CALIBRATING ACTIVE PAUSED} { assert {[dict exists $states $state]} }
        set all [debug_digest]
        set size [r dbsize]
        foreach compression {no yes} {
            r config set rdbcompression $compression
            gp_reload_check $keys
            assert_equal $all [debug_digest] "DEBUG DIGEST with rdbcompression $compression"
            assert_equal $size [r dbsize]
        }
        assert_range [r pttl many:s0] 1 1000000
        assert_equal "not a gate" [r get many:string]
        if {!$::singledb} {
            r select 0
            assert_equal $other [debug_digest_value other:db]
            r del other:db
            r select 9
        }
    }

    # ------------------------------ BGSAVE ---------------------------------

    gp_test {BGSAVE with waiting requests, then DEBUG RELOAD: same digests} {
        gp_begin 120
        set zoo [gp_zoo bg]
        set keys [dict keys $zoo]
        foreach key $keys { assert {[gp_info $key pending] >= 3} }
        set before [gp_digests $keys]
        r bgsave
        waitForBgsave r
        assert_equal ok [s rdb_last_bgsave_status]
        assert_equal $before [gp_digests $keys]
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests $keys]
        foreach key $keys {
            assert_equal 0 [gp_info $key pending]
            assert_equal 0 [gp_answer $key [lindex [dict get $zoo $key] 0]]
        }
    }

    gp_test {BGSAVE writes the gates as they were when it started, whatever happens meanwhile} {
        gp_begin 121
        r flushall
        set zoo [gp_zoo bh]
        set keys [dict keys $zoo]
        set before [gp_digests $keys]
        # The child takes a while for each key ...
        r config set rdb-key-save-delay 100000
        r bgsave
        wait_for_condition 50 10 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "the background save did not start"
        }
        r config set rdb-key-save-delay 0
        # ... while the gates move on.
        foreach key $keys {
            assert_equal 1 [gp_answer $key [lindex [dict get $zoo $key] 0]]
            gp_collect $key 2
        }
        assert_equal OK [r greset bh:reset]
        assert_equal 1 [r del bh:scal]
        gp_score bh:born
        assert_equal 1 [s rdb_bgsave_in_progress] "the save was over before the gates changed: machine too slow for this test"
        waitForBgsave r
        assert_equal ok [s rdb_last_bgsave_status]
        # Load what the child wrote.
        assert_equal OK [r debug reload nosave]
        assert_equal $before [gp_digests $keys]
        assert_equal 0 [r exists bh:born]
        # The requests that were waiting when it started are in the file,
        # and dropped like any others.
        foreach key $keys {
            assert_equal 0 [gp_info $key pending]
            assert_equal 0 [gp_answer $key [lindex [dict get $zoo $key] 1]]
        }
    }
}

# ===================== RDB: what a gate is after a reload ===================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    foreach {kind seed} {classifier 201 score 202} {
        gp_test "After DEBUG RELOAD a $kind gate does not answer until it has proven itself again" {
            gp_begin $seed
            set key b:$kind
            if {$kind eq "classifier"} {
                gp_classifier $key 100
                gp_teach $key 100
            } else {
                gp_score $key
            }
            gp_prove $key
            assert_equal ACTIVE [gp_info $key state]
            set asked [gp_until_accept $key]
            # Spot checks whose feedback is still on its way.
            set old [gp_collect $key 3]
            set before [gp_info $key]
            set used [gp_max_number $key]
            set digest [debug_digest_value $key]

            assert_equal OK [r debug reload]
            assert_equal $digest [debug_digest_value $key]
            # GINFO: the model is there, the proof and the requests are not.
            gp_assert_came_back $key $before
            assert_equal [dict get $before learned] [gp_info $key learned]
            assert_equal 0 [gp_info $key generation]
            assert_equal 0 [gp_info $key pending]
            # The request it answered before the reload goes to the teacher.
            set id [gp_not_answered $key $asked]
            # Ids are not handed out twice.
            foreach pair $old {
                assert {[lindex $pair 0] ne $id}
                assert {[lindex [split [lindex $pair 0] -] 0] ne [lindex [split $id -] 0]}
            }
            # Feedback for what was asked before the reload is not used,
            # however often it is sent.
            foreach pair $old {
                assert_equal 0 [gp_answer $key $pair]
                assert_equal 0 [gp_answer $key $pair]
            }
            # It proves itself again, with a snapshot number that was never
            # used, and answers again.
            gp_prove $key
            assert_equal ACTIVE [gp_info $key state]
            assert {[gp_info $key generation] > $used}
            gp_until_accept $key
            if {$kind eq "classifier"} {
                assert {[gp_info $key learned] > [dict get $before learned]}
            }
        }
    }

    gp_test {After DEBUG RELOAD the first command may be GLEARN with an id from before: reply 0} {
        gp_begin 203
        gp_classifier b:first
        gp_teach b:first 20
        set old [gp_collect b:first 4]
        gp_score b:first:s
        set olds [gp_collect b:first:s 4]
        assert_equal OK [r debug reload]
        foreach pair $old { assert_equal 0 [gp_answer b:first $pair] }
        foreach pair $olds { assert_equal 0 [gp_answer b:first:s $pair] }
        assert_equal 0 [gp_info b:first pending]
        assert_equal 0 [gp_info b:first:s pending]
        # The examples behind those requests were not learned.
        assert_equal 20 [gp_info b:first learned]
        # The gates go on from there.
        assert_equal ABSTAIN [gp_ask b:first]
        assert_equal ABSTAIN [gp_ask b:first:s]
        assert_equal 21 [gp_info b:first learned]
    }

    gp_test {After DEBUG RELOAD the gate does not answer whichever command comes first: GADD, GPAUSE and GRESUME, GRESET} {
        gp_begin 210
        foreach key {b:gadd b:pause b:reset} {
            gp_classifier $key 100
            gp_teach $key 100
            gp_prove $key
            set first_asked($key) [gp_until_accept $key]
            set first_old($key) [gp_collect $key 2]
            set first_used($key) [gp_max_number $key]
        }
        set learned [gp_info b:gadd learned]
        assert_equal OK [r debug reload]

        # An example taught directly counts; nothing else changed for the
        # client.
        assert_equal [expr {$learned + 1}] [r gadd b:gadd TEXT "buy cheap pills" LABEL spam]
        assert {[gp_info b:gadd state] ne "ACTIVE"}

        # Pausing and resuming does not bring the proof back.
        assert_equal OK [r gpause b:pause]
        assert_equal PAUSED [gp_info b:pause state]
        assert_equal OK [r gresume b:pause]
        assert {[gp_info b:pause state] ne "ACTIVE"}

        # A reset forgets the model as well.
        assert_equal OK [r greset b:reset]
        assert_equal 0 [gp_info b:reset learned]
        assert_equal COLD [gp_info b:reset state]
        gp_teach b:reset 100

        foreach key {b:gadd b:pause b:reset} {
            assert_equal 0 [gp_info $key generation]
            foreach pair $first_old($key) { assert_equal 0 [gp_answer $key $pair] }
            gp_not_answered $key $first_asked($key)
            gp_prove $key
            assert_equal ACTIVE [gp_info $key state]
            assert {[gp_info $key generation] > $first_used($key)}
        }
    }

    gp_test {After DEBUG RELOAD reading a gate, or a command that fails, changes nothing} {
        gp_begin 204
        gp_classifier b:idle 100
        gp_teach b:idle 100
        gp_prove b:idle
        set old [gp_collect b:idle 2]
        set id [lindex $old 0 0]
        assert_equal OK [r debug reload]
        set digest [debug_digest_value b:idle]
        set info [gp_info b:idle]
        # Reads.
        assert_equal $info [gp_info b:idle]
        assert_equal gate-type [r type b:idle]
        assert_equal 1 [r exists b:idle]
        r memory usage b:idle
        r dump b:idle
        assert_match "*serializedlength*" [r debug object b:idle]
        # Commands that are refused: arguments are checked before anything
        # changes.
        assert_error "*wrong number of arguments*" {r gpredict b:idle TEXT}
        assert_error "ERR*" {r gpredict b:idle SCORE 0.5}
        assert_error "ERR*" {r gpredict b:idle TEXT hello FEATURES 1 depth}
        assert_error "ERR*" {r gpredict b:idle TEXT hello FEATURES 1 depth nan}
        assert_error "ERR*" {r gpredict b:idle TEXT hello EXTRA}
        assert_error "ERR*" {r glearn b:idle $id LABEL eggs}
        assert_error "ERR*" {r glearn b:idle $id CORRECT 1}
        assert_error "ERR*" {r gadd b:idle TEXT hello LABEL eggs}
        assert_error "ERR*" {r gadd b:idle TEXT hello FEATURES 1 depth LABEL spam}
        assert_error "BUSYKEY*" {r gcreate b:idle ALPHA 0.2 SCORE}
        assert_error "ERR*" {r copy b:idle b:idle:copy}
        assert_equal 0 [r exists b:idle:copy]
        assert_equal $digest [debug_digest_value b:idle]
        assert_equal $info [gp_info b:idle]
    }

    gp_test {After DEBUG RELOAD GINFO already shows the gate as its first write will leave it} {
        gp_begin 205
        # One gate in every state.
        gp_classifier v:cold
        gp_teach v:cold 5
        gp_classifier v:cal
        gp_teach v:cal 20
        gp_classifier v:act 100
        gp_teach v:act 100
        gp_prove v:act
        gp_classifier v:cand 100
        gp_teach v:cand 100
        gp_prove v:cand
        gp_teach v:cand 100
        gp_classifier v:paused 100
        gp_teach v:paused 100
        gp_prove v:paused
        r gpause v:paused
        gp_score v:scal
        gp_score v:sact
        gp_prove v:sact
        gp_score v:spaused
        r gpause v:spaused
        set keys {v:cold v:cal v:act v:cand v:paused v:scal v:sact v:spaused}
        set v_old {}
        foreach key $keys { dict set v_old $key [gp_collect $key 2] }
        assert_equal OK [r debug reload]
        foreach key $keys {
            set shown [gp_info $key]
            # A write that changes nothing GINFO shows: feedback for a
            # request from before the reload.
            assert_equal 0 [gp_answer $key [lindex [dict get $v_old $key] 0]]
            set after [gp_info $key]
            # "memory" is what the key holds now, proof included.
            foreach info {shown after} { dict unset $info memory }
            assert_equal $shown $after "GINFO of $key before and after its first write"
            # A snapshot is being tested, or none is.
            if {[dict get $after state] eq "CALIBRATING"} {
                assert {[dict get $after candidate] > 0}
            }
            if {[dict get $after state] eq "COLD"} {
                assert_equal 0 [dict get $after candidate]
            }
        }
        # A score gate has nothing to learn first: it is tested again at
        # once.
        foreach key {v:scal v:sact} {
            assert_equal CALIBRATING [gp_info $key state] "state of $key"
        }
        assert_equal COLD [gp_info v:cold state]
        foreach key {v:paused v:spaused} { assert_equal PAUSED [gp_info $key state] }
    }

    gp_test {After DEBUG RELOAD a paused gate stays paused and proves itself again in shadow mode} {
        gp_begin 206
        gp_score b:shadow
        gp_prove b:shadow
        gp_until_accept b:shadow
        set used [gp_max_number b:shadow]
        assert_equal OK [r gpause b:shadow]
        assert_equal OK [r debug reload]
        assert_equal PAUSED [gp_info b:shadow state]
        assert_equal 0 [gp_info b:shadow generation]
        set accepted [gp_info b:shadow accepted]
        # It keeps proving itself, and answers nothing.
        for {set n 0} {![gp_proven b:shadow]} {incr n} {
            assert {$n < 600}
            assert {[gp_ask b:shadow score] ne "ACCEPT"}
        }
        assert_equal PAUSED [gp_info b:shadow state]
        assert {[gp_info b:shadow generation] > $used}
        for {set n 0} {$n < 40} {incr n} {
            assert {[gp_ask b:shadow score] ne "ACCEPT"}
        }
        assert_equal $accepted [gp_info b:shadow accepted]
        assert_equal OK [r gresume b:shadow]
        assert_equal ACTIVE [gp_info b:shadow state]
        gp_until_accept b:shadow
    }

    gp_test {Snapshot numbers are never reused: across reloads, a revocation and GRESET} {
        gp_begin 207
        # With AUDIT 1 the gate never answers and every request it could
        # answer is a spot check.
        gp_score b:numbers 1
        set used 0
        # After something that drops the snapshots, every number GINFO
        # shows must be new.
        set fresh {
            foreach field {generation candidate} {
                set number [gp_info b:numbers $field]
                if {$number != 0} {
                    assert {$number > $used}
                }
            }
        }
        set note { set used [expr {max($used, [gp_max_number b:numbers])}] }

        gp_prove b:numbers
        assert_equal 1 [gp_info b:numbers generation]
        eval $note

        assert_equal OK [r debug reload]
        eval $fresh
        gp_prove b:numbers
        assert {[gp_info b:numbers generation] > $used}
        eval $note

        # The gate gets worse and stops by itself ...
        for {set i 0} {$i < 200 && [gp_info b:numbers revocations] == 0} {incr i} {
            lassign [r gpredict b:numbers SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [r glearn b:numbers $id CORRECT 0]
        }
        assert_equal 1 [gp_info b:numbers revocations]
        eval $fresh
        eval $note
        # ... is reloaded before it has proven anything ...
        assert_equal OK [r debug reload]
        assert_equal 1 [gp_info b:numbers revocations]
        eval $fresh
        gp_prove b:numbers
        assert {[gp_info b:numbers generation] > $used}
        eval $note

        # ... is reset, reloaded, and proves itself once more.
        assert_equal OK [r greset b:numbers]
        eval $fresh
        eval $note
        assert_equal OK [r debug reload]
        eval $fresh
        gp_prove b:numbers
        assert {[gp_info b:numbers generation] > $used}
        assert {[gp_info b:numbers generation] >= 4}
    }

    gp_test {A classifier that was reset keeps counting snapshots where it stopped, across a reload} {
        gp_begin 208
        gp_classifier b:ledger 100
        gp_teach b:ledger 100
        gp_prove b:ledger
        gp_teach b:ledger 100
        set used [gp_max_number b:ledger]
        assert {$used >= 2}
        assert_equal OK [r greset b:ledger]
        assert_equal 0 [gp_info b:ledger learned]
        assert_equal OK [r debug reload]
        assert_equal 0 [gp_info b:ledger learned]
        assert_equal COLD [gp_info b:ledger state]
        gp_teach b:ledger 100
        gp_prove b:ledger
        assert {[gp_info b:ledger generation] > $used}
    }

    gp_test {MEMORY USAGE and GINFO memory are the same before and after DEBUG RELOAD} {
        gp_begin 209
        gp_classifier m:c 100
        gp_teach m:c 100
        gp_prove m:c
        gp_teach m:c 100
        gp_collect m:c 5
        gp_score m:s
        gp_prove m:s
        gp_collect m:s 5
        foreach key {m:c m:s} {
            set m_usage($key) [r memory usage $key]
            set m_memory($key) [gp_info $key memory]
            # The key holds at least what the gate says it holds.
            assert {$m_usage($key) >= $m_memory($key)}
            assert {$m_memory($key) > 0}
        }
        # A classifier holds a model, a score gate does not.
        assert {$m_memory(m:c) > $m_memory(m:s) + 8192 * 4}
        assert_equal OK [r debug reload]
        foreach key {m:c m:s} {
            assert_equal $m_memory($key) [gp_info $key memory] "GINFO memory of $key"
            if {[string match {*jemalloc*} [s mem_allocator]]} {
                assert_equal $m_usage($key) [r memory usage $key] "MEMORY USAGE of $key"
            }
        }
    }
}

# ====================== RDB: SAVE, restart, missing module ==================

set gp_dir [tmpdir "server.gate-persistence"]
set gp_saved {}
set gp_ready 0

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule dir $gp_dir] keep_persistence true] {

    gp_test {A server with the module and an empty dataset starts, saves and restarts} {
        assert_equal 0 [r dbsize]
        assert_match "*gate*" [r module list]
        assert_equal OK [r debug reload]
        assert_equal OK [r save]
        restart_server 0 true false
        assert_equal PONG [r ping]
        assert_equal 0 [r dbsize]
        assert_error "ERR no such key" {r ginfo nothing}
    }

    gp_test {SAVE and restart: every gate comes back with the digest it had} {
        gp_begin 301
        # Settings that are not the defaults: after the restart the server
        # has the defaults again, the gates must not.
        r config set gate.feedback-timeout 500
        gp_classifier r:cold
        gp_score r:late
        r config set gate.feedback-timeout 300000
        gp_teach r:cold 10
        gp_collect r:cold 3
        gp_classifier r:cal
        gp_teach r:cal 20
        gp_collect r:cal 4
        gp_classifier r:act 100
        gp_teach r:act 100
        gp_prove r:act
        set r_asked(r:act) [gp_until_accept r:act]
        gp_teach r:act 100
        set r_old(r:act) [gp_collect r:act 3]
        gp_score r:score
        gp_prove r:score
        set r_asked(r:score) [gp_until_accept r:score]
        set r_old(r:score) [gp_collect r:score 3]
        gp_score r:paused
        gp_prove r:paused
        r gpause r:paused
        r set r:string "not a gate"
        set keys {r:cold r:late r:cal r:act r:score r:paused}
        foreach key $keys {
            set r_info($key) [gp_info $key]
            set r_used($key) [gp_max_number $key]
        }
        set before [gp_digests $keys]
        assert_equal OK [r save]
        restart_server 0 true false
        assert_equal $before [gp_digests $keys]
        assert_equal "not a gate" [r get r:string]
        foreach key $keys {
            assert_equal gate-type [r type $key]
            gp_assert_came_back $key $r_info($key)
        }
    }

    gp_test {After a restart a gate does not answer until it has proven itself again} {
        gp_begin 302
        foreach key {r:act r:score} {
            gp_not_answered $key $r_asked($key)
            foreach pair $r_old($key) {
                assert_equal 0 [gp_answer $key $pair]
                assert_equal 0 [gp_answer $key $pair]
            }
            gp_prove $key
            assert_equal ACTIVE [gp_info $key state]
            assert {[gp_info $key generation] > $r_used($key)}
            gp_until_accept $key
        }
        # The paused gate is still paused, whatever it proves.
        for {set n 0} {![gp_proven r:paused]} {incr n} {
            assert {$n < 600}
            assert {[gp_ask r:paused score] ne "ACCEPT"}
        }
        assert_equal PAUSED [gp_info r:paused state]
        assert {[gp_info r:paused generation] > $r_used(r:paused)}
    }

    gp_test {After a restart a gate still has the snapshot step and the feedback window it was created with} {
        gp_begin 303
        # The server is back to its defaults.
        assert_equal 1000 [lindex [r config get gate.snapshot-examples] 1]
        assert_equal 300000 [lindex [r config get gate.feedback-timeout] 1]
        # Created with a snapshot step of 20: 10 examples before the
        # restart, 10 more now.
        assert_equal COLD [gp_info r:cold state]
        assert_equal 10 [gp_info r:cold learned]
        gp_teach r:cold 9
        assert_equal COLD [gp_info r:cold state]
        gp_teach r:cold 1
        assert_equal CALIBRATING [gp_info r:cold state]
        assert {[gp_info r:cold candidate] > 0}
        # Created with a feedback window of half a second.
        set waiting [gp_collect r:late 2]
        set waitingc [gp_collect r:cold 2]
        assert_equal 1 [gp_answer r:late [lindex $waiting 0]]
        assert_equal 1 [gp_answer r:cold [lindex $waitingc 0]]
        after 650
        assert_equal 0 [gp_answer r:late [lindex $waiting 1]] "late feedback for r:late"
        assert_equal 0 [gp_answer r:cold [lindex $waitingc 1]] "late feedback for r:cold"
        assert {[gp_info r:late missing] >= 1}
    }

    gp_test {The two settings survive CONFIG REWRITE and a restart, and are for new gates only} {
        gp_begin 305
        # A gate created with a snapshot step of 30, 10 examples so far.
        gp_classifier r:thirty 30
        gp_teach r:thirty 10
        r config set gate.snapshot-examples 7
        r config set gate.feedback-timeout 86400000
        assert_equal OK [r config rewrite]
        assert_equal OK [r save]
        restart_server 0 true false
        assert_equal 7 [lindex [r config get gate.snapshot-examples] 1]
        assert_equal 86400000 [lindex [r config get gate.feedback-timeout] 1]
        gp_begin 306
        # A gate created now takes its first snapshot after 7 examples ...
        assert_equal OK [r gcreate r:seven ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        gp_teach r:seven 6
        assert_equal COLD [gp_info r:seven state]
        gp_teach r:seven 1
        assert_equal CALIBRATING [gp_info r:seven state]
        # ... the one that was there after 30.
        gp_teach r:thirty 19
        assert_equal COLD [gp_info r:thirty state]
        assert_equal 29 [gp_info r:thirty learned]
        gp_teach r:thirty 1
        assert_equal CALIBRATING [gp_info r:thirty state]
        r config set gate.snapshot-examples 1000
        r config set gate.feedback-timeout 300000
    }

    gp_test {redis-check-rdb accepts an RDB file with gates} {
        assert_equal OK [r save]
        set gp_saved [gp_digests $keys]
        set gp_ready [count_message_lines $gp_dir/stdout "Ready to accept"]
        catch {exec src/redis-check-rdb $gp_dir/dump.rdb} output
        assert_match "*RDB looks OK*" $output
    }
}

# The same directory, without the module.
start_server [list tags {"modules external:skip"} overrides [list dir $gp_dir] keep_persistence true wait_ready false] {
    gp_test {A server without the module refuses to load a dataset with gates} {
        wait_for_condition 200 50 {
            ![is_alive [srv pid]]
        } else {
            fail "the server is still running"
        }
        assert {[count_message_lines $gp_dir/stdout "no matching module type 'gate-type'"] >= 1}
        # It never got as far as taking connections.
        assert_equal $gp_ready [count_message_lines $gp_dir/stdout "Ready to accept"]
    }
}

# The same directory, with the module again.
start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule dir $gp_dir]] {
    gp_test {The dataset the other server refused loads again once the module is back} {
        assert {[llength $gp_saved] == 6}
        assert_equal $gp_saved [gp_digests {r:cold r:late r:cal r:act r:score r:paused}]
        assert_equal "not a gate" [r get r:string]
        gp_begin 304
        assert {[gp_ask r:act] ne "ACCEPT"}
    }
}

# ================================== AOF ====================================

# A bit of everything on a handful of gates. Returns their keys.
proc gp_workload {p} {
    # A classifier and a score gate that prove themselves, answer, and
    # have requests waiting.
    gp_classifier $p:classifier
    gp_teach $p:classifier 20
    gp_prove $p:classifier
    gp_until_accept $p:classifier
    set waiting [gp_collect $p:classifier 3]
    gp_score $p:score
    gp_prove $p:score
    gp_until_accept $p:score
    gp_collect $p:score 3
    # Feedback that is not used: an id used twice, an id never issued.
    assert_equal 1 [gp_answer $p:classifier [lindex $waiting 1]]
    assert_equal 0 [gp_answer $p:classifier [lindex $waiting 1]]
    assert_equal 0 [r glearn $p:classifier 0123456789abcdef-7 LABEL spam]
    assert_equal 0 [r glearn $p:score 0123456789abcdef-7 CORRECT 1]
    assert_equal 0 [r glearn $p:score not-an-id CORRECT 1]
    # Options, labels that are not plain words, named features, a text
    # that is not text.
    set utf8 [encoding convertto utf-8 "h\u00e9h\u00e9"]
    r config set gate.snapshot-examples 6
    assert_equal OK [r gcreate $p:odd ALPHA 0.3 DELTA 0.5 AUDIT 1 CLASSES 3 "oui merci" SCORE $utf8 \
        ALWAYSDEFER 1 SCORE CLASSALPHA 1 "oui merci" 0.1]
    r config set gate.snapshot-examples 1000
    set i 0
    foreach label [list "oui merci" SCORE $utf8 "oui merci" SCORE $utf8 "oui merci" SCORE] {
        r gadd $p:odd TEXT "topic[incr i] of $label" FEATURES 2 depth 0.5 mentioned 1 LABEL $label
    }
    lassign [r gpredict $p:odd TEXT "bin\x00 ary \xff\xfe\r\n text" FEATURES 1 depth -0.25] action id
    assert_equal 1 [r glearn $p:odd $id LABEL $utf8]
    r gpredict $p:odd TEXT "" FEATURES 0
    r gpredict $p:odd TEXT [string repeat "long text " 500]
    # Paused, paused and resumed, reset.
    gp_score $p:paused
    gp_prove $p:paused
    assert_equal OK [r gpause $p:paused]
    for {set i 0} {$i < 6} {incr i} { gp_ask $p:paused score }
    gp_score $p:resumed
    assert_equal OK [r gpause $p:resumed]
    gp_prove $p:resumed
    assert_equal OK [r gresume $p:resumed]
    gp_until_accept $p:resumed
    gp_classifier $p:reset
    gp_teach $p:reset 20
    gp_collect $p:reset 2
    assert_equal OK [r greset $p:reset]
    gp_teach $p:reset 3
    gp_collect $p:reset 1
    # Deleted and created again as the other kind; renamed with requests
    # waiting.
    gp_score $p:again
    gp_collect $p:again 2
    assert_equal 1 [r del $p:again]
    gp_classifier $p:again
    gp_teach $p:again 4
    gp_score $p:old
    set waiting [gp_collect $p:old 2]
    assert_equal OK [r rename $p:old $p:renamed]
    assert_equal 1 [gp_answer $p:renamed [lindex $waiting 0]]
    # In a transaction and in a script.
    gp_score $p:multi
    r multi
    r gpredict $p:multi SCORE 0.9
    r gpredict $p:multi SCORE 0.8
    r gpause $p:multi
    r gresume $p:multi
    set replies [r exec]
    assert_equal ABSTAIN [lindex $replies 0 0]
    assert_equal 1 [r eval {return redis.call('GLEARN', KEYS[1], ARGV[1], 'CORRECT', '1')} 1 $p:multi [lindex $replies 0 1]]
    assert_equal ABSTAIN [r eval {
        local reply = redis.call('GPREDICT', KEYS[1], 'SCORE', '0.7')
        redis.call('GLEARN', KEYS[1], reply[2], 'CORRECT', '0')
        return reply[1]
    } 1 $p:multi]
    # A copy made with DUMP and RESTORE and used afterwards; a time to live.
    assert_equal OK [r restore $p:copy 0 [r dump $p:score]]
    for {set i 0} {$i < 8} {incr i} { gp_ask $p:copy score }
    gp_collect $p:copy 2
    assert_equal 1 [r pexpire $p:paused 100000000]
    return [list $p:classifier $p:score $p:odd $p:paused $p:resumed $p:reset $p:again $p:renamed \
                 $p:multi $p:copy]
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule appendonly yes]] {

    gp_test {AOF: DEBUG LOADAOF gives every gate the digest it had} {
        gp_begin 401
        set keys [gp_workload w]
        r set w:string "not a gate"
        # Every state is there.
        set states {}
        foreach key $keys { dict incr states [gp_info $key state] }
        foreach state {COLD CALIBRATING ACTIVE PAUSED} { assert {[dict exists $states $state]} }
        set before [gp_digests $keys]
        set all [debug_digest]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests $keys]
        assert_equal $all [debug_digest]
        assert_equal "not a gate" [r get w:string]
        assert_equal 0 [r exists w:old]
        # Nothing in the file was refused by the gate it was for.
        assert_equal 0 [count_log_message 0 "did not fit"]
    }

    gp_test {AOF: the file holds what happened (_GATE.APPLY), never the commands} {
        gp_begin 411
        gp_zoo f
        set names [gp_aof_commands [get_last_incr_aof_path r]]
        gp_assert_realized $names
        assert {[dict get $names _gate.apply] > 100}
        # Besides that, only what the tests did to the keys as keys.
        foreach name [dict keys $names] {
            assert {$name in {select _gate.apply set del rename restore pexpireat multi exec}}
        }
    }

    gp_test {AOF: gates loaded from the AOF start without proof and prove themselves again} {
        gp_begin 402
        foreach {key kind} {a:c classifier a:s score} {
            if {$kind eq "classifier"} {
                gp_classifier $key 100
                gp_teach $key 100
            } else {
                gp_score $key
            }
            gp_prove $key
            set a_question($key) [gp_until_accept $key]
            set a_old($key) [gp_collect $key 3]
            set a_info($key) [gp_info $key]
            set a_used($key) [gp_max_number $key]
        }
        set before [gp_digests {a:c a:s}]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests {a:c a:s}]
        foreach key {a:c a:s} {
            gp_assert_came_back $key $a_info($key)
            gp_not_answered $key $a_question($key)
            foreach pair $a_old($key) { assert_equal 0 [gp_answer $key $pair] }
            gp_prove $key
            assert {[gp_info $key generation] > $a_used($key)}
            gp_until_accept $key
        }
        # What they did since is in the file too.
        set before [gp_digests {a:c a:s}]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests {a:c a:s}]
        assert_equal 0 [count_log_message 0 "did not fit"]
        gp_assert_realized [gp_aof_commands [get_last_incr_aof_path r]]
    }

    gp_test {AOF: a request the gate answers by itself writes nothing} {
        gp_begin 409
        assert_equal OK [r gcreate a:quiet ALPHA 0.2 DELTA 0.2 AUDIT 0.000000001 SCORE]
        for {set i 0} {$i < 200 && [gp_info a:quiet state] ne "ACTIVE"} {incr i} {
            lassign [r gpredict a:quiet SCORE 0.9] action id
            assert_equal 1 [r glearn a:quiet $id CORRECT 1]
        }
        assert_equal ACTIVE [gp_info a:quiet state]
        # Every write is in the file before its reply is sent.
        r config set appendfsync always
        assert_equal OK [r gpause a:quiet]
        assert_equal OK [r gresume a:quiet]
        set path [get_last_incr_aof_path r]
        set size [file size $path]
        set changes [s rdb_changes_since_last_save]
        set digest [debug_digest_value a:quiet]
        for {set i 0} {$i < 50} {incr i} {
            assert_equal {ACCEPT {}} [r gpredict a:quiet SCORE 0.9]
        }
        assert_equal 50 [gp_info a:quiet accepted]
        assert_equal $size [file size $path] "size of the AOF"
        assert_equal $changes [s rdb_changes_since_last_save]
        assert_equal $digest [debug_digest_value a:quiet]
        r config set appendfsync everysec
        # The gate is loaded from the file as it is, and its counters still
        # add up.
        assert_equal OK [r debug loadaof]
        assert_equal $digest [debug_digest_value a:quiet]
        set info [gp_info a:quiet]
        assert_equal [dict get $info requests] \
            [expr {[dict get $info accepted] + [dict get $info audited] + [dict get $info abstained]}]
    }

    gp_test {AOF: a request that times out while the gate answers by itself is in the file} {
        gp_begin 403
        # Nearly no spot checks, so that the gate answers whatever is drawn.
        r config set gate.feedback-timeout 1000
        assert_equal OK [r gcreate a:tick ALPHA 0.2 DELTA 0.2 AUDIT 0.000000001 SCORE]
        r config set gate.feedback-timeout 300000
        # More requests than the proof needs, answered in order until the
        # gate answers: the rest keeps waiting.
        set ids {}
        for {set i 0} {$i < 45} {incr i} { lappend ids [lindex [r gpredict a:tick SCORE 0.9] 1] }
        set answered 0
        foreach id $ids {
            if {[gp_info a:tick state] eq "ACTIVE"} break
            assert_equal 1 [r glearn a:tick $id CORRECT 1]
            incr answered
        }
        assert_equal ACTIVE [gp_info a:tick state]
        set waiting [expr {45 - $answered}]
        assert {$waiting >= 1}
        assert_equal $waiting [gp_info a:tick pending]
        after 1100
        # This request is answered by the gate: nothing about it goes to
        # the file. That the waiting requests are now too old must.
        assert_equal {ACCEPT {}} [r gpredict a:tick SCORE 0.9]
        set before [debug_digest_value a:tick]
        assert_equal OK [r debug loadaof]
        assert_equal $before [debug_digest_value a:tick]
        assert_equal 0 [r glearn a:tick [lindex $ids end] CORRECT 1]
        assert {[gp_info a:tick missing] >= $waiting}
        assert_equal 0 [gp_info a:tick pending]
        assert_equal 0 [count_log_message 0 "did not fit"]
    }

    foreach {preamble seed} {yes 404 no 405} {
        gp_test "AOF: BGREWRITEAOF with aof-use-rdb-preamble $preamble, then DEBUG LOADAOF: same digests" {
            gp_begin $seed
            r config set aof-use-rdb-preamble $preamble
            set keys [gp_workload rw$preamble]
            set before [gp_digests $keys]
            set all [debug_digest]
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            # The history is now one snapshot.
            set base [get_base_aof_path r]
            if {$preamble eq "yes"} {
                assert_match "*.base.rdb" $base
            } else {
                assert_match "*.base.aof" $base
                gp_assert_realized [gp_aof_commands $base]
            }
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $keys]
            assert_equal $all [debug_digest]
            # More of the same on top of the snapshot.
            gp_begin [expr {$seed + 10}]
            foreach key $keys {
                if {[gp_info $key kind] eq "score" || [llength [gp_info $key classes]] == 2} {
                    for {set i 0} {$i < 12} {incr i} { gp_ask $key }
                    gp_collect $key 2
                }
            }
            set before [gp_digests $keys]
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $keys]
            # Another rewrite, this time while the gates are in use: what
            # they do meanwhile goes on top of the snapshot.
            r config set rdb-key-save-delay 15000
            r bgrewriteaof
            wait_for_condition 100 10 {
                [s aof_rewrite_in_progress] == 1
            } else {
                fail "the rewrite did not start"
            }
            r config set rdb-key-save-delay 0
            foreach key [list rw$preamble:score rw$preamble:classifier rw$preamble:copy] {
                for {set i 0} {$i < 6} {incr i} { gp_ask $key }
                gp_collect $key 1
            }
            assert_equal OK [r greset rw$preamble:reset]
            assert_equal 1 [s aof_rewrite_in_progress] "the rewrite was over before the gates changed: machine too slow for this test"
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            set before [gp_digests $keys]
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $keys]
            assert_equal 0 [count_log_message 0 "did not fit"]
            gp_assert_realized [gp_aof_commands [get_last_incr_aof_path r]]
        }
    }

    gp_test {AOF: a snapshot that takes over, and gates that stop by themselves, are the same after DEBUG LOADAOF} {
        set took [gp_takeover a:took 420]
        gp_collect $took 3
        gp_begin 410
        # With AUDIT 1 every request the gate could answer is a spot check:
        # wrong answers make it stop.
        gp_score a:revoked 1
        gp_prove a:revoked
        gp_classifier a:crevoked 100 1
        gp_teach a:crevoked 100
        gp_prove a:crevoked
        foreach key {a:revoked a:crevoked} {
            set kind [gp_info $key kind]
            for {set i 0} {$i < 200 && [gp_info $key revocations] == 0} {incr i} {
                lassign [gp_request $kind [expr {2 * $i + 1}]] question answer
                lassign [r gpredict $key {*}$question] action id
                assert_equal AUDIT $action
                set wrong [expr {$kind eq "score" ? "CORRECT 0" : "LABEL ham"}]
                assert_equal 1 [r glearn $key $id {*}$wrong]
            }
            assert_equal 1 [gp_info $key revocations]
            gp_collect $key 2
        }
        set keys [list $took a:revoked a:crevoked]
        set before [gp_digests $keys]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests $keys]
        assert_equal 1 [gp_info a:revoked revocations]
        assert_equal 1 [gp_info a:crevoked revocations]
        assert_equal 0 [count_log_message 0 "did not fit"]
    }

    gp_test {AOF: a DEBUG RELOAD in the middle of the history does not break it} {
        gp_begin 408
        set zoo [gp_zoo az]
        set keys [dict keys $zoo]
        assert_equal OK [r debug reload]
        # The gates go on from what the RDB file gave them: this is where
        # they drop their proof and their waiting requests. Each starts
        # with another command.
        assert_equal OK [r greset az:reset]
        assert_equal OK [r gresume az:spaused]
        assert_equal OK [r gpause az:sact]
        gp_teach az:cold 1
        foreach key $keys {
            assert_equal 0 [gp_answer $key [lindex [dict get $zoo $key] 0]]
            gp_ask $key
            gp_collect $key 2
        }
        set before [gp_digests $keys]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests $keys]
        assert_equal 0 [count_log_message 0 "did not fit"]
    }

    gp_test {AOF: after a restart every gate has the digest it had, and none answers before proving itself again} {
        gp_begin 406
        # Two gates that answer right now, next to everything the tests
        # above left in the dataset.
        gp_classifier z:c 100
        gp_teach z:c 100
        gp_score z:s
        foreach key {z:c z:s} {
            gp_prove $key
            set a_question($key) [gp_until_accept $key]
            set a_old($key) [gp_collect $key 2]
            set a_used($key) [gp_max_number $key]
        }
        r set z:string "not a gate"
        set keys {}
        foreach key [lsort [r keys *]] {
            if {[r type $key] eq "gate-type"} { lappend keys $key }
        }
        foreach key $keys { set a_info($key) [gp_info $key] }
        set before [gp_digests $keys]
        restart_server 0 true false
        assert_equal $before [gp_digests $keys]
        assert_equal "not a gate" [r get z:string]
        assert_equal 0 [count_log_message 0 "did not fit"]
        foreach key $keys { gp_assert_came_back $key $a_info($key) }
        gp_begin 407
        foreach key {z:c z:s} {
            gp_not_answered $key $a_question($key)
            foreach pair $a_old($key) { assert_equal 0 [gp_answer $key $pair] }
            gp_prove $key
            assert {[gp_info $key generation] > $a_used($key)}
            gp_until_accept $key
        }
        # And once more through the file.
        set before [gp_digests $keys]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests $keys]
        assert_equal 0 [count_log_message 0 "did not fit"]
    }
}

# ============================== DUMP / RESTORE ==============================

set gp_payload {}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    foreach {kind seed} {classifier 501 score 502} {
        gp_test "DUMP / RESTORE: the copy of a $kind gate has the digest of the original until it is written to" {
            gp_begin $seed
            set key d:$kind
            if {$kind eq "classifier"} {
                gp_classifier $key 100
                gp_teach $key 100
            } else {
                gp_score $key
            }
            gp_prove $key
            set asked [gp_until_accept $key]
            set old [gp_collect $key 3]
            set info [gp_info $key]
            set used [gp_max_number $key]
            set digest [debug_digest_value $key]
            set payload [r dump $key]
            if {$kind eq "score"} { set gp_payload $payload }

            assert_equal OK [r restore $key:copy 0 $payload]
            assert_equal gate-type [r type $key:copy]
            assert_equal $digest [debug_digest_value $key:copy]
            assert_equal $digest [debug_digest_value $key]
            assert_equal [gp_info $key memory] [gp_info $key:copy memory]
            # The copy came back without proof. The original was not
            # touched: it still answers.
            gp_assert_came_back $key:copy $info
            assert_equal $info [gp_info $key]
            assert_equal ACTIVE [gp_info $key state]
            # Reading the copy does not change it.
            gp_assert_same_bytes $payload [r dump $key:copy] "DUMP of the copy and of the original"
            assert_equal $digest [debug_digest_value $key:copy]

            # The copy sends the request the original answered to the
            # teacher ...
            set id [gp_not_answered $key:copy $asked]
            # ... which changed nothing for the original.
            assert_equal $digest [debug_digest_value $key]
            # Requests of the original are not requests of the copy. They
            # are still good where they were asked.
            foreach pair $old {
                assert_equal 0 [gp_answer $key:copy $pair]
            }
            assert_equal 1 [gp_answer $key [lindex $old 0]]
            # And the request of the copy is not one of the original.
            assert_equal 0 [r glearn $key $id {*}[lindex $old 0 1]]
            # The copy proves itself again, with a new snapshot number.
            gp_prove $key:copy
            assert_equal ACTIVE [gp_info $key:copy state]
            assert {[gp_info $key:copy generation] > $used}
            gp_until_accept $key:copy
            # The original went on answering all along.
            assert_equal ACTIVE [gp_info $key state]
            assert_equal [dict get $info generation] [gp_info $key generation]
            gp_until_accept $key
        }
    }

    gp_test {DUMP / RESTORE: REPLACE over a gate, over another type, onto itself; BUSYKEY without it} {
        gp_begin 503
        gp_classifier d:target
        gp_teach d:target 20
        set told [gp_collect d:target 3]
        set tdigest [debug_digest_value d:target]
        gp_score d:source
        gp_prove d:source
        gp_until_accept d:source
        set sold [gp_collect d:source 2]
        set sinfo [gp_info d:source]
        set sdigest [debug_digest_value d:source]
        set payload [r dump d:source]

        # Without REPLACE the target is not touched.
        assert_error "BUSYKEY*" {r restore d:target 0 $payload}
        assert_equal $tdigest [debug_digest_value d:target]
        assert_equal 1 [gp_answer d:target [lindex $told 0]]

        # With REPLACE it becomes a copy of the source.
        assert_equal OK [r restore d:target 0 $payload replace]
        assert_equal score [gp_info d:target kind]
        assert_equal $sdigest [debug_digest_value d:target]
        gp_assert_came_back d:target $sinfo
        # Nothing of the gate that was there is left.
        assert_error "ERR*" {gp_answer d:target [lindex $told 1]}
        assert_equal 0 [r glearn d:target [lindex $told 1 0] CORRECT 1]
        assert_equal 0 [gp_answer d:target [lindex $sold 0]]
        # The source is as it was.
        assert_equal $sdigest [debug_digest_value d:source]
        assert_equal 1 [gp_answer d:source [lindex $sold 0]]

        # Over a key of another type, and another type over a gate.
        r set d:string "a string"
        set string [r dump d:string]
        assert_equal OK [r restore d:string 0 $payload replace]
        assert_equal gate-type [r type d:string]
        assert_equal $sdigest [debug_digest_value d:string]
        assert_equal OK [r restore d:string 0 $string replace]
        assert_equal "a string" [r get d:string]

        # With a time to live (which is part of the digest of a key).
        assert_equal OK [r restore d:ttl 100000 $payload]
        assert_range [r pttl d:ttl] 1 100000
        gp_assert_same_bytes $payload [r dump d:ttl] "DUMP of the copy with a time to live"
        set ttldigest [debug_digest_value d:ttl]
        assert_equal OK [r debug reload]
        assert_range [r pttl d:ttl] 1 100000
        assert_equal $ttldigest [debug_digest_value d:ttl]
        gp_assert_same_bytes $payload [r dump d:ttl] "DUMP of the copy after DEBUG RELOAD"

        # A gate restored onto itself is a gate that came back: it has to
        # prove itself again.
        gp_score d:self
        gp_prove d:self
        set asked [gp_until_accept d:self]
        set old [gp_collect d:self 2]
        set info [gp_info d:self]
        set used [gp_max_number d:self]
        assert_equal OK [r restore d:self 0 [r dump d:self] replace]
        gp_assert_came_back d:self $info
        gp_not_answered d:self $asked
        assert_equal 0 [gp_answer d:self [lindex $old 0]]
        gp_prove d:self
        assert {[gp_info d:self generation] > $used}
    }

    gp_test {DUMP / RESTORE: with RDB compression, of a full gate, of gates in every state} {
        gp_begin 504
        r flushall
        gp_classifier d:cold
        gp_teach d:cold 5
        gp_classifier d:cal
        gp_teach d:cal 20
        gp_classifier d:cand 100
        gp_teach d:cand 100
        gp_prove d:cand
        gp_teach d:cand 100
        gp_score d:scal
        gp_score d:sact
        gp_prove d:sact
        gp_score d:spaused
        gp_prove d:spaused
        r gpause d:spaused
        set keys {d:cold d:cal d:cand d:scal d:sact d:spaused}
        foreach key $keys {
            set waiting [gp_collect $key 4]
            gp_answer $key [lindex $waiting 2]
        }
        # A gate that holds as many requests as a gate can.
        gp_score d:full
        set rd [redis_deferring_client]
        for {set i 0} {$i < 4096} {incr i} { $rd gpredict d:full SCORE 0.5 }
        for {set i 0} {$i < 4096} {incr i} { $rd read }
        $rd close
        assert_equal 4096 [gp_info d:full pending]
        lappend keys d:full
        foreach compression {yes no} {
            r config set rdbcompression $compression
            foreach key $keys {
                set info [gp_info $key]
                set payload [r dump $key]
                assert_equal OK [r restore $key:$compression 0 $payload]
                assert_equal [debug_digest_value $key] [debug_digest_value $key:$compression] "copy of $key"
                gp_assert_came_back $key:$compression $info
                assert {[gp_ask $key:$compression] ne "ACCEPT"}
            }
        }
        # The copies made with and without compression are the same gate.
        r config set rdbcompression no
        foreach key $keys {
            assert_equal OK [r restore $key:a 0 [r dump $key]]
            r config set rdbcompression yes
            assert_equal OK [r restore $key:b 0 [r dump $key]]
            r config set rdbcompression no
            gp_assert_same_bytes [r dump $key:a] [r dump $key:b] "copies of $key made with and without compression"
        }
    }

    # ------------------------- Damaged payloads ----------------------------

    # The payloads below are many and large. Tcl writes a command in pieces
    # of its channel buffer (4 KB by default), and small pieces sent one
    # after the other wait for each other's TCP acknowledgment: 40 ms for
    # every RESTORE. One piece per command avoids that.
    fconfigure [r channel] -buffersize 1000000

    gp_test {RESTORE of a damaged payload: the test's own payload surgery is sound} {
        gp_begin 510
        r flushall
        r config set rdbcompression no
        gp_score x:score
        gp_collect x:score 3
        gp_classifier x:classifier
        gp_teach x:classifier 20
        gp_collect x:classifier 3
        foreach key {x:score x:classifier} {
            set payload [r dump $key]
            lassign [gp_dump_split $payload] head blob version
            # Split and joined again, checksum included, it is the payload.
            gp_assert_same_bytes $payload [gp_dump_join $head $blob $version] "DUMP of $key, split and joined"
            # And a payload with a wrong checksum does not get in.
            set wrong [string range $payload 0 end-1][binary format c [expr {([scan [string index $payload end] %c] + 1) & 0xff}]]
            assert_error "*DUMP payload version or checksum are wrong*" {r restore x:wrong 0 $wrong}
            assert_equal 0 [r exists x:wrong]
        }
    }

    gp_test {RESTORE of a damaged payload: a cut, padded or mislabelled gate is refused} {
        gp_begin 511
        expr {srand(511)}
        r flushall
        r config set rdbcompression no
        gp_score x:target
        gp_collect x:target 2
        set tdigest [debug_digest_value x:target]
        # Three gates: with and without a model, with and without requests.
        gp_score x:s
        gp_prove x:s
        gp_collect x:s 3
        gp_score x:new
        gp_classifier x:c
        gp_teach x:c 20
        gp_collect x:c 3
        set refused 0
        foreach key {x:s x:new x:c} {
            lassign [gp_dump_split [r dump $key]] head blob version
            set length [string length $blob]
            set cases {}
            # Cut short: at the ends, and anywhere.
            foreach at {0 1 3 4 7 8 9 16 40 100} { lappend cases "cut at $at" [string range $blob 0 [expr {$at - 1}]] }
            foreach back {1 2 8 9 100 121} { lappend cases "cut $back before the end" [string range $blob 0 end-$back] }
            for {set i 0} {$i < 20} {incr i} {
                set at [expr {int(rand() * $length)}]
                lappend cases "cut at $at" [string range $blob 0 [expr {$at - 1}]]
            }
            # Bytes after the end.
            lappend cases "one byte more" "$blob\x00"
            lappend cases "eight bytes more" "$blob[string repeat \x01 8]"
            lappend cases "twice" "$blob$blob"
            # The bytes that say what follows: magic, format versions, kind.
            for {set at 0} {$at < 9} {incr at} {
                foreach flip {1 128} {
                    set byte [expr {[scan [string index $blob $at] %c] ^ $flip}]
                    lappend cases "byte $at xor $flip" [string replace $blob $at $at [binary format c $byte]]
                }
            }
            lappend cases "nothing" ""
            lappend cases "only zeros" [string repeat "\x00" $length]
            lappend cases "only ones" [string repeat "\xff" $length]
            foreach {what damaged} $cases {
                set result [gp_restore_damaged [gp_dump_join $head $damaged $version] "$key, $what"]
                assert_equal refused $result "$key, $what"
                incr refused
            }
            # The frame around the gate.
            set payload [r dump $key]
            set body [string range $payload 0 end-8]
            set frames {}
            # A longer and a shorter string than there is.
            lappend frames "length + 1" "$head[gp_rdb_length [expr {$length + 1}]]$blob\x00$version"
            lappend frames "length - 1" "$head[gp_rdb_length [expr {$length - 1}]]$blob\x00$version"
            # Not a string, two strings, no end, another format version of
            # the module, another module.
            lappend frames "opcode" [string replace $body 10 10 "\x02"]
            lappend frames "two strings" "[string range $body 0 end-3]\x05[gp_rdb_length 4]GATE\x00$version"
            lappend frames "no end" "[string range $body 0 end-3]$version"
            lappend frames "wrong end" "[string range $body 0 end-3]\x01$version"
            lappend frames "encoding version" [string replace $body 9 9 [binary format c [expr {[scan [string index $body 9] %c] ^ 2}]]]
            lappend frames "module" [string replace $body 3 3 [binary format c [expr {[scan [string index $body 3] %c] ^ 4}]]]
            foreach {what damaged} $frames {
                set result [gp_restore_damaged [gp_dump_seal $damaged] "$key, frame: $what"]
                assert_equal refused $result "$key, frame: $what"
                incr refused
            }
            # REPLACE with a damaged payload leaves the key that is there
            # alone.
            assert_error "*Bad data format*" {r restore x:target 0 [gp_dump_join $head [string range $blob 0 end-1] $version] replace}
            assert_equal $tdigest [debug_digest_value x:target]
        }
        assert {$refused >= 180}
        assert_equal PONG [r ping]
    }

    gp_test {RESTORE of a damaged payload: a gate that GCREATE would refuse is refused} {
        gp_begin 512
        r flushall
        r config set rdbcompression no
        # Settings found in the payload by their values. Each is changed to
        # another good value first, to show that it is the setting.
        r config set gate.snapshot-examples 439041101
        r config set gate.feedback-timeout 123456789
        assert_equal OK [r gcreate x:cfg ALPHA 0.21 DELTA 0.17 AUDIT 0.33 CLASSES 2 spam hams \
            CLASSALPHA 2 spam 0.11 hams 0.13]
        r config set gate.snapshot-examples 1000
        r config set gate.feedback-timeout 300000
        lassign [gp_dump_split [r dump x:cfg]] head blob version
        set nan [binary format w 0x7ff8000000000000]
        set inf [binary format q Inf]
        set fields [list \
            alpha [binary format q 0.21] [binary format q 0.4] {alpha 0.4} \
                [list zero [binary format q 0] one [binary format q 1] negative [binary format q -0.21] \
                      "more than one" [binary format q 1.5] "not a number" $nan infinite $inf] \
            delta [binary format q 0.17] [binary format q 0.5] {delta 0.5} \
                [list zero [binary format q 0] one [binary format q 1] negative [binary format q -0.17] \
                      "not a number" $nan] \
            audit [binary format q 0.33] [binary format q 1] {audit 1} \
                [list zero [binary format q 0] "more than one" [binary format q 1.01] \
                      negative [binary format q -0.33] "not a number" $nan] \
            "the error rate of a label" [binary format q 0.11] [binary format q 0.05] {} \
                [list zero [binary format q 0] "more than ALPHA" [binary format q 0.5] \
                      negative [binary format q -0.11] "not a number" $nan] \
            "the snapshot step" [binary format i 439041101] [binary format i 7] {} \
                [list zero [binary format i 0]] \
            "the feedback window" [binary format w 123456789] [binary format w 50] {} \
                [list zero [binary format w 0] negative [binary format w -1]] \
            "a label" spam eggs {classes {eggs hams}} \
                [list "a control character" "sp\x01m" "a zero byte" "sp\x00m" DEL "sp\x7fm" \
                      "not UTF-8" "\xff\xfe\xfd\xfc" "the other label" hams] \
        ]
        set refused 0
        foreach {what old good shown bad} $fields {
            set at [string first $old $blob]
            assert {$at >= 0}
            assert_equal -1 [string first $old $blob [expr {$at + 1}]] "$what is stored once"
            set end [expr {$at + [string length $old] - 1}]
            # The good value gets in, and it is the setting.
            r del x:good
            assert_equal OK [r restore x:good 0 [gp_dump_join $head [string replace $blob $at $end $good] $version]]
            foreach {field value} $shown {
                if {$field eq "classes"} {
                    assert_equal $value [gp_info x:good classes]
                } else {
                    assert {[gp_info x:good $field] == $value}
                }
            }
            if {$what eq "the error rate of a label"} {
                assert {[gp_same_numbers {0.05 0.13} [gp_info x:good alphas]]}
            }
            # The bad ones do not.
            foreach {why value} $bad {
                assert_equal [string length $old] [string length $value]
                set payload [gp_dump_join $head [string replace $blob $at $end $value] $version]
                assert_equal refused [gp_restore_damaged $payload "$what: $why"] "$what: $why"
                incr refused
            }
        }
        assert {$refused >= 25}
        r del x:good
    }

    gp_test {RESTORE of a damaged payload: random damage to a score gate is refused or leaves a working gate} {
        gp_begin 513
        expr {srand(513)}
        r flushall
        r config set rdbcompression no
        # Fresh; being tested, with requests waiting; proven and paused,
        # with spot checks waiting.
        gp_score x:new
        gp_score x:cal
        set waiting [gp_collect x:cal 8]
        gp_answer x:cal [lindex $waiting 2]
        gp_answer x:cal [lindex $waiting 5]
        gp_score x:act 0.5
        gp_prove x:act
        gp_collect x:act 5
        r gpause x:act
        set count {refused 0 accepted 0}
        set kept {}
        foreach key {x:new x:cal x:act} {
            lassign [gp_dump_split [r dump $key]] head blob version
            set length [string length $blob]
            for {set i 0} {$i < 100} {incr i} {
                # One bit, one byte, or a few bytes.
                set damaged $blob
                set what "$key:"
                set changes [lindex {1 1 1 2 4} [expr {int(rand() * 5)}]]
                for {set j 0} {$j < $changes} {incr j} {
                    set at [expr {int(rand() * $length)}]
                    set old [scan [string index $damaged $at] %c]
                    if {$i % 2} {
                        set new [expr {$old ^ (1 << int(rand() * 8))}]
                    } else {
                        set new [expr {($old + 1 + int(rand() * 255)) & 0xff}]
                    }
                    set damaged [string replace $damaged $at $at [binary format c $new]]
                    append what " byte $at from $old to $new"
                }
                set payload [gp_dump_join $head $damaged $version]
                set result [gp_restore_damaged $payload $what]
                dict incr count $result
                if {$result eq "accepted" && [llength $kept] < 20} {
                    # Keep some of these as they came in, for a reload.
                    assert_equal OK [r restore x:kept[llength $kept] 0 $payload]
                    lappend kept x:kept[llength $kept]
                }
            }
        }
        assert_equal 300 [expr {[dict get $count refused] + [dict get $count accepted]}]
        # A good share of the damage is noticed.
        assert {[dict get $count refused] >= 50}
        # What got in can be saved and loaded.
        set before [gp_digests $kept]
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests $kept]
        foreach key $kept { assert {[gp_ask $key score] ne "ACCEPT"} }
        assert_equal PONG [r ping]
        if {$::verbose} { puts "damaged score payloads: $count" }
    }

    gp_test {RESTORE of a damaged payload: random damage to a classifier is refused or leaves a working gate} {
        gp_begin 514
        expr {srand(514)}
        r flushall
        gp_classifier x:c 100 0.5
        gp_teach x:c 100
        gp_prove x:c
        gp_teach x:c 100
        gp_collect x:c 6
        assert {[gp_info x:c candidate] > [gp_info x:c generation]}
        set count {refused 0 accepted 0}
        # As Redis compresses it: damage anywhere after the type byte.
        r config set rdbcompression yes
        set payload [r dump x:c]
        set length [string length $payload]
        assert {$length < 20000}
        for {set i 0} {$i < 100} {incr i} {
            set at [expr {1 + int(rand() * ($length - 11))}]
            set old [scan [string index $payload $at] %c]
            set new [expr {($old + 1 + int(rand() * 255)) & 0xff}]
            set damaged [string replace [string range $payload 0 end-8] $at $at [binary format c $new]]
            set result [gp_restore_damaged [gp_dump_seal $damaged] "compressed x:c: byte $at from $old to $new" 0]
            dict incr count $result
        }
        if {$::verbose} { puts "damaged compressed classifier payloads: $count" }
        assert {[dict get $count refused] >= 20}
        # As the module wrote it. Most of it is the three models, where a
        # changed byte is only another weight: aim at the rest, the first
        # bytes and the proof, requests and counters at the end.
        r config set rdbcompression no
        lassign [gp_dump_split [r dump x:c]] head blob version
        set length [string length $blob]
        assert {$length > 3 * 8192 * 4}
        for {set i 0} {$i < 100} {incr i} {
            if {$i % 2} {
                set at [expr {int(rand() * 256)}]
            } else {
                set at [expr {$length - 1 - int(rand() * 4500)}]
            }
            set old [scan [string index $blob $at] %c]
            if {$i % 4 < 2} {
                set new [expr {$old ^ (1 << int(rand() * 8))}]
            } else {
                set new [expr {($old + 1 + int(rand() * 255)) & 0xff}]
            }
            set damaged [string replace $blob $at $at [binary format c $new]]
            set result [gp_restore_damaged [gp_dump_join $head $damaged $version] "x:c: byte $at from $old to $new"]
            dict incr count $result
        }
        assert_equal 200 [expr {[dict get $count refused] + [dict get $count accepted]}]
        assert_equal PONG [r ping]
        if {$::verbose} { puts "damaged classifier payloads: $count" }
    }

    gp_test {RESTORE of a damaged payload: a refused payload, and a restored gate that is deleted, leave no memory behind} {
        gp_begin 517
        r flushall
        r config set rdbcompression no
        # A gate with three models and requests that hold their clues.
        gp_classifier x:c 100 0.5
        gp_teach x:c 100
        gp_prove x:c
        gp_teach x:c 100
        gp_collect x:c 6
        lassign [gp_dump_split [r dump x:c]] head blob version
        set good [gp_dump_join $head $blob $version]
        # Damage that is only noticed when nearly everything has been read.
        set late [list \
            [gp_dump_join $head [string range $blob 0 end-1] $version] \
            [gp_dump_join $head [string range $blob 0 end-130] $version] \
            [gp_dump_join $head "$blob\x00" $version]]
        # Once, so that the buffers of the connection have their size.
        foreach payload $late {
            assert_error "*Bad data format*" {r restore x:leak 0 $payload}
        }
        assert_equal OK [r restore x:leak 0 $good]
        gp_ask x:leak
        assert_equal 1 [r del x:leak]
        set before [s used_memory]
        for {set i 0} {$i < 50} {incr i} {
            foreach payload $late {
                assert_error "*Bad data format*" {r restore x:leak 0 $payload}
            }
            assert_equal OK [r restore x:leak 0 $good]
            gp_ask x:leak
            assert_equal 1 [r del x:leak]
        }
        set grown [expr {[s used_memory] - $before}]
        # One model alone is 32 KB: 150 refused payloads and 50 deleted gates
        # that kept anything would show.
        assert {$grown < 256 * 1024}
    }

    gp_test {RESTORE of a damaged payload: a gate that got in can still be saved and loaded once it has been used (counters at the top of their range)} {
        gp_begin 515
        r flushall
        r config set rdbcompression no
        # A score gate that was reset six times has used seven snapshot
        # numbers, and after four requests the next request number is 5.
        gp_score x:top
        for {set i 0} {$i < 6} {incr i} { r greset x:top }
        gp_collect x:top 4
        assert_equal 7 [gp_info x:top candidate]
        lassign [gp_dump_split [r dump x:top]] head blob version
        set top [binary format w 0x7fffffffffffffff]
        set problems {}
        # The payload holds the first number in its ledger, for the snapshot
        # being tested and in each of the four waiting requests: the ledger
        # comes first. It holds the second once.
        foreach {what value expected} {"snapshots ever taken" 7 6 "the next request number" 5 1} {
            # Where the payload holds that number.
            set old [binary format w $value]
            set places {}
            for {set at [string first $old $blob]} {$at >= 0} {set at [string first $old $blob [expr {$at + 1}]]} {
                lappend places $at
            }
            assert_equal $expected [llength $places] "places where the payload holds $what"
            set at [lindex $places 0]
            set payload [gp_dump_join $head [string replace $blob $at [expr {$at + 7}] $top] $version]
            # Refusing it is fine. Accepting it means it must keep working:
            # gp_restore_damaged uses the gate, then dumps and restores it.
            if {[catch {gp_restore_damaged $payload "$what at the top of its range"} problem]} {
                lappend problems [string range $problem [string length "assertion:"] end]
                r del gp:damaged gp:again
            }
        }
        if {[llength $problems]} { fail [join $problems "\n"] }
    }

    gp_test {RESTORE of a damaged payload: GINFO and GADD never show a negative count} {
        gp_begin 516
        r flushall
        r config set rdbcompression no
        gp_classifier x:count 1000
        gp_teach x:count 300
        lassign [gp_dump_split [r dump x:count]] head blob version
        # The number of learned examples, found by its value.
        set old [binary format w 300]
        set at [string first $old $blob]
        assert {$at >= 0}
        assert_equal -1 [string first $old $blob [expr {$at + 1}]]
        r del x:good
        assert_equal OK [r restore x:good 0 [gp_dump_join $head [string replace $blob $at [expr {$at + 7}] [binary format w 301]] $version]]
        assert_equal 301 [gp_info x:good learned]
        foreach value {-1 -9223372036854775808} {
            r del x:huge
            set payload [gp_dump_join $head [string replace $blob $at [expr {$at + 7}] [binary format w $value]] $version]
            if {[catch {r restore x:huge 0 $payload} reply]} {
                assert_match "*Bad data format*" $reply
                continue
            }
            assert {[gp_info x:huge learned] >= 0}
            assert {[r gadd x:huge TEXT "one more" LABEL spam] > 0}
            assert {[gp_info x:huge learned] > 0}
        }
        r del x:good x:huge
    }

    gp_test {RESTORE of a damaged payload: the server is still there} {
        assert {[is_alive [srv pid]]}
        assert_equal PONG [r ping]
        assert_equal 0 [count_log_message 0 "REDIS BUG REPORT"]
    }
}

# A gate cannot be restored where the module is not loaded.
start_server {tags {"modules external:skip"}} {
    gp_test {RESTORE of a gate into a server without the module is refused} {
        assert {[string length $gp_payload] > 0}
        assert_error "*Bad data format*" {r restore d:score 0 $gp_payload}
        assert_equal 0 [r exists d:score]
        assert_equal PONG [r ping]
    }
}
