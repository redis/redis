# Gate module, integration suite "commands": the command surface.
#
# Every test states the contract of SPEC.md (section 2 unless said otherwise):
# syntax, replies and errors of the eight G* commands in RESP2 and RESP3, and
# how a gate behaves as an ordinary Redis key (TYPE, DEL, EXPIRE, RENAME,
# MEMORY USAGE, COPY, ACL, COMMAND, CONFIG, MULTI/EXEC, Lua). Where SPEC.md is
# silent the test says so and asserts the most reasonable behaviour.
#
# Run from the repository root:
#
#   ./runtest --single ../modules/gate/tests/integration/commands \
#             --baseport 30000 --portcount 1500 --clients 1
#
# This file is plain ASCII on purpose: byte strings that are not ASCII are
# built with [binary format H* ...] so that nothing depends on the encoding
# the file is read with.

set gatemodule [file normalize modules/gate/gate.so]

# ============================================================================
# Helpers. All are prefixed gcmd_ so they cannot clash with another suite.
# ============================================================================

# A test in which an unexpected error (a command that should have worked)
# fails this one test instead of aborting the whole file.
proc gcmd_test {name code} {
    uplevel 1 [list test $name [list gcmd_guard $code]]
}

proc gcmd_guard {code} {
    set rc [catch {uplevel 1 $code} err]
    if {$rc != 1} return
    if {[string match "assertion:*" $err]} {error $err}
    error "assertion:unexpected error: $err"
}

# GINFO as a dict, or one field of it. Works on the flat RESP2 reply and on
# the RESP3 map alike.
proc gcmd_info {key {field {}}} {
    set info [r ginfo $key]
    if {$field eq {}} {return $info}
    dict get $info $field
}

# Everything that can be seen of a gate: its GINFO and its digest.
proc gcmd_state {key} {
    list [r ginfo $key] [debug_digest_value $key]
}

# 'cmd' must fail with an error matching 'pattern' and leave the gate at
# 'key' exactly as it was ("arguments are checked completely before anything
# changes").
proc gcmd_fails_unchanged {key pattern cmd} {
    set shown [string range $cmd 0 160]
    set before [gcmd_state $key]
    assert_error $pattern [list r {*}$cmd] "command: $shown"
    assert_equal [gcmd_state $key] $before "a failed command changed the gate: $shown"
}

# GCREATE with these arguments must fail with ERR and create nothing.
proc gcmd_create_fails {args} {
    set shown [string range $args 0 160]
    r del gc:bad
    assert_error "ERR *" [list r gcreate gc:bad {*}$args] "GCREATE key $shown"
    assert_equal [r exists gc:bad] 0 "a failed GCREATE left a key behind: $shown"
}

# The configuration part of GINFO.
proc gcmd_config_of {key} {
    set info [r ginfo $key]
    set out {}
    foreach field {kind alpha delta audit classes alphas} {
        lappend out [dict get $info $field]
    }
    return $out
}

proc gcmd_permutations {items} {
    if {[llength $items] <= 1} {return [list $items]}
    set out {}
    for {set i 0} {$i < [llength $items]} {incr i} {
        foreach p [gcmd_permutations [lreplace $items $i $i]] {
            lappend out [linsert $p 0 [lindex $items $i]]
        }
    }
    return $out
}

# ---------------------------------------------------------------- raw RESP
# A connection of our own on which replies are read with their wire types:
#   {status OK} {error msg} {int n} {bulk bytes} {double text} null
#   {array {item ...}} {map {key value key value ...}}
# A null is the one-element list "null" whether it came as a RESP2 null bulk,
# a RESP2 null array or the RESP3 null.

proc gcmd_raw_open {proto} {
    if {$::tls} {
        set fd [::tls::socket [srv 0 host] [srv 0 port]]
    } else {
        set fd [socket [srv 0 host] [srv 0 port]]
    }
    fconfigure $fd -translation binary
    gcmd_raw $fd HELLO $proto
    if {!$::singledb} {gcmd_raw $fd SELECT 9}
    return $fd
}

proc gcmd_raw_read {fd} {
    set line [gets $fd]
    if {$line eq {} && [eof $fd]} {error "connection closed by the server"}
    set line [string trimright $line "\r"]
    set type [string index $line 0]
    set rest [string range $line 1 end]
    switch -exact -- $type {
        + {return [list status $rest]}
        - {return [list error $rest]}
        : {return [list int $rest]}
        , {return [list double $rest]}
        _ {return [list null]}
        # {return [list bool $rest]}
        ( {return [list bignum $rest]}
        $ - = {
            if {$rest == -1} {return [list null]}
            set data [read $fd $rest]
            read $fd 2
            if {$type eq "="} {return [list verbatim $data]}
            return [list bulk $data]
        }
        * - ~ - > {
            if {$rest == -1} {return [list null]}
            set items {}
            for {set i 0} {$i < $rest} {incr i} {lappend items [gcmd_raw_read $fd]}
            if {$type eq "~"} {return [list set $items]}
            if {$type eq ">"} {return [list push $items]}
            return [list array $items]
        }
        % {
            set items {}
            for {set i 0} {$i < 2 * $rest} {incr i} {lappend items [gcmd_raw_read $fd]}
            return [list map $items]
        }
    }
    error "unexpected RESP type byte in reply line '$line'"
}

proc gcmd_raw {fd args} {
    set cmd "*[llength $args]\r\n"
    foreach a $args {append cmd "\$[string length $a]\r\n$a\r\n"}
    puts -nonewline $fd $cmd
    flush $fd
    gcmd_raw_read $fd
}

# The text of a typed reply that must be a string. The words of the API
# (OK aside) may travel as bulk or as simple strings; data must be bulk.
proc gcmd_str {typed} {
    if {[lindex $typed 0] ni {bulk status}} {
        fail "expected a string, got '$typed'"
    }
    lindex $typed 1
}

proc gcmd_bulk {typed} {
    if {[lindex $typed 0] ne "bulk"} {fail "expected a bulk string, got '$typed'"}
    lindex $typed 1
}

# A typed GPREDICT reply must be an array of exactly two elements. Returns
# {action argument} with the argument {} for a null.
proc gcmd_check_predict {typed} {
    if {[lindex $typed 0] ne "array"} {fail "GPREDICT must reply an array, got '$typed'"}
    set items [lindex $typed 1]
    assert_equal [llength $items] 2 "elements of a GPREDICT reply"
    set action [gcmd_str [lindex $items 0]]
    if {$action ni {ACCEPT ABSTAIN AUDIT}} {fail "unknown GPREDICT action '$action'"}
    set arg [lindex $items 1]
    if {$arg eq "null"} {return [list $action {}]}
    set arg [gcmd_bulk $arg]
    if {$arg eq {}} {fail "GPREDICT replied an empty argument instead of a null"}
    list $action $arg
}

# A typed map, or a typed flat field/value array, as a dict from the field
# name to the typed value.
proc gcmd_typed_dict {typed} {
    set out {}
    foreach {field value} [lindex $typed 1] {dict set out [gcmd_str $field] $value}
    return $out
}

# The fields of GINFO in the order of the table in SPEC.md, each with the
# type that table documents.
set ::gcmd_info_spec {
    kind str  state str  alpha num  delta num  audit num
    classes labels  alphas nums  thresholds nums_or_null
    generation int  candidate int  learned int
    requests int  accepted int  audited int  abstained int
    coverage num
    audit-samples int  audit-errors int  calibration-samples int
    pending int  missing int  revocations int  memory int
}

proc gcmd_info_value {proto field kind typed} {
    switch -exact -- $kind {
        str {return [gcmd_str $typed]}
        label {return [gcmd_bulk $typed]}
        int {
            if {[lindex $typed 0] ne "int"} {
                fail "GINFO $field: expected an integer, got '$typed'"
            }
            return [lindex $typed 1]
        }
        num {
            # A number that is not an integer: a double in RESP3, its
            # decimal text as a bulk string in RESP2.
            set want [expr {$proto == 3 ? "double" : "bulk"}]
            if {[lindex $typed 0] ne $want || ![string is double -strict [lindex $typed 1]]} {
                fail "GINFO $field: expected a number ($want in RESP$proto), got '$typed'"
            }
            return [lindex $typed 1]
        }
        num_or_null {
            if {$typed eq "null"} {return {}}
            return [gcmd_info_value $proto $field num $typed]
        }
        labels - nums - nums_or_null {
            if {[lindex $typed 0] ne "array"} {
                fail "GINFO $field: expected an array, got '$typed'"
            }
            set elem [dict get {labels label nums num nums_or_null num_or_null} $kind]
            set out {}
            foreach e [lindex $typed 1] {lappend out [gcmd_info_value $proto $field $elem $e]}
            return $out
        }
    }
    error "unknown GINFO value kind $kind"
}

# Check a typed GINFO reply: a map in RESP3, a flat field/value array in
# RESP2, every field of SPEC.md present, in order, with its documented type.
# Returns the fields as a dict of plain values (null -> {}).
proc gcmd_check_info {proto typed} {
    set want [expr {$proto == 3 ? "map" : "array"}]
    if {[lindex $typed 0] ne $want} {
        fail "GINFO must reply a $want in RESP$proto, got '[string range $typed 0 80]'"
    }
    set items [lindex $typed 1]
    assert_equal [llength $items] [llength $::gcmd_info_spec] "GINFO fields and values"
    set out {}
    set i 0
    foreach {field kind} $::gcmd_info_spec {
        assert_equal [gcmd_str [lindex $items $i]] $field "GINFO field [expr {$i / 2 + 1}]"
        dict set out $field [gcmd_info_value $proto $field $kind [lindex $items [expr {$i + 1}]]]
        incr i 2
    }
    return $out
}

# ------------------------------------------------------------ making proof
# Two sets of texts a classifier separates easily. Example i is a spam text
# for even i and a ham text for odd i.
set ::gcmd_spam {
    "buy cheap pills now"
    "cheap pills online, buy now!"
    "win money now, cheap"
    "buy now and win free pills"
    "cheap money: free pills online"
}
set ::gcmd_ham {
    "meeting at noon tomorrow"
    "lunch tomorrow at the office?"
    "see you at the meeting"
    "project review at noon"
    "office lunch after the review"
}

proc gcmd_example {i} {
    set k [expr {($i / 2) % 5}]
    if {$i % 2} {return [list [lindex $::gcmd_ham $k] ham]}
    list [lindex $::gcmd_spam $k] spam
}

# GCREATE a classifier that takes its first snapshot after 40 examples
# instead of 1,000. The setting is read at GCREATE, so every other gate of
# the server keeps the default and stays COLD however much a test teaches it.
proc gcmd_create_fast {key args} {
    r config set gate.snapshot-examples 40
    set rc [catch {r gcreate $key {*}$args} reply]
    r config set gate.snapshot-examples 1000
    if {$rc} {error $reply}
    return $reply
}

# GADD the first n examples to a "spam ham" classifier.
proc gcmd_teach {key n} {
    for {set i 0} {$i < $n} {incr i} {
        lassign [gcmd_example $i] text label
        r gadd $key TEXT $text LABEL $label
    }
}

# Make a "spam ham" classifier (created with gcmd_create_fast)
# answer both labels by itself: teach it, then play the client loop with a
# teacher that is always right. Nothing here depends on the random draws:
# until a label is proven every request goes to the teacher and is proof.
proc gcmd_prove_classifier {key} {
    gcmd_teach $key 40
    for {set i 0} {$i < 600} {incr i} {
        set th [gcmd_info $key thresholds]
        if {[lindex $th 0] ne {} && [lindex $th 1] ne {}} {return $i}
        lassign [gcmd_example $i] text label
        lassign [r gpredict $key TEXT $text] action id
        if {$id ne {}} {r glearn $key $id LABEL $label}
    }
    fail "classifier $key proved nothing in 600 requests: [r ginfo $key]"
}

# Make a score gate answer by itself at a threshold between 0.1 and 0.9:
# the cheap result is always right at score 0.9 and always wrong at 0.1.
proc gcmd_prove_score {key} {
    for {set i 0} {$i < 400} {incr i} {
        if {[lindex [gcmd_info $key thresholds] 0] ne {}} {return $i}
        foreach {score correct} {0.9 1 0.1 0} {
            lassign [r gpredict $key SCORE $score] action id
            if {$id ne {}} {r glearn $key $id CORRECT $correct}
        }
    }
    fail "score gate $key proved nothing in 400 rounds: [r ginfo $key]"
}

# n GPREDICTs in one pipeline; returns the replies.
proc gcmd_pipeline_predict {key n score} {
    set rd [redis_deferring_client]
    for {set i 0} {$i < $n} {incr i} {$rd gpredict $key SCORE $score}
    set replies {}
    for {set i 0} {$i < $n} {incr i} {lappend replies [$rd read]}
    $rd close
    return $replies
}

# A second connection logged in as an ACL user (who has no password).
proc gcmd_login {user} {
    set c [redis_client]
    $c auth $user anything
    return $c
}

# The protocols to test, honouring --force-resp3 and the resp3 deny tag the
# way tests/unit/moduleapi/reply.tcl does.
proc gcmd_protocols {} {
    set out {}
    foreach proto {2 3} {
        if {[lsearch $::denytags "resp3"] >= 0} {
            if {$proto == 3} continue
        } elseif {$::force_resp3} {
            if {$proto == 2} continue
        }
        lappend out $proto
    }
    return $out
}

# Every command but GCREATE, in every form, on the key KEY.
set ::gcmd_forms {
    {gpredict KEY TEXT hello}
    {gpredict KEY TEXT hello FEATURES 1 mentioned 1}
    {gpredict KEY SCORE 0.5}
    {glearn KEY 0123456789abcdef-1 LABEL a}
    {glearn KEY 0123456789abcdef-1 CORRECT 1}
    {gadd KEY TEXT hello LABEL a}
    {ginfo KEY}
    {gpause KEY}
    {gresume KEY}
    {greset KEY}
}

proc gcmd_form {form key} {
    set out {}
    foreach word $form {lappend out [expr {$word eq "KEY" ? $key : $word}]}
    return $out
}

# Gate i of the "many keys" tests: a score gate for even i, a classifier with
# labels of its own for odd i, each with its own ALPHA.
proc gcmd_many_key {i} {return "many:$i"}
proc gcmd_many_alpha {i} {format %.3f [expr {0.05 + $i / 1000.0}]}
proc gcmd_many_answer {i} {
    if {$i % 2 == 0} {return {CORRECT 1}}
    list LABEL yes$i
}

# Random choices for the random argument test. Tcl's rand() is the same
# sequence everywhere for the same srand().
proc gcmd_rnd {n} {expr {int(rand() * $n)}}
proc gcmd_pick {list} {lindex $list [gcmd_rnd [llength $list]]}
proc gcmd_picks {list min max} {
    set out {}
    set n [expr {$min + [gcmd_rnd [expr {$max - $min + 1}]]}]
    for {set i 0} {$i < $n} {incr i} {lappend out [gcmd_pick $list]}
    return $out
}

# ============================================================================
# 1. Syntax, replies and errors
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    # Draws and ids come from a seeded sequence in this server, so every run
    # of this block is the same. The seed is never set again here: setting it
    # restarts the sequence and would hand out the same ids twice.
    r config set gate.test-seed 20261128

    # ------------------------------------------------------------- GCREATE

    gcmd_test {GCREATE: a classifier gate; the reply is OK} {
        assert_equal OK [r gcreate gc:cls ALPHA 0.05 CLASSES 2 ignore respond]
        assert_equal 1 [r exists gc:cls]
        set info [r ginfo gc:cls]
        assert_equal classifier [dict get $info kind]
        assert_equal {ignore respond} [dict get $info classes]
        # Nothing is being tested before the first snapshot.
        assert_equal COLD [dict get $info state]
        assert_equal 0 [dict get $info candidate]
    }

    gcmd_test {GCREATE: a score gate; the reply is OK} {
        assert_equal OK [r gcreate gc:score ALPHA 0.05 SCORE]
        set info [r ginfo gc:score]
        assert_equal score [dict get $info kind]
        assert_equal {} [dict get $info classes]
        # SPEC 3: a score gate has nothing to freeze, its first test starts
        # at GCREATE; snapshots are numbered from 1.
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 1 [dict get $info candidate]
    }

    gcmd_test {GCREATE: AUDIT defaults to 0.05 and DELTA to 0.01} {
        foreach key {gc:cls gc:score} {
            set info [r ginfo $key]
            assert {[dict get $info alpha] == 0.05}
            assert {[dict get $info audit] == 0.05}
            assert {[dict get $info delta] == 0.01}
        }
        # Without CLASSALPHA every label has the error rate ALPHA.
        foreach a [gcmd_info gc:cls alphas] {assert {$a == 0.05}}
        assert_equal 2 [llength [gcmd_info gc:cls alphas]]
    }

    gcmd_test {GCREATE: every option is stored} {
        assert_equal OK [r gcreate gc:all ALPHA 0.1 CLASSES 3 a b c AUDIT 0.25 DELTA 0.02 \
                             ALWAYSDEFER 1 b CLASSALPHA 1 c 0.03]
        set info [r ginfo gc:all]
        assert_equal classifier [dict get $info kind]
        assert {[dict get $info alpha] == 0.1}
        assert {[dict get $info audit] == 0.25}
        assert {[dict get $info delta] == 0.02}
        assert_equal {a b c} [dict get $info classes]
        lassign [dict get $info alphas] a b c
        assert {$a == 0.1}
        assert {$b == 0}
        assert {$c == 0.03}

        assert_equal OK [r gcreate gc:all:score ALPHA 0.3 SCORE AUDIT 1 DELTA 0.5]
        set info [r ginfo gc:all:score]
        assert_equal score [dict get $info kind]
        assert {[dict get $info alpha] == 0.3}
        assert {[dict get $info audit] == 1}
        assert {[dict get $info delta] == 0.5}
        assert_equal 1 [llength [dict get $info alphas]]
        assert {[lindex [dict get $info alphas] 0] == 0.3}
    }

    gcmd_test {GCREATE: options may come in any order (classifier, all 720 orders)} {
        set groups {{ALPHA 0.1} {CLASSES 3 a b c} {AUDIT 0.25} {DELTA 0.02}
                    {ALWAYSDEFER 1 b} {CLASSALPHA 1 c 0.03}}
        set want [gcmd_config_of gc:all]
        set n 0
        foreach order [gcmd_permutations $groups] {
            set args [concat {*}$order]
            r del gc:order
            assert_equal OK [r gcreate gc:order {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:order] $want "GCREATE key $args"
            incr n
        }
        assert_equal 720 $n
        r del gc:order
    }

    gcmd_test {GCREATE: options may come in any order (score gate, all 24 orders)} {
        set groups {{ALPHA 0.3} SCORE {AUDIT 1} {DELTA 0.5}}
        set want [gcmd_config_of gc:all:score]
        foreach order [gcmd_permutations $groups] {
            set args [concat {*}$order]
            r del gc:order
            assert_equal OK [r gcreate gc:order {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:order] $want "GCREATE key $args"
        }
        r del gc:order
    }

    gcmd_test {GCREATE: option words are case-insensitive} {
        # SPEC.md does not say so in as many words; it is how every Redis
        # command reads its option words.
        set want [gcmd_config_of gc:all]
        foreach args {
            {alpha 0.1 classes 3 a b c audit 0.25 delta 0.02 alwaysdefer 1 b classalpha 1 c 0.03}
            {Alpha 0.1 Classes 3 a b c Audit 0.25 Delta 0.02 AlwaysDefer 1 b ClassAlpha 1 c 0.03}
            {aLPHA 0.1 cLASSES 3 a b c aUDIT 0.25 dELTA 0.02 aLWAYSDEFER 1 b cLASSALPHA 1 c 0.03}
        } {
            r del gc:case
            assert_equal OK [r gcreate gc:case {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:case] $want "GCREATE key $args"
        }
        set want [gcmd_config_of gc:all:score]
        foreach args {
            {alpha 0.3 score audit 1 delta 0.5}
            {Alpha 0.3 Score Audit 1 Delta 0.5}
        } {
            r del gc:case
            assert_equal OK [r gcreate gc:case {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:case] $want "GCREATE key $args"
        }
        r del gc:case
    }

    gcmd_test {GCREATE: labels may spell option words} {
        set words {ALPHA SCORE CLASSES AUDIT DELTA ALWAYSDEFER CLASSALPHA}
        assert_equal OK [r gcreate gc:words ALPHA 0.1 CLASSES 7 {*}$words]
        assert_equal $words [gcmd_info gc:words classes]

        # Also where the label lists follow each other, in the two other
        # lists, and in another case than the option word.
        assert_equal OK [r gcreate gc:words2 CLASSES 3 ALPHA score CLASSES ALPHA 0.1 \
                             ALWAYSDEFER 1 ALPHA CLASSALPHA 1 score 0.05]
        set info [r ginfo gc:words2]
        assert_equal {ALPHA score CLASSES} [dict get $info classes]
        lassign [dict get $info alphas] a b c
        assert {$a == 0}
        assert {$b == 0.05}
        assert {$c == 0.1}

        # The words of the other commands are labels like any other.
        assert_equal OK [r gcreate gc:words3 ALPHA 0.1 CLASSES 4 TEXT LABEL FEATURES CORRECT]
        assert_equal 1 [r gadd gc:words3 TEXT TEXT LABEL LABEL]
        assert_equal 2 [r gadd gc:words3 TEXT LABEL LABEL TEXT]
        assert_equal 3 [r gadd gc:words3 TEXT FEATURES LABEL FEATURES]
        assert_equal 4 [r gadd gc:words3 TEXT x FEATURES 2 LABEL 1 TEXT 0.5 LABEL CORRECT]
        lassign [r gpredict gc:words3 TEXT FEATURES] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gc:words3 $id LABEL LABEL]
        lassign [r gpredict gc:words3 TEXT TEXT FEATURES 1 FEATURES 1] action id
        assert_equal 1 [r glearn gc:words3 $id LABEL CORRECT]
    }

    gcmd_test {GCREATE: labels are compared byte for byte} {
        assert_equal OK [r gcreate gc:bytes ALPHA 0.1 CLASSES 4 a A {a } aa]
        assert_equal {a A {a } aa} [gcmd_info gc:bytes classes]
        assert_equal 1 [r gadd gc:bytes TEXT x LABEL A]
        assert_equal 2 [r gadd gc:bytes TEXT x LABEL {a }]
        gcmd_fails_unchanged gc:bytes "ERR *" {gadd gc:bytes TEXT x LABEL AA}
        gcmd_fails_unchanged gc:bytes "ERR *" {gadd gc:bytes TEXT x LABEL { a}}
    }

    gcmd_test {GCREATE: labels are 1 to 64 bytes of UTF-8 without control characters} {
        set good [list \
            x 0 -1 {not spam} {a,b;c} "it's" {"quoted"} \
            [string repeat x 64] \
            [binary format H* e697a5e69cace8aa9e] \
            [binary format H* d180d183d181] \
            [binary format H* [string repeat f09f9880 16]] \
            [binary format H* c2a0] \
            [binary format H* f48fbfbf]]
        foreach label $good {
            r del gc:label
            binary scan $label H* hex
            assert_equal OK [r gcreate gc:label ALPHA 0.1 CLASSES 2 $label other] "label 0x$hex"
            assert_equal [list $label other] [gcmd_info gc:label classes] "label 0x$hex"
            # It is the label GADD and GLEARN take.
            assert_equal 1 [r gadd gc:label TEXT hello LABEL $label] "label 0x$hex"
        }
        r del gc:label

        set bad [list \
            {} \
            [string repeat x 65] \
            [binary format H* [string repeat f09f9880 17]] \
            "a\x00b" "a\x00" "\x00" "a\nb" "a\rb" "a\tb" "\x1b" "\x7f" "a\x7f" \
            [binary format H* c280] [binary format H* c285] [binary format H* c29f] \
            [binary format H* ff] [binary format H* fffe] [binary format H* 80] \
            [binary format H* c0af] [binary format H* e08080] [binary format H* f0808080] \
            [binary format H* eda080] [binary format H* f4908080] \
            [binary format H* e282] [binary format H* 61c3]]
        foreach label $bad {
            binary scan $label H* hex
            # ... as the first, a middle and the last label
            foreach labels [list [list $label b c] [list a $label c] [list a b $label]] {
                r del gc:bad
                assert_error "ERR *" {r gcreate gc:bad ALPHA 0.1 CLASSES 3 {*}$labels} "label 0x$hex"
                assert_equal [r exists gc:bad] 0 "label 0x$hex"
            }
        }
    }

    gcmd_test {GCREATE: CLASSES lists 2 to 64 labels} {
        set labels {}
        for {set i 0} {$i < 65} {incr i} {lappend labels L$i}

        r del gc:n
        assert_equal OK [r gcreate gc:n ALPHA 0.1 CLASSES 2 L0 L1]
        assert_equal {L0 L1} [gcmd_info gc:n classes]

        # every count in between
        for {set n 2} {$n <= 64} {incr n} {
            r del gc:n
            assert_equal [r gcreate gc:n ALPHA 0.1 CLASSES $n {*}[lrange $labels 0 [expr {$n - 1}]]] OK \
                "CLASSES $n"
            set info [r ginfo gc:n]
            assert_equal [dict get $info classes] [lrange $labels 0 [expr {$n - 1}]] "CLASSES $n"
            assert_equal [llength [dict get $info alphas]] $n "CLASSES $n"
            assert_equal [llength [dict get $info thresholds]] $n "CLASSES $n"
        }
        # the gate with 64 labels takes every one of them
        for {set i 0} {$i < 64} {incr i} {
            assert_equal [expr {$i + 1}] [r gadd gc:n TEXT "an example for label $i" LABEL L$i]
        }
        lassign [r gpredict gc:n TEXT "an example for label 7"] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gc:n $id LABEL L63]
        gcmd_fails_unchanged gc:n "ERR *" {gadd gc:n TEXT x LABEL L64}
        assert_equal 65 [gcmd_info gc:n learned]

        # 64 labels with every other option as long as it can be
        r del gc:n
        set classalpha {}
        foreach l [lrange $labels 0 31] {lappend classalpha $l 0.05}
        assert_equal OK [r gcreate gc:n ALWAYSDEFER 32 {*}[lrange $labels 32 63] \
                             CLASSALPHA 32 {*}$classalpha DELTA 0.5 AUDIT 1 \
                             CLASSES 64 {*}[lrange $labels 0 63] ALPHA 0.1]
        set alphas [gcmd_info gc:n alphas]
        for {set i 0} {$i < 64} {incr i} {
            assert {[lindex $alphas $i] == ($i < 32 ? 0.05 : 0)}
        }
        r del gc:n

        gcmd_create_fails ALPHA 0.1 CLASSES 1 L0
        gcmd_create_fails ALPHA 0.1 CLASSES 65 {*}$labels
        gcmd_create_fails ALPHA 0.1 CLASSES 0
    }

    gcmd_test {GCREATE: ALPHA is required, greater than 0 and less than 1} {
        gcmd_create_fails CLASSES 2 a b
        gcmd_create_fails SCORE AUDIT 0.5 DELTA 0.5
        gcmd_create_fails CLASSES 2 a b AUDIT 0.5 DELTA 0.5
        foreach alpha {0 0.0 -0 1 1.0 -0.1 1.5 2 -1 1e308 abc {} nan NaN inf -inf +inf Infinity
                       1e999 0.1abc abc0.1 {0.1 } { 0.1} 0,1 0.1.2 0..1 -- 1e 0.1e} {
            gcmd_create_fails ALPHA $alpha SCORE
            gcmd_create_fails ALPHA $alpha CLASSES 2 a b
        }
        gcmd_create_fails ALPHA
        gcmd_create_fails SCORE ALPHA
        # The usual ways to write a number between 0 and 1 are all taken;
        # 1e-05 is how Python prints 0.00001.
        foreach {alpha value} {0.5 0.5  1e-05 0.00001  1e-1 0.1  5E-1 0.5  0.999999 0.999999
                               0.000001 0.000001  0.10000000000000001 0.1} {
            r del gc:alpha
            assert_equal OK [r gcreate gc:alpha ALPHA $alpha SCORE] "ALPHA $alpha"
            assert {[gcmd_info gc:alpha alpha] == $value}
        }
        r del gc:alpha
    }

    gcmd_test {GCREATE: AUDIT is greater than 0 and at most 1} {
        foreach audit {0 0.0 -0.5 1.01 2 abc {} nan inf -inf 1e999 {1 } 0.5x} {
            gcmd_create_fails ALPHA 0.1 SCORE AUDIT $audit
            gcmd_create_fails ALPHA 0.1 CLASSES 2 a b AUDIT $audit
        }
        gcmd_create_fails ALPHA 0.1 SCORE AUDIT
        foreach audit {1 1.0 0.5 0.001 1e-9} {
            r del gc:audit
            assert_equal OK [r gcreate gc:audit ALPHA 0.1 SCORE AUDIT $audit] "AUDIT $audit"
            assert {[gcmd_info gc:audit audit] == $audit}
        }
        r del gc:audit
    }

    gcmd_test {GCREATE: DELTA is greater than 0 and less than 1} {
        foreach delta {0 0.0 1 1.0 -0.1 2 abc {} nan inf -inf 1e999 {0.5 } 0.5x} {
            gcmd_create_fails ALPHA 0.1 SCORE DELTA $delta
            gcmd_create_fails ALPHA 0.1 CLASSES 2 a b DELTA $delta
        }
        gcmd_create_fails ALPHA 0.1 SCORE DELTA
        foreach delta {0.5 0.999 0.001 1e-9} {
            r del gc:delta
            assert_equal OK [r gcreate gc:delta ALPHA 0.1 SCORE DELTA $delta] "DELTA $delta"
            assert {[gcmd_info gc:delta delta] == $delta}
        }
        r del gc:delta
    }

    gcmd_test {GCREATE: exactly one of CLASSES and SCORE is required} {
        gcmd_create_fails ALPHA 0.1 DELTA 0.1
        gcmd_create_fails ALPHA 0.1 AUDIT 0.5 DELTA 0.1
        gcmd_create_fails ALPHA 0.1 SCORE CLASSES 2 a b
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b SCORE
        gcmd_create_fails SCORE ALPHA 0.1 CLASSES 2 a b
    }

    gcmd_test {GCREATE: ALWAYSDEFER and CLASSALPHA need CLASSES} {
        gcmd_create_fails ALPHA 0.1 SCORE ALWAYSDEFER 1 a
        gcmd_create_fails ALPHA 0.1 ALWAYSDEFER 1 a SCORE
        gcmd_create_fails ALPHA 0.1 SCORE CLASSALPHA 1 a 0.05
        gcmd_create_fails ALPHA 0.1 CLASSALPHA 1 a 0.05 SCORE
        gcmd_create_fails ALPHA 0.1 ALWAYSDEFER 1 a
        gcmd_create_fails ALPHA 0.1 CLASSALPHA 1 a 0.05
    }

    gcmd_test {GCREATE: ALWAYSDEFER and CLASSALPHA name labels of CLASSES} {
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 c
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 A
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 {}
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 "a\x00"
        gcmd_create_fails ALPHA 0.1 ALWAYSDEFER 1 c CLASSES 2 a b
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 c 0.05
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 A 0.05
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b CLASSALPHA 2 a 0.05 c 0.05
        gcmd_create_fails ALPHA 0.1 CLASSALPHA 1 c 0.05 CLASSES 2 a b
    }

    gcmd_test {GCREATE: CLASSALPHA is a stricter error rate, at most ALPHA} {
        foreach alpha {0.2 0.11 0.1000001 1 1.5 0 0.0 -0.05 abc {} nan inf 1e999 0.05x} {
            gcmd_create_fails ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 a $alpha
            gcmd_create_fails CLASSALPHA 1 a $alpha ALPHA 0.1 CLASSES 2 a b
        }
        foreach alpha {0.1 0.05 0.0999999 1e-9} {
            r del gc:ca
            assert_equal OK [r gcreate gc:ca ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 b $alpha]
            lassign [gcmd_info gc:ca alphas] a b
            assert {$a == 0.1}
            assert {$b == $alpha}
        }
        r del gc:ca
    }

    gcmd_test {GCREATE: a count must match the arguments that follow it} {
        foreach args {
            {ALPHA 0.1 CLASSES}
            {ALPHA 0.1 CLASSES 2}
            {ALPHA 0.1 CLASSES 2 a}
            {ALPHA 0.1 CLASSES 3 a b}
            {CLASSES 3 a b ALPHA 0.1}
            {ALPHA 0.1 CLASSES 2 a b c}
            {ALPHA 0.1 CLASSES -2 a b}
            {ALPHA 0.1 CLASSES two a b}
            {ALPHA 0.1 CLASSES 2.0 a b}
            {ALPHA 0.1 CLASSES {} a b}
            {ALPHA 0.1 CLASSES 2x a b}
            {ALPHA 0.1 CLASSES 4294967298 a b}
            {ALPHA 0.1 CLASSES 9223372036854775808 a b}
            {ALPHA 0.1 CLASSES 18446744073709551615 a b}
            {ALPHA 0.1 CLASSES 18446744073709551616 a b}
            {ALPHA 0.1 CLASSES 18446744073709551618 a b}
            {ALPHA 0.1 CLASSES 99999999999999999999999999 a b}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 0}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 2 a}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER -1 a}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER x a}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 4294967297 a}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 18446744073709551617 a}
            {ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 a b}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 0}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 a}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 2 a 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 2 a 0.05 b}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 a 0.05 b 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA -1 a 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA x a 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 4294967297 a 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 9223372036854775809 a 0.05}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 18446744073709551617 a 0.05}
        } {
            gcmd_create_fails {*}$args
        }
    }

    gcmd_test {GCREATE: unknown options and stray arguments are errors} {
        foreach args {
            {ALPHA 0.1 SCORE extra}
            {ALPHA 0.1 SCORE BOGUS 1}
            {BOGUS ALPHA 0.1 SCORE}
            {ALPHA 0.1 BOGUS SCORE}
            {ALPHA 0.1 SCORE 0.5}
            {ALPHA 0.1 0.2 SCORE}
            {ALPHA 0.1 SCORE TEXT hello}
            {ALPHA 0.1 SCORE LABEL a}
            {ALPHA 0.1 CLASSES 2 a b extra}
            {ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 a extra}
            {ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 a 0.05 extra}
            {ALPHA 0.1 SCORES}
            {ALPHA 0.1 SCOR}
            {ALPHAS 0.1 SCORE}
            {ALPHA 0.1 {SCORE }}
            {{} ALPHA 0.1 SCORE}
        } {
            gcmd_create_fails {*}$args
        }
        gcmd_create_fails ALPHA 0.1 "SCORE\x00"
        gcmd_create_fails ALPHA 0.1 "SCORE\x00junk"
        gcmd_create_fails "ALPHA\x00" 0.1 SCORE
    }

    gcmd_test {GCREATE: each option at most once} {
        foreach args {
            {ALPHA 0.1 ALPHA 0.1 SCORE}
            {ALPHA 0.1 SCORE ALPHA 0.2}
            {ALPHA 0.1 SCORE SCORE}
            {SCORE ALPHA 0.1 score}
            {ALPHA 0.1 SCORE AUDIT 0.5 AUDIT 0.5}
            {ALPHA 0.1 SCORE DELTA 0.5 DELTA 0.5}
            {ALPHA 0.1 SCORE DELTA 0.5 delta 0.4}
            {ALPHA 0.1 CLASSES 2 a b CLASSES 2 a b}
            {ALPHA 0.1 CLASSES 2 a b CLASSES 2 c d}
            {ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 a ALWAYSDEFER 1 b}
            {ALPHA 0.1 CLASSES 3 a b c CLASSALPHA 1 a 0.05 CLASSALPHA 1 b 0.05}
            {ALPHA 0.1 CLASSES 2 a b ALPHA 0.1}
            {ALPHA 0.1 CLASSES 2 a b AUDIT 0.5 DELTA 0.5 AUDIT 0.5}
        } {
            gcmd_create_fails {*}$args
        }
    }

    gcmd_test {GCREATE: label lists that contradict themselves are refused} {
        # SPEC.md is silent on all four; refusing is the reading that never
        # drops part of a command silently.
        # the same label twice in CLASSES
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a a
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b a
        # the same label twice in ALWAYSDEFER or in CLASSALPHA
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 2 a a
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b c CLASSALPHA 2 a 0.05 a 0.04
        # a label that is never answered and has an error rate
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 a CLASSALPHA 1 a 0.05
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b c CLASSALPHA 1 a 0.05 ALWAYSDEFER 1 a
        # a gate that may never answer anything
        gcmd_create_fails ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 2 a b
        gcmd_create_fails ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 3 c b a
        # all labels but one is fine
        r del gc:defer
        assert_equal OK [r gcreate gc:defer ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 2 c a]
        lassign [gcmd_info gc:defer alphas] a b c
        assert {$a == 0}
        assert {$b == 0.1}
        assert {$c == 0}
        r del gc:defer
    }

    gcmd_test {GCREATE: BUSYKEY on an existing key of any type; nothing is overwritten} {
        r set gc:busy:string value
        r rpush gc:busy:list a b
        r hset gc:busy:hash f v
        r sadd gc:busy:set m
        r zadd gc:busy:zset 1 m
        r xadd gc:busy:stream 1-1 f v
        r setex gc:busy:volatile 1000 value
        r gcreate gc:busy:score ALPHA 0.3 SCORE AUDIT 0.5
        r gcreate gc:busy:gate ALPHA 0.1 CLASSES 2 a b AUDIT 0.5
        r gadd gc:busy:gate TEXT hello LABEL a
        lassign [r gpredict gc:busy:gate TEXT hello] action id
        assert {$id ne {}}

        foreach type {string list hash set zset stream volatile score gate} {
            set key gc:busy:$type
            set kind [r type $key]
            set digest [debug_digest_value $key]
            foreach args {{ALPHA 0.2 SCORE} {ALPHA 0.2 CLASSES 2 x y} {ALPHA 0.2 SCORE DELTA 0.5 AUDIT 1}} {
                assert_error "BUSYKEY*" {r gcreate $key {*}$args} "GCREATE over a $type"
            }
            assert_equal [r type $key] $kind "type of the $type key"
            assert_equal [debug_digest_value $key] $digest "value of the $type key"
        }
        assert_equal value [r get gc:busy:string]
        assert_equal {a b} [r lrange gc:busy:list 0 -1]
        assert_equal value [r get gc:busy:volatile]
        assert_range [r ttl gc:busy:volatile] 1 1000

        # The gates are still the old ones, waiting request included.
        set info [r ginfo gc:busy:gate]
        assert_equal classifier [dict get $info kind]
        assert_equal {a b} [dict get $info classes]
        assert {[dict get $info alpha] == 0.1}
        assert {[dict get $info audit] == 0.5}
        assert_equal 1 [dict get $info learned]
        assert_equal 1 [dict get $info pending]
        assert_equal 1 [r glearn gc:busy:gate $id LABEL b]
        assert_equal score [gcmd_info gc:busy:score kind]
        assert {[gcmd_info gc:busy:score alpha] == 0.3}
    }

    # ------------------------------------------------------------ GPREDICT

    gcmd_test {GPREDICT: the reply is the action and its argument} {
        r gcreate gp:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gp:score ALPHA 0.1 SCORE
        foreach {key form answer} {gp:cls {TEXT {thanks everyone!}} {LABEL a}
                                   gp:score {SCORE 0.93} {CORRECT 1}} {
            set reply [r gpredict $key {*}$form]
            assert_equal 2 [llength $reply]
            lassign $reply action id
            # Nothing is proven yet: the gate does not know and asks for the
            # teacher's answer.
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            assert_equal 1 [gcmd_info $key pending]
            assert_equal 1 [r glearn $key $id {*}$answer]
            assert_equal 0 [gcmd_info $key pending]
        }
    }

    gcmd_test {GPREDICT: a retried request is a new request} {
        foreach {key form a1 a2} {gp:cls {TEXT same} {LABEL a} {LABEL b}
                                  gp:score {SCORE 0.5} {CORRECT 1} {CORRECT 0}} {
            set requests [gcmd_info $key requests]
            lassign [r gpredict $key {*}$form] action1 id1
            lassign [r gpredict $key {*}$form] action2 id2
            assert {$id1 ne {} && $id2 ne {}}
            assert {$id1 ne $id2}
            assert_equal [expr {$requests + 2}] [gcmd_info $key requests]
            assert_equal 2 [gcmd_info $key pending]
            # Each one takes its own answer.
            assert_equal 1 [r glearn $key $id1 {*}$a1]
            assert_equal 1 [r glearn $key $id2 {*}$a2]
            assert_equal 0 [gcmd_info $key pending]
        }
    }

    gcmd_test {GPREDICT: the TEXT form, with and without FEATURES} {
        foreach form {
            {TEXT hello}
            {text hello}
            {TeXt hello}
            {TEXT hello FEATURES 1 mentioned 1}
            {TEXT hello FEATURES 2 mentioned 1 thread_depth 0.3}
            {text hello features 2 mentioned 1 thread_depth 0.3}
            {TEXT hello fEaTuReS 1 x -0.5}
            {TEXT hello FEATURES 1 x 0}
            {TEXT hello FEATURES 1 x -1}
            {TEXT hello FEATURES 1 x 1e-5}
            {TEXT hello FEATURES 1 x 2.5E-1}
            {TEXT hello FEATURES 1 x 1e300}
            {TEXT hello FEATURES 1 x -1e300}
            {TEXT hello FEATURES 3 {a b} 1 a.b 0.5 a:b:c -0.5}
            {TEXT TEXT}
            {TEXT FEATURES}
            {TEXT SCORE}
            {TEXT LABEL}
            {TEXT hello FEATURES 1 FEATURES 1}
            {TEXT hello FEATURES 2 TEXT 1 SCORE 0.5}
        } {
            set reply [r gpredict gp:cls {*}$form]
            assert_equal 2 [llength $reply] "GPREDICT key $form"
            lassign $reply action id
            assert_equal $action ABSTAIN "GPREDICT key $form"
            assert {$id ne {}}
            assert_equal [r glearn gp:cls $id LABEL a] 1 "GPREDICT key $form"
        }
    }

    gcmd_test {GPREDICT: bad arguments on a classifier are errors and change nothing} {
        foreach form {
            {SCORE 0.5}
            {score 0.5}
            {BOGUS hello}
            {hello TEXT}
            {TEXT hello extra}
            {TEXT hello extra more}
            {TEXT hello SCORE 0.5}
            {TEXT hello TEXT again}
            {TEXT hello LABEL a}
            {TEXT hello FEATURES}
            {TEXT hello FEATURES 1}
            {TEXT hello FEATURES 1 x}
            {TEXT hello FEATURES 2 x 1}
            {TEXT hello FEATURES 2 x 1 y}
            {TEXT hello FEATURES 1 x 1 y 2}
            {TEXT hello FEATURES 1 x 1 extra}
            {TEXT hello FEATURES 3 x 1 y 2}
            {TEXT hello FEATURES -1 x 1}
            {TEXT hello FEATURES abc x 1}
            {TEXT hello FEATURES 1.5 x 1}
            {TEXT hello FEATURES {} x 1}
            {TEXT hello FEATURES x 1}
            {TEXT hello FEATURES 4294967297 x 1}
            {TEXT hello FEATURES 9223372036854775809 x 1}
            {TEXT hello FEATURES 18446744073709551617 x 1}
            {TEXT hello FEATURES 1 x 1 FEATURES 1 y 2}
            {FEATURES 1 x 1 TEXT hello}
        } {
            gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls {*}$form]
        }
    }

    gcmd_test {GPREDICT: a feature value must be a finite number} {
        foreach value {abc {} nan NaN inf -inf +inf Infinity -Infinity 1e999 -1e999
                       1.5.2 1,5 1e e1 -- {1 } { 1} 1x x1 0x} {
            gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls TEXT hello FEATURES 1 x $value]
            gcmd_fails_unchanged gp:cls "ERR *" \
                [list gpredict gp:cls TEXT hello FEATURES 3 a 1 b $value c 1]
        }
        gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls TEXT hello FEATURES 1 x "1\x00"]
        gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls TEXT hello FEATURES 1 x "1\x002"]
    }

    gcmd_test {GPREDICT: a repeated feature name is an error} {
        # SPEC.md does not mention it; a name given twice has no single
        # value, so it cannot be right.
        foreach form {
            {TEXT hello FEATURES 2 x 1 x 2}
            {TEXT hello FEATURES 2 x 1 x 1}
            {TEXT hello FEATURES 3 x 1 y 2 x 3}
            {TEXT hello FEATURES 4 a 1 b 2 c 3 b 4}
        } {
            gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls {*}$form]
        }
        # Names are compared byte for byte: these are different names.
        lassign [r gpredict gp:cls TEXT hello FEATURES 3 x 1 X 2 xx 3] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gp:cls $id LABEL a]
    }

    gcmd_test {GPREDICT: 64 named features are accepted, 65 are refused} {
        # The limit is not in SPEC.md; 64 is the one gate_defs.h sets.
        set features {}
        for {set i 0} {$i < 64} {incr i} {lappend features f$i [expr {($i - 32) / 32.0}]}
        set reply [r gpredict gp:cls TEXT hello FEATURES 64 {*}$features]
        assert_equal ABSTAIN [lindex $reply 0]
        assert_equal 1 [r glearn gp:cls [lindex $reply 1] LABEL b]
        gcmd_fails_unchanged gp:cls "ERR *" \
            [list gpredict gp:cls TEXT hello FEATURES 65 {*}$features f64 0.5]
        # every count in between
        for {set n 1} {$n < 64} {incr n} {
            set reply [r gpredict gp:cls TEXT hello FEATURES $n {*}[lrange $features 0 [expr {2 * $n - 1}]]]
            assert_equal [lindex $reply 0] ABSTAIN "FEATURES $n"
            assert_equal [r glearn gp:cls [lindex $reply 1] LABEL a] 1 "FEATURES $n"
        }

        # GADD reads the same input.
        set learned [gcmd_info gp:cls learned]
        assert_equal [expr {$learned + 1}] \
            [r gadd gp:cls TEXT hello FEATURES 64 {*}$features LABEL a]
        gcmd_fails_unchanged gp:cls "ERR *" \
            [list gadd gp:cls TEXT hello FEATURES 65 {*}$features f64 0.5 LABEL a]
    }

    gcmd_test {GPREDICT: TEXT is bytes: empty, NUL bytes, invalid UTF-8, 100 KiB} {
        set texts [list \
            {} \
            "\x00" \
            "a\x00b\x00c" \
            "\x00\x00\x00 text after NULs" \
            [binary format H* fffec0af80] \
            [binary format H* eda080f490808061] \
            "line one\r\nline two\ttabbed" \
            [string repeat "word " 20480] \
            [string repeat x 102400] \
            [string repeat "\x00" 102400] \
            [string repeat [binary format H* ff] 102400]]
        assert_equal 102400 [string length [lindex $texts 7]]
        foreach text $texts {
            set shown "TEXT of [string length $text] bytes"
            set reply [r gpredict gp:cls TEXT $text]
            assert_equal [llength $reply] 2 $shown
            assert_equal [lindex $reply 0] ABSTAIN $shown
            assert {[lindex $reply 1] ne {}}
            assert_equal [r glearn gp:cls [lindex $reply 1] LABEL b] 1 $shown
            # with named features, and through GADD
            set reply [r gpredict gp:cls TEXT $text FEATURES 1 x 1]
            assert_equal [lindex $reply 0] ABSTAIN $shown
            assert_equal [r glearn gp:cls [lindex $reply 1] LABEL a] 1 $shown
            set learned [gcmd_info gp:cls learned]
            assert_equal [r gadd gp:cls TEXT $text LABEL a] [expr {$learned + 1}] $shown
        }
        assert_equal 0 [gcmd_info gp:cls pending]
    }

    gcmd_test {GPREDICT: the SCORE form takes a number from 0 to 1} {
        foreach score {0 1 0.5 0.0 1.0 0.93 0.999999 0.000001 1e-9 5e-1 5E-1 .5 1e0 -0 0e0 1e-320} {
            set reply [r gpredict gp:score SCORE $score]
            assert_equal [llength $reply] 2 "SCORE $score"
            lassign $reply action id
            assert_equal $action ABSTAIN "SCORE $score"
            assert {$id ne {}}
            assert_equal [r glearn gp:score $id CORRECT 1] 1 "SCORE $score"
        }
        # the word in any case
        foreach word {score Score sCoRe} {
            lassign [r gpredict gp:score $word 0.5] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn gp:score $id CORRECT 0]
        }
    }

    gcmd_test {GPREDICT: a score a rounding error outside 0..1 is accepted} {
        # Commit d77742b: 1.0000001 is what a cosine similarity computed in
        # floating point looks like, not a caller error. SPEC.md does not
        # mention the allowance.
        foreach score {1.0000001 1.00000001 1.0000000000000002 -0.0000001 -1e-12} {
            set reply [r gpredict gp:score SCORE $score]
            assert_equal [lindex $reply 0] ABSTAIN "SCORE $score"
            assert_equal [r glearn gp:score [lindex $reply 1] CORRECT 1] 1 "SCORE $score"
        }
    }

    gcmd_test {numbers written with many digits are numbers} {
        # SPEC.md gives ranges, not a way to write a number. These are the
        # exact decimal expansions of the doubles 0.1 and 0.0001 (what an
        # arbitrary precision decimal type prints: 57 and 68 bytes) and 0.5
        # printed with 70 decimals.
        set tenth 0.1000000000000000055511151231257827021181583404541015625
        set small 0.000100000000000000004792173602385929598312941379845142364501953125
        set padded 0.5[string repeat 0 69]
        foreach score [list $tenth $small $padded] {
            set shown "SCORE of [string length $score] bytes"
            set rc [catch {r gpredict gp:score SCORE $score} reply]
            assert_equal $rc 0 "$shown: $reply"
            assert_equal [lindex $reply 0] ABSTAIN $shown
            assert_equal [r glearn gp:score [lindex $reply 1] CORRECT 1] 1 $shown
        }
        foreach value [list $tenth $small $padded -$small] {
            set shown "feature value of [string length $value] bytes"
            set rc [catch {r gpredict gp:cls TEXT hello FEATURES 1 x $value} reply]
            assert_equal $rc 0 "$shown: $reply"
            assert_equal [r glearn gp:cls [lindex $reply 1] LABEL a] 1 $shown
        }
        foreach {alpha value} [list $tenth 0.1 $small 0.0001 $padded 0.5] {
            set shown "ALPHA of [string length $alpha] bytes"
            r del gc:digits
            set rc [catch {r gcreate gc:digits ALPHA $alpha DELTA $alpha AUDIT $alpha SCORE} reply]
            assert_equal $rc 0 "$shown: $reply"
            assert {[gcmd_info gc:digits alpha] == $value}
            assert {[gcmd_info gc:digits delta] == $value}
            assert {[gcmd_info gc:digits audit] == $value}
        }
        r del gc:digits
    }

    gcmd_test {GPREDICT: bad arguments on a score gate are errors and change nothing} {
        foreach form {
            {SCORE -0.1}
            {SCORE 1.1}
            {SCORE 1.001}
            {SCORE -0.001}
            {SCORE 2}
            {SCORE -1}
            {SCORE 100}
            {SCORE 1e308}
            {SCORE abc}
            {SCORE {}}
            {SCORE nan}
            {SCORE NaN}
            {SCORE inf}
            {SCORE -inf}
            {SCORE Infinity}
            {SCORE 1e999}
            {SCORE 0.5x}
            {SCORE x0.5}
            {SCORE { 0.5}}
            {SCORE {0.5 }}
            {SCORE 0,5}
            {SCORE 0.5.1}
            {SCORE 0.5 extra}
            {SCORE 0.5 SCORE 0.6}
            {SCORE 0.5 0.6}
            {TEXT hello}
            {text hello}
            {TEXT hello FEATURES 1 x 1}
            {BOGUS 0.5}
            {0.5 SCORE}
        } {
            gcmd_fails_unchanged gp:score "ERR *" [list gpredict gp:score {*}$form]
        }
        gcmd_fails_unchanged gp:score "ERR *" [list gpredict gp:score SCORE "0.5\x00"]
        gcmd_fails_unchanged gp:score "ERR *" [list gpredict gp:score SCORE "0.5\x009"]
        gcmd_fails_unchanged gp:score "ERR *" [list gpredict gp:score "SCORE\x00" 0.5]
    }

    gcmd_test {GPREDICT: a full gate abstains without an id} {
        # SPEC 3 "Full": at most 4,096 requests wait for feedback; while full
        # the gate answers ABSTAIN to everything.
        r gcreate gp:full ALPHA 0.2 SCORE
        set replies [gcmd_pipeline_predict gp:full 4096 0.5]
        set ids {}
        foreach reply $replies {
            assert_equal 2 [llength $reply]
            assert_equal ABSTAIN [lindex $reply 0]
            assert {[lindex $reply 1] ne {}}
            dict set ids [lindex $reply 1] 1
        }
        assert_equal 4096 [dict size $ids]
        assert_equal 4096 [gcmd_info gp:full pending]

        foreach score {0.5 0 1 0.99} {
            assert_equal {ABSTAIN {}} [r gpredict gp:full SCORE $score]
        }
        assert_equal 4096 [gcmd_info gp:full pending]
        assert_equal 4100 [gcmd_info gp:full requests]
        assert_equal 4100 [gcmd_info gp:full abstained]

        # A full gate still checks its arguments.
        gcmd_fails_unchanged gp:full "ERR *" {gpredict gp:full SCORE 7}
        gcmd_fails_unchanged gp:full "ERR *" {gpredict gp:full TEXT hello}

        # One answer makes room for one request.
        assert_equal 1 [r glearn gp:full [lindex [lindex $replies 0] 1] CORRECT 1]
        assert_equal 4095 [gcmd_info gp:full pending]
        lassign [r gpredict gp:full SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        assert {![dict exists $ids $id]}
        assert_equal {ABSTAIN {}} [r gpredict gp:full SCORE 0.5]
        assert_equal 4096 [gcmd_info gp:full pending]
    }

    gcmd_test {GPREDICT: a classifier never holds more than 4,096 waiting requests} {
        r gcreate gp:full:cls ALPHA 0.2 CLASSES 2 a b
        set rd [redis_deferring_client]
        for {set i 0} {$i < 4200} {incr i} {$rd gpredict gp:full:cls TEXT "request $i"}
        set with_id 0
        set without 0
        for {set i 0} {$i < 4200} {incr i} {
            set reply [$rd read]
            assert_equal 2 [llength $reply]
            assert_equal ABSTAIN [lindex $reply 0]
            if {[lindex $reply 1] eq {}} {incr without} else {incr with_id}
        }
        $rd close
        # 4,200 requests and no answer: some got no id
        assert {$without > 0}
        assert {$with_id <= 4096}
        set info [r ginfo gp:full:cls]
        assert_equal $with_id [dict get $info pending]
        assert_equal 4200 [dict get $info requests]
        assert_equal 4200 [dict get $info abstained]
        r del gp:full:cls
    }

    # -------------------------------------------------------------- GLEARN

    gcmd_test {GLEARN: 1 when the feedback is used, 0 when it is sent again} {
        r gcreate gl:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:score ALPHA 0.1 SCORE
        lassign [r gpredict gl:cls TEXT hello] action id
        assert_equal 1 [r glearn gl:cls $id LABEL a]
        assert_equal 0 [r glearn gl:cls $id LABEL a]
        # A retry with another answer is not used either.
        assert_equal 0 [r glearn gl:cls $id LABEL b]
        assert_equal 1 [gcmd_info gl:cls learned]
        assert_equal 0 [gcmd_info gl:cls pending]

        lassign [r gpredict gl:score SCORE 0.7] action id
        assert_equal 1 [r glearn gl:score $id CORRECT 1]
        assert_equal 0 [r glearn gl:score $id CORRECT 1]
        assert_equal 0 [r glearn gl:score $id CORRECT 0]
        assert_equal 0 [gcmd_info gl:score pending]
        assert_equal 1 [gcmd_info gl:score calibration-samples]
    }

    gcmd_test {GLEARN: LABEL and CORRECT in any case} {
        foreach word {label Label lAbEl} {
            lassign [r gpredict gl:cls TEXT hello] action id
            assert_equal 1 [r glearn gl:cls $id $word b]
        }
        foreach word {correct Correct cOrReCt} {
            lassign [r gpredict gl:score SCORE 0.7] action id
            assert_equal 1 [r glearn gl:score $id $word 0]
        }
    }

    gcmd_test {GLEARN: an id the gate never issued gets 0, and costs nothing} {
        r gcreate gl:ids ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:ids:score ALPHA 0.1 SCORE
        foreach {key answer} {gl:ids {LABEL a} gl:ids:score {CORRECT 1}} {
            # The only request this gate has seen.
            if {$key eq "gl:ids"} {
                lassign [r gpredict $key TEXT hello] action id
            } else {
                lassign [r gpredict $key SCORE 0.5] action id
            }
            set info [r ginfo $key]
            set bogus [list \
                {} x garbage 0 1 -1 1-1 - -- "*" \
                "$id " " $id" "${id}x" "x$id" "${id}0" "${id}00" "$id\x00" "\x00$id" "$id\n" \
                "$id-1" "$id$id" [string range $id 0 end-1] [string range $id 1 end] \
                [string repeat 0 16]-1 0000000000000000-0 ffffffffffffffff-18446744073709551615 \
                ffffffffffffffff-18446744073709551616 0123456789abcdef-1 \
                [string repeat 9 100] [string repeat x 100000] \
                [binary format H* fffe00ff]]
            foreach bad $bogus {
                if {$bad eq $id} continue
                assert_equal [r glearn $key $bad {*}$answer] 0 "id of [string length $bad] bytes"
            }
            # Nothing was used up: the gate is as it was and the real id works.
            assert_equal $info [r ginfo $key]
            assert_equal 1 [r glearn $key $id {*}$answer]
            assert_equal 0 [r glearn $key $id {*}$answer]
        }
    }

    gcmd_test {GLEARN: the id of another gate gets 0} {
        r gcreate gl:x1 ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:x2 ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:x3 ALPHA 0.1 SCORE
        r gcreate gl:x4 ALPHA 0.1 SCORE
        lassign [r gpredict gl:x1 TEXT hello] action id1
        lassign [r gpredict gl:x2 TEXT hello] action id2
        lassign [r gpredict gl:x3 SCORE 0.5] action id3
        lassign [r gpredict gl:x4 SCORE 0.5] action id4
        assert_equal 4 [llength [lsort -unique [list $id1 $id2 $id3 $id4]]]

        foreach id [list $id2 $id3 $id4] {assert_equal 0 [r glearn gl:x1 $id LABEL a]}
        foreach id [list $id1 $id3 $id4] {assert_equal 0 [r glearn gl:x2 $id LABEL a]}
        foreach id [list $id1 $id2 $id4] {assert_equal 0 [r glearn gl:x3 $id CORRECT 1]}
        foreach id [list $id1 $id2 $id3] {assert_equal 0 [r glearn gl:x4 $id CORRECT 1]}
        foreach key {gl:x1 gl:x2 gl:x3 gl:x4} {assert_equal 1 [gcmd_info $key pending]}

        # Every request can still be answered where it was asked.
        assert_equal 1 [r glearn gl:x1 $id1 LABEL a]
        assert_equal 1 [r glearn gl:x2 $id2 LABEL b]
        assert_equal 1 [r glearn gl:x3 $id3 CORRECT 1]
        assert_equal 1 [r glearn gl:x4 $id4 CORRECT 0]
    }

    gcmd_test {GLEARN: an unknown label is an error and the request can still be answered} {
        lassign [r gpredict gl:cls TEXT hello] action id
        foreach label [list nosuch A B {} {a } { a} ab "a\x00" "a\x00b" 0 1 [string repeat a 65] \
                           [string repeat a 100000]] {
            gcmd_fails_unchanged gl:cls "ERR *" [list glearn gl:cls $id LABEL $label]
        }
        assert_equal 1 [gcmd_info gl:cls pending]
        assert_equal 1 [r glearn gl:cls $id LABEL a]
        assert_equal 0 [gcmd_info gl:cls pending]
    }

    gcmd_test {GLEARN: CORRECT on a classifier and LABEL on a score gate are errors} {
        lassign [r gpredict gl:cls TEXT hello] action id
        foreach form {{CORRECT 1} {CORRECT 0} {correct 1} {CORRECT a} {BOGUS a} {a LABEL} {{} a}
                      {SCORE 0.5} {TEXT a}} {
            gcmd_fails_unchanged gl:cls "ERR *" [list glearn gl:cls $id {*}$form]
        }
        assert_equal 1 [r glearn gl:cls $id LABEL a]

        lassign [r gpredict gl:score SCORE 0.7] action id
        foreach form {{LABEL a} {LABEL 1} {LABEL 0} {label 1} {BOGUS 1} {1 CORRECT} {{} 1}
                      {SCORE 1}} {
            gcmd_fails_unchanged gl:score "ERR *" [list glearn gl:score $id {*}$form]
        }
        assert_equal 1 [r glearn gl:score $id CORRECT 1]
    }

    gcmd_test {GLEARN: CORRECT takes 0 or 1 and nothing else} {
        lassign [r gpredict gl:score SCORE 0.7] action id
        foreach value [list 2 -1 10 11 yes no true false {} { 1} {1 } 0.5 "1\x00" "0\x000" a] {
            gcmd_fails_unchanged gl:score "ERR *" [list glearn gl:score $id CORRECT $value]
        }
        assert_equal 1 [gcmd_info gl:score pending]
        assert_equal 1 [r glearn gl:score $id CORRECT 0]
        assert_equal 0 [r glearn gl:score $id CORRECT 0]
    }

    # ---------------------------------------------------------------- GADD

    gcmd_test {GADD: the reply counts the examples the gate has learned from} {
        r gcreate ga:cls ALPHA 0.1 CLASSES 2 a b
        assert_equal 1 [r gadd ga:cls TEXT "first example" LABEL a]
        assert_equal 2 [r gadd ga:cls TEXT "second example" LABEL b]
        assert_equal 3 [r gadd ga:cls TEXT "third example" FEATURES 1 x 0.5 LABEL a]
        assert_equal 4 [r gadd ga:cls text "fourth example" features 2 x 0.5 y -1 label b]
        assert_equal 4 [gcmd_info ga:cls learned]
        # The same example again is one more example.
        assert_equal 5 [r gadd ga:cls TEXT "first example" LABEL a]
        # An answer given with GLEARN is an example too.
        lassign [r gpredict ga:cls TEXT "sixth example"] action id
        assert_equal 1 [r glearn ga:cls $id LABEL b]
        assert_equal 6 [gcmd_info ga:cls learned]
        assert_equal 7 [r gadd ga:cls TEXT "seventh example" LABEL a]
        assert_equal 7 [gcmd_info ga:cls learned]
    }

    gcmd_test {GADD: an example is never proof} {
        gcmd_create_fast ga:proof ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        gcmd_teach ga:proof 39
        set info [r ginfo ga:proof]
        assert_equal COLD [dict get $info state]
        assert_equal 0 [dict get $info candidate]
        gcmd_teach ga:proof 1
        # The first snapshot is taken; teaching goes on, proving does not start.
        gcmd_teach ga:proof 100
        set info [r ginfo ga:proof]
        assert_equal 140 [dict get $info learned]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 1 [dict get $info candidate]
        assert_equal 0 [dict get $info generation]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal {{} {}} [dict get $info thresholds]
        assert_equal 0 [dict get $info requests]
        assert_equal 0 [dict get $info pending]
        r del ga:proof
    }

    gcmd_test {GADD: refused on a score gate} {
        r gcreate ga:score ALPHA 0.1 SCORE
        foreach form {{TEXT hello LABEL a} {TEXT hello LABEL 1} {TEXT hello CORRECT 1}
                      {TEXT hello FEATURES 1 x 1 LABEL a} {SCORE 0.5 CORRECT 1} {SCORE 0.5 LABEL a}} {
            gcmd_fails_unchanged ga:score "ERR *" [list gadd ga:score {*}$form]
        }
        assert_equal 0 [gcmd_info ga:score learned]
    }

    gcmd_test {GADD: a bad label or bad syntax is an error and teaches nothing} {
        foreach form {
            {TEXT hello LABEL nosuch}
            {TEXT hello LABEL {}}
            {TEXT hello LABEL A}
            {TEXT hello LABEL {a }}
            {TEXT hello CORRECT 1}
            {TEXT hello BOGUS a}
            {TEXT hello a LABEL}
            {TEXT hello LABEL a extra}
            {TEXT hello LABEL a LABEL b}
            {TEXT hello LABEL a FEATURES 1 x 1}
            {TEXT hello FEATURES 1 x 1}
            {TEXT hello FEATURES 1 x 1 extra a}
            {TEXT hello FEATURES 2 x 1 LABEL a}
            {TEXT hello FEATURES 1 x abc LABEL a}
            {TEXT hello FEATURES 1 x nan LABEL a}
            {TEXT hello FEATURES 1 x inf LABEL a}
            {TEXT hello FEATURES 2 x 1 x 2 LABEL a}
            {TEXT hello FEATURES -1 LABEL a}
            {TEXT hello FEATURES LABEL a}
            {LABEL a TEXT hello}
            {SCORE 0.5 LABEL a}
            {BOGUS hello LABEL a}
            {hello TEXT LABEL a}
        } {
            gcmd_fails_unchanged ga:cls "ERR *" [list gadd ga:cls {*}$form]
        }
        gcmd_fails_unchanged ga:cls "ERR *" [list gadd ga:cls TEXT hello LABEL "a\x00"]
        gcmd_fails_unchanged ga:cls "ERR *" [list gadd ga:cls TEXT hello "LABEL\x00" a]
        assert_equal 8 [r gadd ga:cls TEXT hello LABEL a]
    }

    gcmd_test {GADD: a label that is always deferred can still be taught} {
        r gcreate ga:defer ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 b
        assert_equal 1 [r gadd ga:defer TEXT hello LABEL b]
        lassign [r gpredict ga:defer TEXT hello] action id
        assert_equal 1 [r glearn ga:defer $id LABEL b]
        assert_equal 2 [gcmd_info ga:defer learned]
        r del ga:defer
    }

    # --------------------------------------------------------------- GINFO

    gcmd_test {GINFO: a new classifier gate} {
        r gcreate gi:cls ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 b CLASSALPHA 1 c 0.05 \
            AUDIT 0.2 DELTA 0.02
        set info [r ginfo gi:cls]
        assert_equal 23 [dict size $info]
        assert_equal classifier [dict get $info kind]
        assert_equal COLD [dict get $info state]
        assert {[dict get $info alpha] == 0.1}
        assert {[dict get $info delta] == 0.02}
        assert {[dict get $info audit] == 0.2}
        assert_equal {a b c} [dict get $info classes]
        # one error rate per label; 0 means always defer
        lassign [dict get $info alphas] a b c
        assert_equal 3 [llength [dict get $info alphas]]
        assert {$a == 0.1}
        assert {$b == 0}
        assert {$c == 0.05}
        # null where the gate does not answer yet
        assert_equal {{} {} {}} [dict get $info thresholds]
        foreach field {generation candidate learned requests accepted audited abstained
                       audit-samples audit-errors calibration-samples pending missing
                       revocations} {
            assert_equal [dict get $info $field] 0 "GINFO $field"
        }
        assert {[dict get $info coverage] == 0}
        assert {[dict get $info memory] > 0}
    }

    gcmd_test {GINFO: a new score gate} {
        r gcreate gi:score ALPHA 0.3 SCORE AUDIT 0.5 DELTA 0.25
        set info [r ginfo gi:score]
        assert_equal 23 [dict size $info]
        assert_equal score [dict get $info kind]
        assert_equal CALIBRATING [dict get $info state]
        assert {[dict get $info alpha] == 0.3}
        assert {[dict get $info delta] == 0.25}
        assert {[dict get $info audit] == 0.5}
        # no labels; one error rate and one threshold
        assert_equal {} [dict get $info classes]
        assert_equal 1 [llength [dict get $info alphas]]
        assert {[lindex [dict get $info alphas] 0] == 0.3}
        assert_equal {{}} [dict get $info thresholds]
        assert_equal 0 [dict get $info generation]
        assert_equal 1 [dict get $info candidate]
        foreach field {learned requests accepted audited abstained audit-samples audit-errors
                       calibration-samples pending missing revocations} {
            assert_equal [dict get $info $field] 0 "GINFO $field"
        }
        assert {[dict get $info coverage] == 0}
        assert {[dict get $info memory] > 0}
    }

    gcmd_test {GINFO: counters follow the commands} {
        # classifier: three requests, two answers, one example
        lassign [r gpredict gi:cls TEXT one] action id1
        lassign [r gpredict gi:cls TEXT two] action id2
        lassign [r gpredict gi:cls TEXT three] action id3
        set info [r ginfo gi:cls]
        assert_equal 3 [dict get $info requests]
        assert_equal 3 [dict get $info abstained]
        assert_equal 0 [dict get $info accepted]
        assert_equal 0 [dict get $info audited]
        assert_equal 3 [dict get $info pending]
        assert_equal 0 [dict get $info learned]
        r glearn gi:cls $id1 LABEL a
        r glearn gi:cls $id2 LABEL c
        r gadd gi:cls TEXT four LABEL b
        set info [r ginfo gi:cls]
        assert_equal 3 [dict get $info requests]
        assert_equal 1 [dict get $info pending]
        assert_equal 3 [dict get $info learned]
        assert_equal COLD [dict get $info state]
        r glearn gi:cls $id3 LABEL a

        # score gate: every answer is proof for the snapshot being tested
        lassign [r gpredict gi:score SCORE 0.9] action id1
        lassign [r gpredict gi:score SCORE 0.8] action id2
        set info [r ginfo gi:score]
        assert_equal 2 [dict get $info requests]
        assert_equal 2 [dict get $info abstained]
        assert_equal 2 [dict get $info pending]
        assert_equal 0 [dict get $info calibration-samples]
        r glearn gi:score $id1 CORRECT 1
        r glearn gi:score $id2 CORRECT 0
        set info [r ginfo gi:score]
        assert_equal 0 [dict get $info pending]
        assert_equal 2 [dict get $info calibration-samples]
        # a score gate has no model to teach
        assert_equal 0 [dict get $info learned]
        assert_equal 0 [dict get $info missing]
    }

    gcmd_test {GINFO: pending counts the requests still waiting for GLEARN, in any order of answers} {
        # SPEC: "pending | requests waiting for GLEARN". A request whose
        # GLEARN replied 1 is not waiting for it any more, also when an older
        # request is still unanswered.
        foreach {key form answer} {gi:order:cls {TEXT hello} {LABEL a}
                                   gi:order:score {SCORE 0.5} {CORRECT 1}} {
            if {$key eq "gi:order:cls"} {
                r gcreate $key ALPHA 0.1 CLASSES 2 a b
            } else {
                r gcreate $key ALPHA 0.1 SCORE
            }
            set ids {}
            for {set i 0} {$i < 5} {incr i} {lappend ids [lindex [r gpredict $key {*}$form] 1]}
            assert_equal [gcmd_info $key pending] 5 "$key"
            # newest first
            set left 5
            foreach id [lreverse $ids] {
                assert_equal [r glearn $key $id {*}$answer] 1 "$key"
                incr left -1
                assert_equal [gcmd_info $key pending] $left \
                    "$key: pending after [expr {5 - $left}] of 5 requests were answered, newest first"
            }
            r del $key
        }
    }

    gcmd_test {GINFO: reading changes nothing} {
        foreach key {gi:cls gi:score} {
            set digest [debug_digest_value $key]
            set first [r ginfo $key]
            for {set i 0} {$i < 5} {incr i} {assert_equal $first [r ginfo $key]}
            assert_equal $digest [debug_digest_value $key]
        }
    }

    # ---------------------------------------------- GPAUSE, GRESUME, GRESET

    gcmd_test {GPAUSE and GRESUME: OK, and again OK} {
        r gcreate gadm:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gadm:score ALPHA 0.1 SCORE
        foreach {key state} {gadm:cls COLD gadm:score CALIBRATING} {
            assert_equal $state [gcmd_info $key state]
            # resuming a gate that is not paused changes nothing
            set before [gcmd_state $key]
            assert_equal OK [r gresume $key]
            assert_equal $before [gcmd_state $key]

            assert_equal OK [r gpause $key]
            assert_equal PAUSED [gcmd_info $key state]
            set paused [gcmd_state $key]
            assert_equal OK [r gpause $key]
            assert_equal OK [r gpause $key]
            assert_equal $paused [gcmd_state $key]

            assert_equal OK [r gresume $key]
            assert_equal $state [gcmd_info $key state]
            assert_equal OK [r gresume $key]
            assert_equal $state [gcmd_info $key state]
            assert_equal $before [gcmd_state $key]
        }
    }

    gcmd_test {GPAUSE: everything but answering keeps running} {
        r gpause gadm:cls
        r gpause gadm:score
        # requests are asked and answered, examples are learned
        lassign [r gpredict gadm:cls TEXT hello] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gadm:cls $id LABEL a]
        assert_equal 2 [r gadd gadm:cls TEXT hello LABEL b]
        lassign [r gpredict gadm:score SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gadm:score $id CORRECT 1]
        assert_equal PAUSED [gcmd_info gadm:cls state]
        assert_equal 2 [gcmd_info gadm:cls learned]
        assert_equal 1 [gcmd_info gadm:cls requests]
        assert_equal PAUSED [gcmd_info gadm:score state]
        assert_equal 1 [gcmd_info gadm:score calibration-samples]
        r gresume gadm:cls
        r gresume gadm:score
    }

    gcmd_test {GRESET: forgets what was learned, keeps the configuration} {
        r gcreate gadm:reset ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 b CLASSALPHA 1 c 0.05 \
            AUDIT 0.2 DELTA 0.02
        set fresh [r ginfo gadm:reset]
        r gadd gadm:reset TEXT hello LABEL a
        r gadd gadm:reset TEXT world LABEL c
        lassign [r gpredict gadm:reset TEXT hello] action answered
        r glearn gadm:reset $answered LABEL a
        lassign [r gpredict gadm:reset TEXT again] action waiting
        assert_equal 3 [gcmd_info gadm:reset learned]
        assert_equal 1 [gcmd_info gadm:reset pending]

        assert_equal OK [r greset gadm:reset]
        # A gate that never took a snapshot is, after a reset, what it was
        # when it was created (its size included).
        assert_equal $fresh [r ginfo gadm:reset]
        # Ids from before the reset are refused.
        assert_equal 0 [r glearn gadm:reset $waiting LABEL a]
        assert_equal 0 [r glearn gadm:reset $answered LABEL a]
        assert_equal 0 [gcmd_info gadm:reset learned]

        # again, and on an empty gate
        assert_equal OK [r greset gadm:reset]
        assert_equal OK [r greset gadm:reset]
        assert_equal $fresh [r ginfo gadm:reset]
        # It works like a new gate.
        lassign [r gpredict gadm:reset TEXT hello] action id
        assert {$id ne $waiting && $id ne $answered}
        assert_equal 1 [r glearn gadm:reset $id LABEL c]
        assert_equal 2 [r gadd gadm:reset TEXT hello LABEL b]
    }

    gcmd_test {GRESET: a score gate starts a new test under a new snapshot number} {
        # SPEC 3: snapshots are numbered 1, 2, 3, ... and a number is never
        # reused; GRESET never hands budget back.
        r gcreate gadm:rs ALPHA 0.1 SCORE
        assert_equal 1 [gcmd_info gadm:rs candidate]
        lassign [r gpredict gadm:rs SCORE 0.5] action id
        r glearn gadm:rs $id CORRECT 1
        lassign [r gpredict gadm:rs SCORE 0.5] action id
        assert_equal OK [r greset gadm:rs]
        set info [r ginfo gadm:rs]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 2 [dict get $info candidate]
        assert_equal 0 [dict get $info generation]
        assert_equal 0 [dict get $info requests]
        assert_equal 0 [dict get $info pending]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal 0 [r glearn gadm:rs $id CORRECT 1]
        assert_equal OK [r greset gadm:rs]
        assert_equal 3 [gcmd_info gadm:rs candidate]
    }

    gcmd_test {GRESET: a paused gate stays paused} {
        # SPEC.md does not say whether GRESET resumes. A reset that made a
        # gate somebody paused answer again would be a surprise.
        r gcreate gadm:pr ALPHA 0.1 SCORE
        r gpause gadm:pr
        assert_equal OK [r greset gadm:pr]
        assert_equal PAUSED [gcmd_info gadm:pr state]
        assert_equal OK [r gresume gadm:pr]
        assert_equal CALIBRATING [gcmd_info gadm:pr state]
    }

    # ----------------------------------------------------- missing key, type

    gcmd_test {every command but GCREATE: ERR no such key on a missing key} {
        set size [r dbsize]
        foreach form $::gcmd_forms {
            set cmd [gcmd_form $form nokey:missing]
            assert_error "ERR no such key" {r {*}$cmd} "command: $cmd"
        }
        # ... and nothing appears
        assert_equal 0 [r exists nokey:missing]
        assert_equal $size [r dbsize]
        # A key that was deleted is a missing key.
        r gcreate nokey:gone ALPHA 0.1 CLASSES 2 a b
        r del nokey:gone
        foreach form $::gcmd_forms {
            set cmd [gcmd_form $form nokey:gone]
            assert_error "ERR no such key" {r {*}$cmd} "command: $cmd"
        }
        assert_equal $size [r dbsize]
    }

    gcmd_test {every command on a key of another type: WRONGTYPE, BUSYKEY for GCREATE} {
        r set wt:string value
        r rpush wt:list a b
        r hset wt:hash f v
        r sadd wt:set m
        r zadd wt:zset 1 m
        r xadd wt:stream 1-1 f v
        foreach type {string list hash set zset stream} {
            set key wt:$type
            set digest [debug_digest_value $key]
            foreach form $::gcmd_forms {
                set cmd [gcmd_form $form $key]
                assert_error "WRONGTYPE*" {r {*}$cmd} "on a $type: $cmd"
            }
            assert_error "BUSYKEY*" {r gcreate $key ALPHA 0.1 SCORE}
            assert_error "BUSYKEY*" {r gcreate $key ALPHA 0.1 CLASSES 2 a b}
            assert_equal [r type $key] $type
            assert_equal [debug_digest_value $key] $digest "value of the $type key"
        }
        assert_equal value [r get wt:string]
    }

    gcmd_test {arity: too few or too many arguments} {
        r gcreate ar:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate ar:score ALPHA 0.1 SCORE
        set before [list [gcmd_state ar:cls] [gcmd_state ar:score]]
        foreach key {ar:cls ar:score ar:missing} {
            foreach form {
                {gcreate KEY}
                {gcreate KEY ALPHA}
                {gcreate KEY ALPHA 0.1}
                {gcreate KEY SCORE}
                {gcreate KEY CLASSES 2}
                {gpredict KEY}
                {gpredict KEY TEXT}
                {gpredict KEY SCORE}
                {glearn KEY}
                {glearn KEY 0123456789abcdef-1}
                {glearn KEY 0123456789abcdef-1 LABEL}
                {glearn KEY 0123456789abcdef-1 CORRECT}
                {glearn KEY 0123456789abcdef-1 LABEL a extra}
                {glearn KEY 0123456789abcdef-1 CORRECT 1 extra}
                {gadd KEY}
                {gadd KEY TEXT}
                {gadd KEY TEXT hello}
                {gadd KEY TEXT hello LABEL}
                {ginfo KEY extra}
                {gpause KEY extra}
                {gresume KEY extra}
                {greset KEY extra}
                {ginfo KEY KEY}
            } {
                set cmd [gcmd_form $form $key]
                assert_error "ERR wrong number of arguments*" {r {*}$cmd} "command: $cmd"
            }
        }
        foreach name {gcreate gpredict glearn gadd ginfo gpause gresume greset} {
            assert_error "ERR wrong number of arguments*" {r $name}
        }
        assert_equal $before [list [gcmd_state ar:cls] [gcmd_state ar:score]]
        assert_equal 0 [r exists ar:missing]
    }

    # ---------------------------------------- gates that answer by themselves

    gcmd_test {setup: gates that answer by themselves} {
        # a score gate answering from a threshold between 0.1 and 0.9
        r gcreate p:score ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score p:score
        set info [r ginfo p:score]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info generation]
        set threshold [lindex [dict get $info thresholds] 0]
        assert {$threshold > 0.1 && $threshold <= 0.9}

        # the same with AUDIT 1: everything it could answer is a spot check
        r gcreate p:audit ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score p:audit
        assert_equal ACTIVE [gcmd_info p:audit state]

        # a classifier answering both of its labels
        gcmd_create_fast p:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        gcmd_prove_classifier p:cls
        set info [r ginfo p:cls]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info generation]
        foreach threshold [dict get $info thresholds] {
            # two labels: thresholds lie between 0.5 and 0.999
            assert {$threshold >= 0.5 && $threshold <= 0.999}
        }
    }

    gcmd_test {GPREDICT: a score gate that answers replies ACCEPT and a null} {
        set seen {ACCEPT 0 AUDIT 0}
        for {set i 0} {$i < 200} {incr i} {
            set reply [r gpredict p:score SCORE 0.9]
            assert_equal 2 [llength $reply]
            lassign $reply action arg
            if {$action eq "ACCEPT"} {
                # use your cheap result: there is no label and no id
                assert_equal {} $arg
            } elseif {$action eq "AUDIT"} {
                assert {$arg ne {}}
                assert_equal 1 [r glearn p:score $arg CORRECT 1]
            } else {
                fail "request $i: a score above the proven threshold got '$reply'"
            }
            dict incr seen $action
        }
        # AUDIT is 0.05: most requests are answered
        assert {[dict get $seen ACCEPT] > [dict get $seen AUDIT]}
        assert_equal ACTIVE [gcmd_info p:score state]
    }

    gcmd_test {GPREDICT: below its threshold a score gate abstains, mostly without an id} {
        set with_id 0
        set without 0
        for {set i 0} {$i < 200} {incr i} {
            set reply [r gpredict p:score SCORE 0.05]
            assert_equal 2 [llength $reply]
            lassign $reply action arg
            assert_equal ABSTAIN $action
            if {$arg eq {}} {
                incr without
            } else {
                # a spot check of what the gate would not answer
                incr with_id
                assert_equal 1 [r glearn p:score $arg CORRECT 0]
            }
        }
        assert {$without > $with_id}
        assert_equal 0 [gcmd_info p:score pending]
        assert_equal ACTIVE [gcmd_info p:score state]
    }

    gcmd_test {GPREDICT: with AUDIT 1 nothing is accepted: AUDIT and an id} {
        for {set i 0} {$i < 60} {incr i} {
            set reply [r gpredict p:audit SCORE 0.9]
            assert_equal 2 [llength $reply]
            lassign $reply action id
            assert_equal AUDIT $action
            assert {$id ne {}}
            assert_equal 1 [r glearn p:audit $id CORRECT 1]
            assert_equal 0 [r glearn p:audit $id CORRECT 1]
        }
        set info [r ginfo p:audit]
        assert_equal 0 [dict get $info accepted]
        assert_equal 60 [dict get $info audited]
        assert_equal 60 [dict get $info audit-samples]
        assert_equal 0 [dict get $info audit-errors]
        assert_equal ACTIVE [dict get $info state]
    }

    gcmd_test {GPREDICT: a classifier that answers replies ACCEPT and the label} {
        set seen {ACCEPT 0 AUDIT 0}
        for {set i 0} {$i < 200} {incr i} {
            lassign [gcmd_example $i] text label
            set reply [r gpredict p:cls TEXT $text]
            assert_equal 2 [llength $reply]
            lassign $reply action arg
            if {$action eq "ACCEPT"} {
                assert_equal $label $arg
            } elseif {$action eq "AUDIT"} {
                assert {$arg ne {} && $arg ni {spam ham}}
                assert_equal 1 [r glearn p:cls $arg LABEL $label]
            } else {
                fail "request $i: a proven classifier replied '$reply'"
            }
            dict incr seen $action
        }
        assert {[dict get $seen ACCEPT] > [dict get $seen AUDIT]}
        assert_equal 0 [gcmd_info p:cls pending]
    }

    gcmd_test {GPREDICT: an ACCEPT changes nothing but the request counters} {
        # SPEC 3 "Redis": an ACCEPT writes nothing.
        foreach {key form answer} {p:score {SCORE 0.9} {CORRECT 1}
                                   p:cls {TEXT {buy cheap pills now}} {LABEL spam}} {
            set accepts 0
            for {set i 0} {$i < 100 && $accepts < 5} {incr i} {
                set digest [debug_digest_value $key]
                set before [r ginfo $key]
                lassign [r gpredict $key {*}$form] action arg
                if {$action ne "ACCEPT"} {
                    assert_equal 1 [r glearn $key $arg {*}$answer]
                    continue
                }
                incr accepts
                assert_equal $digest [debug_digest_value $key]
                set after [r ginfo $key]
                foreach field [dict keys $before] {
                    set old [dict get $before $field]
                    set new [dict get $after $field]
                    if {$field in {requests accepted}} {
                        assert_equal $new [expr {$old + 1}] "$key: $field after an ACCEPT"
                    } elseif {$field ne "coverage"} {
                        assert_equal $new $old "$key: $field after an ACCEPT"
                    }
                }
            }
            assert_equal 5 $accepts
        }
    }

    gcmd_test {GCREATE: an ALWAYSDEFER label is never answered} {
        gcmd_create_fast p:defer ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham ALWAYSDEFER 1 ham
        gcmd_teach p:defer 40
        for {set i 0} {$i < 600} {incr i} {
            if {[lindex [gcmd_info p:defer thresholds] 0] ne {}} break
            lassign [gcmd_example $i] text label
            lassign [r gpredict p:defer TEXT $text] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn p:defer $id LABEL $label]
        }
        set info [r ginfo p:defer]
        assert_equal ACTIVE [dict get $info state]
        assert {[lindex [dict get $info thresholds] 0] ne {}}
        assert_equal {} [lindex [dict get $info thresholds] 1]

        set spam {ACCEPT 0 AUDIT 0}
        for {set i 0} {$i < 200} {incr i} {
            lassign [gcmd_example $i] text label
            lassign [r gpredict p:defer TEXT $text] action arg
            if {$label eq "ham"} {
                # always ask the teacher: no answer, and not a spot check of one
                assert_equal ABSTAIN $action
                assert {$arg ne {} && $arg ne "ham"}
                assert_equal 1 [r glearn p:defer $arg LABEL ham]
            } elseif {$action eq "ACCEPT"} {
                assert_equal spam $arg
                dict incr spam ACCEPT
            } else {
                assert_equal AUDIT $action
                assert_equal 1 [r glearn p:defer $arg LABEL spam]
                dict incr spam AUDIT
            }
        }
        assert {[dict get $spam ACCEPT] > [dict get $spam AUDIT]}
        # however much proof there is, the label gets no threshold
        set info [r ginfo p:defer]
        assert_equal {} [lindex [dict get $info thresholds] 1]
        assert {[lindex [dict get $info alphas] 1] == 0}
        assert_equal ACTIVE [dict get $info state]
        r del p:defer
    }

    gcmd_test {GCREATE: a CLASSALPHA label needs more proof than the others} {
        gcmd_create_fast p:strict ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham CLASSALPHA 1 spam 0.02
        gcmd_teach p:strict 40
        # The teacher is always right about both labels, and they are asked
        # equally often: the label with the tenfold stricter error rate is
        # proven later, and much later.
        set proven {}
        for {set i 0} {$i < 3000} {incr i} {
            lassign [gcmd_info p:strict thresholds] spam ham
            if {$ham ne {} && ![dict exists $proven ham]} {dict set proven ham $i}
            if {$spam ne {} && ![dict exists $proven spam]} {dict set proven spam $i}
            if {[dict size $proven] == 2} break
            if {[dict exists $proven ham]} {
                # ham is answered, spam is not yet
                assert_equal {} $spam
            }
            lassign [gcmd_example $i] text label
            lassign [r gpredict p:strict TEXT $text] action arg
            if {$action eq "ACCEPT"} {
                assert_equal ham $label
                assert_equal ham $arg
            } else {
                if {$label eq "spam"} {assert_equal ABSTAIN $action}
                assert_equal 1 [r glearn p:strict $arg LABEL $label]
            }
        }
        assert_equal 2 [dict size $proven]
        assert {[dict get $proven spam] > 4 * [dict get $proven ham]}
        assert_equal 0 [gcmd_info p:strict audit-errors]
        r del p:strict
    }

    gcmd_test {GPREDICT: named features decide where the text does not; labels come back byte for byte} {
        set yes "yes please"
        set no [binary format H* e38184e38184e38188]
        set text "the same text every time"
        gcmd_create_fast p:feat ALPHA 0.2 DELTA 0.2 CLASSES 2 $yes $no
        for {set i 0} {$i < 40} {incr i} {
            if {$i % 2} {
                r gadd p:feat TEXT $text FEATURES 1 flag 2 LABEL $yes
            } else {
                r gadd p:feat TEXT $text FEATURES 1 flag -2 LABEL $no
            }
        }
        for {set i 0} {$i < 600} {incr i} {
            set th [gcmd_info p:feat thresholds]
            if {[lindex $th 0] ne {} && [lindex $th 1] ne {}} break
            if {$i % 2} {
                lassign [r gpredict p:feat TEXT $text FEATURES 1 flag 2] action id
                assert_equal 1 [r glearn p:feat $id LABEL $yes]
            } else {
                lassign [r gpredict p:feat TEXT $text FEATURES 1 flag -2] action id
                assert_equal 1 [r glearn p:feat $id LABEL $no]
            }
        }
        assert_equal ACTIVE [gcmd_info p:feat state]
        assert_equal [list $yes $no] [gcmd_info p:feat classes]

        set accepted {}
        for {set i 0} {$i < 100} {incr i} {
            foreach {value label} [list 2 $yes -2 $no] {
                lassign [r gpredict p:feat TEXT $text FEATURES 1 flag $value] action arg
                if {$action eq "ACCEPT"} {
                    assert_equal $label $arg
                    dict incr accepted $label
                } else {
                    assert_equal AUDIT $action
                    assert_equal 1 [r glearn p:feat $arg LABEL $label]
                }
            }
        }
        assert {[dict get $accepted $yes] > 50}
        assert {[dict get $accepted $no] > 50}
        r del p:feat
    }

    gcmd_test {GPREDICT: a gate that is full abstains even where it would answer} {
        r gcreate p:busy ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score p:busy
        assert_equal 0 [gcmd_info p:busy pending]
        # 4,096 spot checks nobody answers
        set replies [gcmd_pipeline_predict p:busy 4096 0.9]
        foreach reply $replies {
            assert_equal AUDIT [lindex $reply 0]
            assert {[lindex $reply 1] ne {}}
        }
        assert_equal 4096 [gcmd_info p:busy pending]
        assert_equal ACTIVE [gcmd_info p:busy state]
        foreach score {0.9 0.99 0.05} {
            assert_equal {ABSTAIN {}} [r gpredict p:busy SCORE $score]
        }
        # the oldest answer makes room: the gate answers (here: audits) again
        assert_equal 1 [r glearn p:busy [lindex [lindex $replies 0] 1] CORRECT 1]
        lassign [r gpredict p:busy SCORE 0.9] action id
        assert_equal AUDIT $action
        assert {$id ne {}}
        assert_equal {ABSTAIN {}} [r gpredict p:busy SCORE 0.9]
        r del p:busy
    }

    gcmd_test {GINFO: audit-samples and audit-errors count the spot checks and those that disagreed} {
        # a score gate
        r gcreate p:errors ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score p:errors
        assert_equal 0 [gcmd_info p:errors audit-samples]
        foreach {correct samples errors} {1 1 0  1 2 0  0 3 1  1 4 1} {
            lassign [r gpredict p:errors SCORE 0.9] action id
            assert_equal AUDIT $action
            # nothing is counted before the teacher's answer is known
            assert_equal [expr {$samples - 1}] [gcmd_info p:errors audit-samples]
            assert_equal 1 [r glearn p:errors $id CORRECT $correct]
            set info [r ginfo p:errors]
            assert_equal $samples [dict get $info audit-samples]
            assert_equal $errors [dict get $info audit-errors]
        }
        # one disagreement in four is no reason to stop answering
        set info [r ginfo p:errors]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 0 [dict get $info revocations]
        assert_equal 4 [dict get $info audited]
        r del p:errors

        # a classifier: the teacher disagrees with the gate's label once
        gcmd_create_fast p:errors ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham
        gcmd_prove_classifier p:errors
        foreach {teacher samples errors} {spam 1 0  ham 2 1  spam 3 1} {
            lassign [r gpredict p:errors TEXT "buy cheap pills now"] action id
            assert_equal AUDIT $action
            assert_equal 1 [r glearn p:errors $id LABEL $teacher]
            set info [r ginfo p:errors]
            assert_equal $samples [dict get $info audit-samples]
            assert_equal $errors [dict get $info audit-errors]
        }
        assert_equal ACTIVE [gcmd_info p:errors state]
        r del p:errors
    }

    gcmd_test {GINFO: a gate that answers} {
        foreach key {p:score p:audit p:cls} {
            set info [r ginfo $key]
            assert_equal ACTIVE [dict get $info state]
            assert {[dict get $info generation] >= 1}
            # the counters add up
            assert_equal [dict get $info requests] \
                [expr {[dict get $info accepted] + [dict get $info audited] + [dict get $info abstained]}]
            # a share of the requests
            assert {[dict get $info coverage] > 0 && [dict get $info coverage] <= 1}
            assert {[dict get $info audit-errors] <= [dict get $info audit-samples]}
            assert_equal 0 [dict get $info audit-errors]
            assert_equal 0 [dict get $info missing]
            assert_equal 0 [dict get $info revocations]
            assert_equal 0 [dict get $info pending]
        }
        assert {[gcmd_info p:score accepted] > 0}
        assert {[gcmd_info p:cls accepted] > 0}
        assert {[gcmd_info p:cls learned] >= 40}
        assert {[gcmd_info p:score calibration-samples] > 0}
    }

    gcmd_test {GPAUSE: a gate that answers stops answering; GRESUME lets it answer again} {
        foreach {key form answer} {p:score {SCORE 0.9} {CORRECT 1}
                                   p:cls {TEXT {meeting at noon tomorrow}} {LABEL ham}} {
            set thresholds [gcmd_info $key thresholds]
            assert_equal OK [r gpause $key]
            assert_equal PAUSED [gcmd_info $key state]
            set accepted [gcmd_info $key accepted]
            for {set i 0} {$i < 60} {incr i} {
                set reply [r gpredict $key {*}$form]
                assert_equal 2 [llength $reply]
                # not ACCEPT, and not AUDIT either: the gate would not have answered
                assert_equal ABSTAIN [lindex $reply 0]
                if {[lindex $reply 1] ne {}} {
                    assert_equal 1 [r glearn $key [lindex $reply 1] {*}$answer]
                }
            }
            assert_equal $accepted [gcmd_info $key accepted]
            # what was proven is kept while paused
            assert_equal $thresholds [gcmd_info $key thresholds]
            assert_equal PAUSED [gcmd_info $key state]

            assert_equal OK [r gresume $key]
            assert_equal ACTIVE [gcmd_info $key state]
            set accepts 0
            for {set i 0} {$i < 60} {incr i} {
                lassign [r gpredict $key {*}$form] action arg
                if {$action eq "ACCEPT"} {
                    incr accepts
                } else {
                    assert_equal AUDIT $action
                    assert_equal 1 [r glearn $key $arg {*}$answer]
                }
            }
            assert {$accepts > 0}
        }
    }

    gcmd_test {GPAUSE: a paused gate proves itself and answers as soon as it is resumed} {
        # "a paused gate keeps learning and proving itself (shadow mode)"
        r gcreate p:shadow ALPHA 0.2 DELTA 0.2 SCORE
        r gpause p:shadow
        set actions {}
        for {set i 0} {$i < 400} {incr i} {
            if {[lindex [gcmd_info p:shadow thresholds] 0] ne {}} break
            foreach {score correct} {0.9 1 0.1 0} {
                lassign [r gpredict p:shadow SCORE $score] action id
                dict set actions $action 1
                if {$id ne {}} {r glearn p:shadow $id CORRECT $correct}
            }
        }
        assert_equal ABSTAIN [dict keys $actions]
        set info [r ginfo p:shadow]
        assert_equal PAUSED [dict get $info state]
        assert_equal 1 [dict get $info generation]
        assert {[lindex [dict get $info thresholds] 0] ne {}}
        assert_equal 0 [dict get $info accepted]
        assert_equal {ABSTAIN} [lsort -unique [lmap reply [gcmd_pipeline_predict p:shadow 50 0.9] {lindex $reply 0}]]

        assert_equal OK [r gresume p:shadow]
        assert_equal ACTIVE [gcmd_info p:shadow state]
        set actions [lsort -unique [lmap reply [gcmd_pipeline_predict p:shadow 100 0.9] {lindex $reply 0}]]
        assert {"ACCEPT" in $actions}
        assert {"ABSTAIN" ni $actions}
        r del p:shadow
    }

    gcmd_test {GRESET: a gate that answers forgets its proof} {
        foreach {key kind} {p:reset:score score p:reset:cls classifier} {
            if {$kind eq "score"} {
                r gcreate $key ALPHA 0.2 DELTA 0.2 SCORE AUDIT 0.5
                gcmd_prove_score $key
            } else {
                gcmd_create_fast $key ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham AUDIT 0.5
                gcmd_prove_classifier $key
            }
            set before [r ginfo $key]
            assert_equal ACTIVE [dict get $before state]
            # leave a spot check waiting
            for {set i 0} {$i < 200} {incr i} {
                if {$kind eq "score"} {
                    lassign [r gpredict $key SCORE 0.9] action id
                } else {
                    lassign [r gpredict $key TEXT "buy cheap pills now"] action id
                }
                if {$action eq "AUDIT"} break
            }
            assert_equal AUDIT $action

            assert_equal OK [r greset $key]
            set after [r ginfo $key]
            # the configuration is kept
            foreach field {kind alpha delta audit classes alphas} {
                assert_equal [dict get $after $field] [dict get $before $field] "$key: $field"
            }
            # everything learned and proven is gone
            assert_equal [expr {$kind eq "score" ? "CALIBRATING" : "COLD"}] [dict get $after state]
            assert_equal 0 [dict get $after generation]
            foreach threshold [dict get $after thresholds] {assert_equal {} $threshold}
            foreach field {learned requests accepted audited abstained audit-samples audit-errors
                           calibration-samples pending missing revocations} {
                assert_equal [dict get $after $field] 0 "$key: $field after GRESET"
            }
            assert {[dict get $after coverage] == 0}
            assert_equal 0 [r glearn $key $id [expr {$kind eq "score" ? "CORRECT" : "LABEL"}] \
                                [expr {$kind eq "score" ? 1 : "spam"}]]
            # nothing is answered any more
            for {set i 0} {$i < 20} {incr i} {
                if {$kind eq "score"} {
                    set reply [r gpredict $key SCORE 0.9]
                } else {
                    set reply [r gpredict $key TEXT "buy cheap pills now"]
                }
                assert_equal ABSTAIN [lindex $reply 0]
                assert {[lindex $reply 1] ne {}}
            }
            # snapshot numbers are not reused
            if {$kind eq "score"} {
                assert {[dict get $after candidate] > [dict get $before generation]}
            } else {
                gcmd_teach $key 40
                assert {[gcmd_info $key candidate] > [dict get $before candidate]}
                assert {[gcmd_info $key candidate] > [dict get $before generation]}
            }
            r del $key
        }
    }

    gcmd_test {the client loop of SPEC.md gets the teacher's answers} {
        set teacher 0
        for {set i 0} {$i < 100} {incr i} {
            lassign [gcmd_example $i] text label
            lassign [r gpredict p:cls TEXT $text] action arg
            if {$action eq "ACCEPT"} {
                assert_equal $label $arg
                continue
            }
            incr teacher
            if {$arg ne {}} {assert_equal 1 [r glearn p:cls $arg LABEL $label]}
        }
        # the gate saved most of the teacher calls
        assert {$teacher < 50}
    }

    # ------------------------------------------------- RESP2 and RESP3, typed

    gcmd_test {setup: a full gate and a string for the typed replies} {
        r gcreate rp:full ALPHA 0.2 SCORE
        gcmd_pipeline_predict rp:full 4096 0.5
        assert_equal 4096 [gcmd_info rp:full pending]
        r set rp:string value
    }

    foreach proto [gcmd_protocols] {
        set fd {}
        gcmd_test "RESP$proto: HELLO $proto" {
            r hello $proto
            set fd [gcmd_raw_open $proto]
        }
        if {$fd eq {}} continue

        gcmd_test "RESP$proto: GCREATE replies the simple string OK; BUSYKEY is an error" {
            gcmd_raw $fd DEL rp:cls rp:score
            assert_equal {status OK} [gcmd_raw $fd GCREATE rp:cls ALPHA 0.1 CLASSES 3 a b c \
                                          ALWAYSDEFER 1 b CLASSALPHA 1 c 0.05]
            assert_equal {status OK} [gcmd_raw $fd GCREATE rp:score ALPHA 0.1 SCORE]
            foreach key {rp:cls rp:score rp:string} {
                set reply [gcmd_raw $fd GCREATE $key ALPHA 0.1 SCORE]
                assert_equal error [lindex $reply 0]
                assert_match "BUSYKEY*" [lindex $reply 1]
            }
            r del rp:other
            assert_equal OK [r gcreate rp:other ALPHA 0.1 SCORE]
            assert_error "BUSYKEY*" {r gcreate rp:other ALPHA 0.1 SCORE}
        }

        gcmd_test "RESP$proto: GPREDICT replies an array of two, ABSTAIN and an id" {
            foreach {key form} {rp:cls {TEXT hello} rp:cls {TEXT hello FEATURES 1 x 0.5}
                                rp:score {SCORE 0.5}} {
                lassign [gcmd_check_predict [gcmd_raw $fd GPREDICT $key {*}$form]] action id
                assert_equal ABSTAIN $action
                assert {$id ne {}}
                # the same through the test client
                set reply [r gpredict $key {*}$form]
                assert_equal 2 [llength $reply]
                assert_equal ABSTAIN [lindex $reply 0]
                assert {[lindex $reply 1] ne {} && [lindex $reply 1] ne $id}
            }
        }

        gcmd_test "RESP$proto: GLEARN replies the integer 1, then 0" {
            lassign [gcmd_check_predict [gcmd_raw $fd GPREDICT rp:cls TEXT hello]] action id
            assert_equal {int 1} [gcmd_raw $fd GLEARN rp:cls $id LABEL a]
            assert_equal {int 0} [gcmd_raw $fd GLEARN rp:cls $id LABEL a]
            assert_equal {int 0} [gcmd_raw $fd GLEARN rp:cls not-an-id LABEL a]
            lassign [gcmd_check_predict [gcmd_raw $fd GPREDICT rp:score SCORE 0.5]] action id
            assert_equal {int 1} [gcmd_raw $fd GLEARN rp:score $id CORRECT 1]
            assert_equal {int 0} [gcmd_raw $fd GLEARN rp:score $id CORRECT 1]
            lassign [r gpredict rp:score SCORE 0.5] action id
            assert_equal 1 [r glearn rp:score $id CORRECT 0]
            assert_equal 0 [r glearn rp:score $id CORRECT 0]
        }

        gcmd_test "RESP$proto: GADD replies an integer" {
            set learned [r gadd rp:cls TEXT hello LABEL a]
            assert_equal [list int [expr {$learned + 1}]] [gcmd_raw $fd GADD rp:cls TEXT hello LABEL b]
            assert_equal [list int [expr {$learned + 2}]] \
                [gcmd_raw $fd GADD rp:cls TEXT hello FEATURES 1 x 1 LABEL c]
        }

        gcmd_test "RESP$proto: GPAUSE, GRESUME and GRESET reply the simple string OK" {
            foreach key {rp:cls rp:score} {
                assert_equal {status OK} [gcmd_raw $fd GPAUSE $key]
                assert_equal {status OK} [gcmd_raw $fd GPAUSE $key]
                assert_equal {status OK} [gcmd_raw $fd GRESUME $key]
                assert_equal {status OK} [gcmd_raw $fd GRESUME $key]
                assert_equal {status OK} [gcmd_raw $fd GRESET $key]
                assert_equal {status OK} [gcmd_raw $fd GRESET $key]
                assert_equal OK [r gpause $key]
                assert_equal OK [r gresume $key]
                assert_equal OK [r greset $key]
            }
        }

        gcmd_test "RESP$proto: GINFO of a classifier: every field, in order, typed" {
            set info [gcmd_check_info $proto [gcmd_raw $fd GINFO rp:cls]]
            assert_equal classifier [dict get $info kind]
            assert_equal COLD [dict get $info state]
            assert {[dict get $info alpha] == 0.1}
            assert {[dict get $info delta] == 0.01}
            assert {[dict get $info audit] == 0.05}
            assert_equal {a b c} [dict get $info classes]
            assert_equal 3 [llength [dict get $info alphas]]
            lassign [dict get $info alphas] a b c
            assert {$a == 0.1 && $b == 0 && $c == 0.05}
            assert_equal {{} {} {}} [dict get $info thresholds]
            assert_equal 0 [dict get $info generation]
            assert_equal 0 [dict get $info candidate]
            assert {[dict get $info coverage] == 0}
            assert {[dict get $info memory] > 0}
            # the test client sees the same fields
            assert_equal [dict keys $info] [dict keys [r ginfo rp:cls]]
        }

        gcmd_test "RESP$proto: GINFO of a score gate: every field, in order, typed" {
            set info [gcmd_check_info $proto [gcmd_raw $fd GINFO rp:score]]
            assert_equal score [dict get $info kind]
            assert_equal CALIBRATING [dict get $info state]
            assert_equal {} [dict get $info classes]
            assert_equal 1 [llength [dict get $info alphas]]
            assert {[lindex [dict get $info alphas] 0] == 0.1}
            assert_equal {{}} [dict get $info thresholds]
            assert {[dict get $info candidate] >= 1}
            assert_equal [dict keys $info] [dict keys [r ginfo rp:score]]
        }

        gcmd_test "RESP$proto: GINFO of gates that answer: thresholds are numbers" {
            set info [gcmd_check_info $proto [gcmd_raw $fd GINFO p:cls]]
            assert_equal ACTIVE [dict get $info state]
            assert_equal {spam ham} [dict get $info classes]
            assert_equal 2 [llength [dict get $info thresholds]]
            foreach threshold [dict get $info thresholds] {
                assert {$threshold >= 0.5 && $threshold <= 0.999}
            }
            assert {[dict get $info coverage] > 0 && [dict get $info coverage] <= 1}
            assert {[dict get $info generation] >= 1}

            set info [gcmd_check_info $proto [gcmd_raw $fd GINFO p:score]]
            assert_equal ACTIVE [dict get $info state]
            assert_equal 1 [llength [dict get $info thresholds]]
            set threshold [lindex [dict get $info thresholds] 0]
            assert {$threshold > 0.1 && $threshold <= 0.9}
            assert {[dict get $info coverage] > 0 && [dict get $info coverage] <= 1}
        }

        gcmd_test "RESP$proto: ACCEPT and the label as a bulk string" {
            set accepts 0
            for {set i 0} {$i < 100 && $accepts < 3} {incr i} {
                lassign [gcmd_example $i] text label
                set typed [gcmd_raw $fd GPREDICT p:cls TEXT $text]
                lassign [gcmd_check_predict $typed] action arg
                if {$action eq "ACCEPT"} {
                    assert_equal [list bulk $label] [lindex [lindex $typed 1] 1]
                    incr accepts
                } else {
                    assert_equal AUDIT $action
                    assert_equal {int 1} [gcmd_raw $fd GLEARN p:cls $arg LABEL $label]
                }
            }
            assert_equal 3 $accepts
        }

        gcmd_test "RESP$proto: ACCEPT and a null from a score gate" {
            set accepts 0
            for {set i 0} {$i < 100 && $accepts < 3} {incr i} {
                set typed [gcmd_raw $fd GPREDICT p:score SCORE 0.9]
                lassign [gcmd_check_predict $typed] action arg
                if {$action eq "ACCEPT"} {
                    assert_equal null [lindex [lindex $typed 1] 1]
                    incr accepts
                } else {
                    assert_equal AUDIT $action
                    assert_equal {int 1} [gcmd_raw $fd GLEARN p:score $arg CORRECT 1]
                }
            }
            assert_equal 3 $accepts
            # the test client reads the null as an empty string
            for {set i 0} {$i < 100} {incr i} {
                lassign [r gpredict p:score SCORE 0.9] action arg
                if {$action eq "ACCEPT"} break
                r glearn p:score $arg CORRECT 1
            }
            assert_equal {ACCEPT {}} [list $action $arg]
        }

        gcmd_test "RESP$proto: AUDIT and an id" {
            for {set i 0} {$i < 5} {incr i} {
                set typed [gcmd_raw $fd GPREDICT p:audit SCORE 0.9]
                lassign [gcmd_check_predict $typed] action id
                assert_equal AUDIT $action
                assert_equal bulk [lindex [lindex [lindex $typed 1] 1] 0]
                assert_equal {int 1} [gcmd_raw $fd GLEARN p:audit $id CORRECT 1]
            }
        }

        gcmd_test "RESP$proto: ABSTAIN and a null" {
            # a gate that is full
            set typed [gcmd_raw $fd GPREDICT rp:full SCORE 0.5]
            assert_equal {ABSTAIN {}} [gcmd_check_predict $typed]
            assert_equal null [lindex [lindex $typed 1] 1]
            assert_equal {ABSTAIN {}} [r gpredict rp:full SCORE 0.5]
            # a gate that does not know and wants no feedback
            set nulls 0
            for {set i 0} {$i < 100 && $nulls < 3} {incr i} {
                set typed [gcmd_raw $fd GPREDICT p:score SCORE 0.05]
                lassign [gcmd_check_predict $typed] action arg
                assert_equal ABSTAIN $action
                if {$arg eq {}} {
                    assert_equal null [lindex [lindex $typed 1] 1]
                    incr nulls
                } else {
                    assert_equal {int 1} [gcmd_raw $fd GLEARN p:score $arg CORRECT 0]
                }
            }
            assert_equal 3 $nulls
        }

        gcmd_test "RESP$proto: errors are error replies" {
            foreach form $::gcmd_forms {
                set cmd [gcmd_form $form rp:missing]
                assert_equal {error {ERR no such key}} [gcmd_raw $fd {*}$cmd] "command: $cmd"
                set reply [gcmd_raw $fd {*}[gcmd_form $form rp:string]]
                assert_equal error [lindex $reply 0]
                assert_match "WRONGTYPE *" [lindex $reply 1]
            }
            foreach cmd {
                {GCREATE rp:bad ALPHA 2 SCORE}
                {GCREATE rp:bad ALPHA 0.1 CLASSES 1 a}
                {GPREDICT rp:cls SCORE 0.5}
                {GPREDICT rp:score TEXT hello}
                {GPREDICT rp:score SCORE 2}
                {GPREDICT rp:cls TEXT hello FEATURES 2 x 1 x 2}
                {GLEARN rp:cls some-id LABEL nosuch}
                {GLEARN rp:score some-id CORRECT 2}
                {GADD rp:score TEXT hello LABEL a}
                {GADD rp:cls TEXT hello LABEL nosuch}
                {GINFO}
                {GPAUSE rp:cls extra}
            } {
                set reply [gcmd_raw $fd {*}$cmd]
                assert_equal [lindex $reply 0] error "command: $cmd"
                assert_match "ERR *" [lindex $reply 1]
            }
            assert_equal {int 0} [gcmd_raw $fd EXISTS rp:bad rp:missing]
        }

        catch {close $fd}
    }
    catch {r hello 2}

    gcmd_test {the server is still up (syntax, replies and errors)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 2. A gate is a key
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77001

    gcmd_test {TYPE returns gate-type; SCAN finds gates by type} {
        r gcreate kt:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate kt:score ALPHA 0.1 SCORE
        r set kt:string value
        assert_equal gate-type [r type kt:cls]
        assert_equal gate-type [r type kt:score]
        assert_equal 2 [r exists kt:cls kt:score]
        set found {}
        set cursor 0
        while 1 {
            lassign [r scan $cursor type gate-type] cursor keys
            lappend found {*}$keys
            if {$cursor == 0} break
        }
        assert_equal {kt:cls kt:score} [lsort $found]
        assert_equal {kt:cls kt:score kt:string} [lsort [r keys kt:*]]
    }

    gcmd_test {commands of other types reply WRONGTYPE on a gate and leave it alone} {
        lassign [r gpredict kt:cls TEXT hello] action id
        set before [gcmd_state kt:cls]
        foreach cmd {
            {get kt:cls} {append kt:cls x} {incr kt:cls} {strlen kt:cls} {getrange kt:cls 0 1}
            {setrange kt:cls 0 x} {lpush kt:cls x} {lrange kt:cls 0 -1} {hset kt:cls f v}
            {hgetall kt:cls} {sadd kt:cls m} {smembers kt:cls} {zadd kt:cls 1 m}
            {zrange kt:cls 0 -1} {xadd kt:cls * f v} {pfadd kt:cls x} {setbit kt:cls 1 1}
            {getex kt:cls} {getdel kt:cls} {sort kt:cls} {lpos kt:cls x}
        } {
            assert_error "WRONGTYPE*" {r {*}$cmd} "command: $cmd"
        }
        assert_equal 0 [r setnx kt:cls value]
        assert_equal $before [gcmd_state kt:cls]
        assert_equal 1 [r glearn kt:cls $id LABEL a]
    }

    gcmd_test {a key of another module's type is the wrong type too} {
        # Vector sets are a module type like a gate; skip where they are not built in.
        if {[r command info vadd] ne {{}} && [r command info vadd] ne {}} {
            r vadd kt:vset VALUES 3 1 0 0 element
            set digest [debug_digest_value kt:vset]
            foreach form $::gcmd_forms {
                set cmd [gcmd_form $form kt:vset]
                assert_error "WRONGTYPE*" {r {*}$cmd} "on a vector set: $cmd"
            }
            assert_error "BUSYKEY*" {r gcreate kt:vset ALPHA 0.1 SCORE}
            assert_equal $digest [debug_digest_value kt:vset]
            assert_equal 1 [r vcard kt:vset]
            # ... and a gate is the wrong type for the other module
            set before [gcmd_state kt:score]
            assert_error "WRONGTYPE*" {r vcard kt:score}
            assert_error "WRONGTYPE*" {r vadd kt:score VALUES 3 1 0 0 element}
            assert_equal $before [gcmd_state kt:score]
            r del kt:vset
        }
    }

    gcmd_test {SET over a gate replaces it like any key} {
        r gcreate kt:over ALPHA 0.1 SCORE
        assert_equal OK [r set kt:over value]
        assert_equal string [r type kt:over]
        assert_error "WRONGTYPE*" {r ginfo kt:over}
        assert_error "WRONGTYPE*" {r gpredict kt:over SCORE 0.5}
        assert_error "BUSYKEY*" {r gcreate kt:over ALPHA 0.1 SCORE}
        r del kt:over
        assert_equal OK [r gcreate kt:over ALPHA 0.1 SCORE]
        r del kt:over
    }

    gcmd_test {DEL then GCREATE: a new gate; ids of the old one are refused} {
        r gcreate kd:cls ALPHA 0.1 CLASSES 2 a b AUDIT 0.5
        r gcreate kd:score ALPHA 0.1 SCORE AUDIT 0.5
        r gadd kd:cls TEXT hello LABEL a
        lassign [r gpredict kd:cls TEXT hello] action old_cls
        lassign [r gpredict kd:score SCORE 0.5] action old_score
        r gpause kd:cls
        assert_equal 2 [r del kd:cls kd:score]
        assert_equal 0 [r exists kd:cls kd:score]
        assert_error "ERR no such key" {r glearn kd:cls $old_cls LABEL a}
        assert_error "ERR no such key" {r ginfo kd:score}

        # the other kind under each name, to make sure nothing is left over
        assert_equal OK [r gcreate kd:cls ALPHA 0.2 SCORE]
        assert_equal OK [r gcreate kd:score ALPHA 0.2 CLASSES 2 x y]
        set info [r ginfo kd:cls]
        assert_equal score [dict get $info kind]
        assert_equal CALIBRATING [dict get $info state]
        assert {[dict get $info alpha] == 0.2}
        assert {[dict get $info audit] == 0.05}
        assert_equal 0 [dict get $info requests]
        assert_equal 0 [dict get $info pending]
        assert_equal 1 [dict get $info candidate]
        set info [r ginfo kd:score]
        assert_equal classifier [dict get $info kind]
        assert_equal COLD [dict get $info state]
        assert_equal {x y} [dict get $info classes]
        assert_equal 0 [dict get $info learned]

        assert_equal 0 [r glearn kd:cls $old_score CORRECT 1]
        assert_equal 0 [r glearn kd:cls $old_cls CORRECT 1]
        assert_equal 0 [r glearn kd:score $old_cls LABEL x]
        assert_equal 0 [r glearn kd:score $old_score LABEL x]
        lassign [r gpredict kd:cls SCORE 0.5] action id
        assert {$id ne $old_cls && $id ne $old_score}
        assert_equal 1 [r glearn kd:cls $id CORRECT 1]

        # and the same kind again
        r del kd:cls
        assert_equal OK [r gcreate kd:cls ALPHA 0.2 SCORE]
        assert_equal 0 [r glearn kd:cls $id CORRECT 1]
        assert_equal 0 [gcmd_info kd:cls calibration-samples]
    }

    gcmd_test {EXPIRE, TTL and PERSIST work on a gate} {
        r gcreate ke:gate ALPHA 0.1 SCORE
        assert_equal -1 [r ttl ke:gate]
        assert_equal 1 [r expire ke:gate 100]
        assert_range [r ttl ke:gate] 1 100
        # Using the gate does not drop its time to live.
        lassign [r gpredict ke:gate SCORE 0.5] action id
        r glearn ke:gate $id CORRECT 1
        r gpause ke:gate
        r gresume ke:gate
        r greset ke:gate
        r ginfo ke:gate
        assert_range [r ttl ke:gate] 1 100
        assert_equal 1 [r persist ke:gate]
        assert_equal -1 [r ttl ke:gate]
        assert_equal score [gcmd_info ke:gate kind]
        assert_equal 0 [r persist ke:gate]
    }

    gcmd_test {a gate that expired is gone and its name is free} {
        r gcreate ke:gone ALPHA 0.1 CLASSES 2 a b
        lassign [r gpredict ke:gone TEXT hello] action id
        assert_equal 1 [r pexpire ke:gone 1]
        wait_for_condition 100 10 {
            [r exists ke:gone] == 0
        } else {
            fail "the gate did not expire"
        }
        foreach form $::gcmd_forms {
            assert_error "ERR no such key" {r {*}[gcmd_form $form ke:gone]}
        }
        assert_equal OK [r gcreate ke:gone ALPHA 0.1 CLASSES 2 a b]
        assert_equal -1 [r ttl ke:gone]
        assert_equal 0 [r glearn ke:gone $id LABEL a]
        assert_equal 0 [gcmd_info ke:gone requests]
    }

    gcmd_test {GCREATE over a key that expired but was not collected yet} {
        if {[lsearch $::denytags "needs:debug"] < 0} {
            r debug set-active-expire 0
            r set ke:lazy value px 1
            r gcreate ke:lazy:gate ALPHA 0.1 SCORE
            r pexpire ke:lazy:gate 1
            after 20
            assert_equal OK [r gcreate ke:lazy ALPHA 0.1 SCORE]
            assert_equal OK [r gcreate ke:lazy:gate ALPHA 0.2 CLASSES 2 a b]
            assert_equal -1 [r ttl ke:lazy]
            assert_equal -1 [r ttl ke:lazy:gate]
            assert_equal classifier [gcmd_info ke:lazy:gate kind]
            r debug set-active-expire 1
        }
    }

    gcmd_test {RENAME keeps the whole state of a gate} {
        r gcreate kr:src ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham AUDIT 0.5
        gcmd_teach kr:src 10
        lassign [r gpredict kr:src TEXT "buy cheap pills now"] action id
        r gpause kr:src
        set before [gcmd_state kr:src]

        assert_equal OK [r rename kr:src kr:dst]
        assert_equal 0 [r exists kr:src]
        assert_error "ERR no such key" {r ginfo kr:src}
        assert_error "ERR no such key" {r glearn kr:src $id LABEL spam}
        assert_equal gate-type [r type kr:dst]
        assert_equal $before [gcmd_state kr:dst]
        # the request that was waiting moved with it
        assert_equal 1 [r glearn kr:dst $id LABEL spam]
        assert_equal 0 [r glearn kr:dst $id LABEL spam]
        assert_equal 12 [r gadd kr:dst TEXT hello LABEL ham]
        assert_equal PAUSED [gcmd_info kr:dst state]

        # RENAMENX, and RENAME over another gate and over a string
        r gcreate kr:other ALPHA 0.1 SCORE
        r set kr:string value
        assert_equal 0 [r renamenx kr:dst kr:other]
        assert_equal score [gcmd_info kr:other kind]
        set before [gcmd_state kr:dst]
        assert_equal OK [r rename kr:dst kr:other]
        assert_equal $before [gcmd_state kr:other]
        assert_equal OK [r rename kr:other kr:string]
        assert_equal gate-type [r type kr:string]
        assert_equal $before [gcmd_state kr:string]
        assert_equal 1 [r renamenx kr:string kr:final]
        assert_equal $before [gcmd_state kr:final]
        assert_equal {} [r keys kr:s*]
    }

    gcmd_test {a proven gate keeps answering after RENAME} {
        r gcreate kr:proven ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score kr:proven
        set before [gcmd_state kr:proven]
        r rename kr:proven kr:proven2
        assert_equal $before [gcmd_state kr:proven2]
        set actions [lsort -unique [lmap reply [gcmd_pipeline_predict kr:proven2 100 0.9] {lindex $reply 0}]]
        assert {"ACCEPT" in $actions}
        assert {"ABSTAIN" ni $actions}
        r del kr:proven2
    }

    gcmd_test {MOVE keeps the whole state of a gate} {
        r select 10
        r del km:gate
        r select 9
        r gcreate km:gate ALPHA 0.1 CLASSES 2 a b
        r gadd km:gate TEXT hello LABEL a
        lassign [r gpredict km:gate TEXT hello] action id
        set before [gcmd_state km:gate]
        assert_equal 1 [r move km:gate 10]
        assert_equal 0 [r exists km:gate]
        assert_error "ERR no such key" {r ginfo km:gate}
        r select 10
        set rc [catch {
            assert_equal $before [gcmd_state km:gate]
            assert_equal 1 [r glearn km:gate $id LABEL b]
            assert_equal 1 [r del km:gate]
        } err]
        r select 9
        if {$rc} {error $err}
    }

    gcmd_test {SWAPDB keeps the whole state of a gate} {
        r select 12
        set empty [r dbsize]
        r select 9
        r gcreate ks:gate ALPHA 0.1 CLASSES 2 a b
        r gadd ks:gate TEXT hello LABEL a
        lassign [r gpredict ks:gate TEXT hello] action id
        set before [gcmd_state ks:gate]
        if {$empty == 0} {
            assert_equal OK [r swapdb 9 12]
            set there [r exists ks:gate]
            r select 12
            set rc [catch {
                assert_equal $before [gcmd_state ks:gate]
            } err]
            r select 9
            assert_equal OK [r swapdb 9 12]
            if {$rc} {error $err}
            assert_equal 0 $there
        }
        assert_equal $before [gcmd_state ks:gate]
        assert_equal 1 [r glearn ks:gate $id LABEL b]
        r del ks:gate
    }

    gcmd_test {the same name in two databases is two gates} {
        r select 10
        r del kdb:gate
        r select 9
        r gcreate kdb:gate ALPHA 0.1 SCORE
        lassign [r gpredict kdb:gate SCORE 0.5] action id9
        r select 10
        set rc [catch {
            assert_error "ERR no such key" {r ginfo kdb:gate}
            assert_equal OK [r gcreate kdb:gate ALPHA 0.2 CLASSES 2 a b]
            lassign [r gpredict kdb:gate TEXT hello] action id10
            assert_equal 0 [r glearn kdb:gate $id9 LABEL a]
            assert_equal 1 [r glearn kdb:gate $id10 LABEL a]
            r del kdb:gate
        } err]
        r select 9
        if {$rc} {error $err}
        assert_equal score [gcmd_info kdb:gate kind]
        assert_equal 1 [r glearn kdb:gate $id9 CORRECT 1]
    }

    gcmd_test {MEMORY USAGE is positive and grows with the requests that wait} {
        r gcreate kmem:score ALPHA 0.1 SCORE
        r gcreate kmem:cls ALPHA 0.1 CLASSES 2 a b
        foreach key {kmem:score kmem:cls} {
            set usage [r memory usage $key]
            set memory [gcmd_info $key memory]
            assert {$usage > 0}
            assert {$memory > 0}

            set ids {}
            for {set i 0} {$i < 100} {incr i} {
                if {$key eq "kmem:score"} {
                    lappend ids [lindex [r gpredict $key SCORE 0.5] 1]
                } else {
                    lappend ids [lindex [r gpredict $key TEXT "a waiting request number $i"] 1]
                }
            }
            assert_equal 100 [gcmd_info $key pending]
            set usage_full [r memory usage $key]
            set memory_full [gcmd_info $key memory]
            assert {$usage_full > $usage}
            assert {$memory_full > $memory}

            # ... and shrinks again once they are answered
            foreach id $ids {
                if {$key eq "kmem:score"} {
                    r glearn $key $id CORRECT 1
                } else {
                    r glearn $key $id LABEL a
                }
            }
            assert_equal 0 [gcmd_info $key pending]
            assert {[gcmd_info $key memory] < $memory_full}
            assert {[r memory usage $key] < $usage_full}
        }
        # a classifier holds a model, a score gate does not
        assert {[gcmd_info kmem:cls memory] > [gcmd_info kmem:score memory]}
    }

    gcmd_test {OBJECT, TOUCH, RANDOMKEY, DBSIZE and friends take a gate} {
        r gcreate ko:gate ALPHA 0.1 CLASSES 2 a b
        r gpredict ko:gate TEXT hello
        assert {[r object encoding ko:gate] ne {}}
        assert {[r object idletime ko:gate] >= 0}
        assert {[r object refcount ko:gate] >= 1}
        # OBJECT FREQ is an error unless an LFU policy is selected ...
        catch {r object freq ko:gate}
        r config set maxmemory-policy allkeys-lfu
        assert {[r object freq ko:gate] >= 0}
        r gpredict ko:gate TEXT hello
        r ginfo ko:gate
        assert {[r object freq ko:gate] >= 0}
        r config set maxmemory-policy noeviction
        assert_equal 1 [r touch ko:gate]
        assert_equal 1 [r exists ko:gate]
        assert {[r dbsize] >= 1}
        assert {[r randomkey] ne {}}
        assert_equal gate-type [r type ko:gate]
        if {[lsearch $::denytags "needs:debug"] < 0} {
            assert_match "*encoding:*" [r debug object ko:gate]
            assert_equal 40 [string length [r debug digest-value ko:gate]]
        }
        assert_equal 2 [gcmd_info ko:gate pending]
        assert_equal PONG [r ping]
    }

    gcmd_test {COPY is refused and creates nothing} {
        r gcreate kc:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate kc:score ALPHA 0.1 SCORE
        r gadd kc:cls TEXT hello LABEL a
        lassign [r gpredict kc:score SCORE 0.5] action id
        r set kc:string value
        r select 10
        r del kc:dst
        r select 9
        foreach key {kc:cls kc:score} {
            set before [gcmd_state $key]
            assert_error "ERR*" {r copy $key kc:dst}
            assert_error "ERR*" {r copy $key kc:dst REPLACE}
            assert_error "ERR*" {r copy $key kc:dst DB 10}
            assert_error "ERR*" {r copy $key kc:dst DB 10 REPLACE}
            assert_equal 0 [r exists kc:dst]
            # an existing destination is not replaced
            assert_error "ERR*" {r copy $key kc:string REPLACE}
            assert_equal value [r get kc:string]
            assert_equal $before [gcmd_state $key]
        }
        r select 10
        set there [r exists kc:dst]
        r select 9
        assert_equal 0 $there
        # the source still works
        assert_equal 1 [r glearn kc:score $id CORRECT 1]
        assert_equal 2 [r gadd kc:cls TEXT hello LABEL b]
        # copying another key over a gate is ordinary COPY
        assert_equal 0 [r copy kc:string kc:cls]
        assert_equal gate-type [r type kc:cls]
        assert_equal 1 [r copy kc:string kc:cls REPLACE]
        assert_equal value [r get kc:cls]
    }

    gcmd_test {DEL, UNLINK, SET and FLUSHDB ASYNC free a gate with many waiting requests} {
        foreach how {del unlink set flushdb} {
            r select 11
            set rc [catch {
                r gcreate kf:big ALPHA 0.1 CLASSES 3 a b c
                for {set i 0} {$i < 300} {incr i} {
                    r gpredict kf:big TEXT "waiting request $i of many"
                }
                assert_equal 300 [gcmd_info kf:big pending]
                switch $how {
                    del {assert_equal 1 [r del kf:big]}
                    unlink {assert_equal 1 [r unlink kf:big]}
                    set {
                        assert_equal OK [r set kf:big value]
                        assert_equal value [r get kf:big]
                        assert_equal 1 [r del kf:big]
                    }
                    flushdb {assert_equal OK [r flushdb async]}
                }
                assert_equal 0 [r exists kf:big]
                assert_equal 0 [r dbsize]
            } err]
            r select 9
            if {$rc} {error $err}
        }
        assert_equal PONG [r ping]
    }

    gcmd_test {key names are binary safe} {
        set names [list {} { } "a b" "new\nline" "nul\x00inside" "\x00" [binary format H* fffe00] \
                       [string repeat k 1000] "{braces}" "*" "quote\"d"]
        set ids {}
        foreach name $names {
            assert_equal OK [r gcreate $name ALPHA 0.1 SCORE]
            assert_equal gate-type [r type $name]
            lappend ids [lindex [r gpredict $name SCORE 0.5] 1]
        }
        # Each is its own gate.
        foreach name $names id $ids {
            assert_equal 1 [gcmd_info $name pending]
            foreach other $names {
                if {$other ne $name} {assert_equal 0 [r glearn $other $id CORRECT 1]}
            }
            assert_equal 1 [r glearn $name $id CORRECT 1]
        }
        assert_equal [llength $names] [r del {*}$names]
    }

    gcmd_test {the server is still up (keys)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 3. COMMAND, ACL and the internal command
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77002

    gcmd_test {COMMAND INFO: arity, write or read-only, one key at position 1} {
        foreach {name arity kind} {
            gcreate -5 write   gpredict -4 write   glearn 5 write   gadd -6 write
            ginfo 2 readonly   gpause 2 write      gresume 2 write  greset 2 write
        } {
            set reply [r command info $name]
            assert_equal [llength $reply] 1 "COMMAND INFO $name"
            set info [lindex $reply 0]
            assert_equal [string tolower [lindex $info 0]] $name
            assert_equal [lindex $info 1] $arity "arity of $name"
            set flags [lindex $info 2]
            if {$kind eq "write"} {
                assert {"write" in $flags}
                assert {"readonly" ni $flags}
            } else {
                assert {"readonly" in $flags}
                assert {"write" ni $flags}
            }
            # first key, last key, step
            assert_equal [lrange $info 3 5] {1 1 1} "keys of $name"
            set categories [lindex $info 6]
            assert {"@gate" in $categories}
            assert {[expr {$kind eq "write" ? "@write" : "@read"}] in $categories}
            # exactly one key specification
            set specs [lindex $info 8]
            assert_equal [llength $specs] 1 "key specifications of $name"
            set spec_flags [dict get [lindex $specs 0] flags]
            if {$kind eq "write"} {
                assert {"RW" in $spec_flags}
            } else {
                assert {"RO" in $spec_flags}
            }
        }
    }

    gcmd_test {COMMAND GETKEYS: the key and nothing else} {
        foreach cmd {
            {gcreate thekey ALPHA 0.1 CLASSES 2 a b}
            {gcreate thekey CLASSES 3 thekey other more ALPHA 0.1}
            {gcreate thekey ALPHA 0.1 SCORE}
            {gpredict thekey TEXT hello}
            {gpredict thekey TEXT hello FEATURES 2 x 1 y 2}
            {gpredict thekey SCORE 0.5}
            {glearn thekey 0123456789abcdef-1 LABEL a}
            {glearn thekey 0123456789abcdef-1 CORRECT 1}
            {gadd thekey TEXT hello LABEL a}
            {gadd thekey TEXT hello FEATURES 1 x 1 LABEL a}
            {ginfo thekey}
            {gpause thekey}
            {gresume thekey}
            {greset thekey}
        } {
            assert_equal [r command getkeys {*}$cmd] thekey "COMMAND GETKEYS $cmd"
            set reply [r command getkeysandflags {*}$cmd]
            assert_equal [llength $reply] 1 "COMMAND GETKEYSANDFLAGS $cmd"
            lassign [lindex $reply 0] key flags
            assert_equal $key thekey
            if {[lindex $cmd 0] eq "ginfo"} {
                assert {"RO" in $flags}
            } else {
                assert {"RW" in $flags}
            }
        }
    }

    gcmd_test {the gate commands are the @gate category} {
        assert_equal {gadd gcreate ginfo glearn gpause gpredict greset gresume} \
            [lsort [string tolower [r acl cat gate]]]
        assert {"gate" in [r acl cat]}
    }

    gcmd_test {ACL: a user limited to +@gate runs every gate command and nothing else} {
        r acl setuser gate-only on nopass ~* +@gate
        set c [gcmd_login gate-only]
        assert_equal OK [$c gcreate acl:cls ALPHA 0.1 CLASSES 2 a b]
        assert_equal OK [$c gcreate acl:score ALPHA 0.1 SCORE]
        lassign [$c gpredict acl:cls TEXT hello] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$c glearn acl:cls $id LABEL a]
        assert_equal 2 [$c gadd acl:cls TEXT hello LABEL b]
        lassign [$c gpredict acl:score SCORE 0.5] action id
        assert_equal 1 [$c glearn acl:score $id CORRECT 1]
        assert_equal classifier [dict get [$c ginfo acl:cls] kind]
        assert_equal OK [$c gpause acl:cls]
        assert_equal OK [$c gresume acl:cls]
        assert_equal OK [$c greset acl:cls]
        foreach cmd {{get acl:cls} {type acl:cls} {del acl:cls} {ping} {set acl:x 1} {exists acl:cls}
                     {expire acl:cls 10} {rename acl:cls acl:y} {dbsize}} {
            assert_error "NOPERM*" {$c {*}$cmd} "command: $cmd"
        }
        $c close
        assert_equal 2 [r exists acl:cls acl:score]
    }

    gcmd_test {ACL: a user with -@gate runs no gate command} {
        r acl setuser no-gate on nopass ~* +@all -@gate
        r del acl:cls acl:score
        r gcreate acl:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate acl:score ALPHA 0.1 SCORE
        set before [list [gcmd_state acl:cls] [gcmd_state acl:score]]
        set c [gcmd_login no-gate]
        foreach key {acl:cls acl:score acl:new} {
            foreach form $::gcmd_forms {
                set cmd [gcmd_form $form $key]
                assert_error "NOPERM*" {$c {*}$cmd} "command: $cmd"
            }
            assert_error "NOPERM*" {$c gcreate $key ALPHA 0.1 SCORE}
            assert_error "NOPERM*" {$c gcreate $key ALPHA 0.1 CLASSES 2 a b}
        }
        # The rest of Redis is theirs.
        assert_equal PONG [$c ping]
        assert_equal gate-type [$c type acl:cls]
        assert_equal 0 [$c exists acl:new]
        $c close
        assert_equal $before [list [gcmd_state acl:cls] [gcmd_state acl:score]]
    }

    gcmd_test {ACL: read permission on the key allows GINFO and no command that writes} {
        r acl setuser gate-reader on nopass %R~* +@gate
        set before [list [gcmd_state acl:cls] [gcmd_state acl:score]]
        set c [gcmd_login gate-reader]
        assert_equal classifier [dict get [$c ginfo acl:cls] kind]
        assert_equal score [dict get [$c ginfo acl:score] kind]
        assert_error "ERR no such key" {$c ginfo acl:new}
        foreach key {acl:cls acl:score acl:new} {
            foreach form $::gcmd_forms {
                if {[lindex $form 0] eq "ginfo"} continue
                set cmd [gcmd_form $form $key]
                assert_error "NOPERM*" {$c {*}$cmd} "command: $cmd"
            }
            assert_error "NOPERM*" {$c gcreate $key ALPHA 0.1 SCORE}
        }
        $c close
        assert_equal $before [list [gcmd_state acl:cls] [gcmd_state acl:score]]
        assert_equal 0 [r exists acl:new]
    }

    gcmd_test {ACL: +gpredict +glearn on a key pattern, the application server of the README} {
        r acl setuser app on nopass ~app:* +gpredict +glearn
        r gcreate app:gate ALPHA 0.1 SCORE
        r gcreate other:gate ALPHA 0.1 SCORE
        set c [gcmd_login app]
        lassign [$c gpredict app:gate SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$c glearn app:gate $id CORRECT 1]
        # not the operator's commands
        foreach cmd {{ginfo app:gate} {gpause app:gate} {gresume app:gate} {greset app:gate}
                     {gcreate app:new ALPHA 0.1 SCORE} {gadd app:gate TEXT hello LABEL a}} {
            assert_error "NOPERM*" {$c {*}$cmd} "command: $cmd"
        }
        # not other keys
        assert_error "NOPERM*" {$c gpredict other:gate SCORE 0.5}
        assert_error "NOPERM*" {$c glearn other:gate $id CORRECT 1}
        $c close
        assert_equal 0 [r exists app:new]
        assert_equal 0 [gcmd_info other:gate requests]
    }

    gcmd_test {_GATE.APPLY is an unknown command to a client} {
        r gcreate ap:gate ALPHA 0.1 SCORE
        set before [gcmd_state ap:gate]
        set attempts {
            {_gate.apply ap:gate TICK 0}
            {_GATE.APPLY ap:gate TICK 0}
            {_GATE.APPLY ap:gate PAUSE 0}
            {_GATE.APPLY ap:gate RESET 0 0000000000000001}
            {_GATE.APPLY ap:gate RECOVER 0 0000000000000001}
            {_GATE.APPLY ap:new CREATE 0 0000000000000001 0000000000000002 1000 300000 ALPHA 0.1 SCORE}
            {_GATE.APPLY ap:new RESTORE garbage}
            {_GATE.APPLY ap:gate}
            {_GATE.APPLY}
        }
        foreach cmd $attempts {
            assert_error "*unknown command*" {r {*}$cmd} "command: $cmd"
        }
        # not for a user who may run everything either
        r acl setuser everything on nopass ~* &* +@all
        set c [gcmd_login everything]
        foreach cmd $attempts {
            assert_error "*unknown command*" {$c {*}$cmd} "command: $cmd"
        }
        $c close
        # not from a transaction
        r multi
        set rc [catch {r _gate.apply ap:gate PAUSE 0} err]
        catch {r exec} exec_err
        assert_equal 1 $rc
        assert_match "*unknown command*" $err
        assert_match "EXECABORT*" $exec_err
        # not from a script
        assert_error "*" {r eval {return redis.call('_GATE.APPLY', KEYS[1], 'PAUSE', '0')} 1 ap:gate}
        assert_error "*" {r eval {return redis.call('_gate.apply', KEYS[1], 'CREATE', '0',
            '0000000000000001', '0000000000000002', '1000', '300000', 'ALPHA', '0.1', 'SCORE')} 1 ap:new}

        assert_equal $before [gcmd_state ap:gate]
        assert_equal 0 [r exists ap:new]
    }

    gcmd_test {the server is still up (COMMAND and ACL)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 4. Server settings
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    gcmd_test {CONFIG GET: the three settings and their defaults} {
        assert_equal {gate.snapshot-examples 1000} [r config get gate.snapshot-examples]
        assert_equal {gate.feedback-timeout 300000} [r config get gate.feedback-timeout]
        assert_equal {gate.test-seed 0} [r config get gate.test-seed]
        set all [r config get gate.*]
        assert_equal {gate.feedback-timeout gate.snapshot-examples gate.test-seed} \
            [lsort [dict keys $all]]
    }

    gcmd_test {CONFIG SET: values in range are stored} {
        # SPEC.md names the settings and their defaults, not their ranges;
        # these are the ranges gate.c registers.
        foreach {name value} {
            gate.snapshot-examples 1          gate.snapshot-examples 20
            gate.snapshot-examples 1000000000 gate.snapshot-examples 1000
            gate.feedback-timeout 1           gate.feedback-timeout 1000
            gate.feedback-timeout 604800000   gate.feedback-timeout 300000
            gate.test-seed 1                  gate.test-seed 9223372036854775807
            gate.test-seed 0
        } {
            assert_equal OK [r config set $name $value]
            assert_equal [list $name $value] [r config get $name]
        }
        # several at once
        assert_equal OK [r config set gate.snapshot-examples 7 gate.feedback-timeout 7000 gate.test-seed 7]
        assert_equal 7 [dict get [r config get gate.snapshot-examples] gate.snapshot-examples]
        assert_equal 7000 [dict get [r config get gate.feedback-timeout] gate.feedback-timeout]
        assert_equal 7 [dict get [r config get gate.test-seed] gate.test-seed]
        assert_equal OK [r config set gate.snapshot-examples 1000 gate.feedback-timeout 300000 gate.test-seed 0]
    }

    gcmd_test {CONFIG SET: values out of range are refused and change nothing} {
        foreach {name value} {
            gate.snapshot-examples 0          gate.snapshot-examples -1
            gate.snapshot-examples 1000000001 gate.snapshot-examples abc
            gate.snapshot-examples 1.5        gate.snapshot-examples {}
            gate.feedback-timeout 0           gate.feedback-timeout -1
            gate.feedback-timeout 604800001   gate.feedback-timeout abc
            gate.feedback-timeout 1.5         gate.feedback-timeout {}
            gate.test-seed -1                 gate.test-seed abc
            gate.test-seed 9223372036854775808
        } {
            assert_error "ERR*" {r config set $name $value} "CONFIG SET $name '$value'"
        }
        assert_error "ERR*" {r config set gate.no-such-setting 1}
        # one bad value refuses the whole command
        assert_error "ERR*" {r config set gate.snapshot-examples 5 gate.feedback-timeout 0}
        assert_equal {gate.snapshot-examples 1000} [r config get gate.snapshot-examples]
        assert_equal {gate.feedback-timeout 300000} [r config get gate.feedback-timeout]
        assert_equal {gate.test-seed 0} [r config get gate.test-seed]
    }

    gcmd_test {a gate can be created and used at both ends of the ranges} {
        foreach {examples timeout} {1 1  1000000000 604800000} {
            r config set gate.snapshot-examples $examples gate.feedback-timeout $timeout
            r del cf:edge:cls cf:edge:score
            assert_equal OK [r gcreate cf:edge:cls ALPHA 0.1 CLASSES 2 a b]
            assert_equal OK [r gcreate cf:edge:score ALPHA 0.1 SCORE]
            assert_equal 1 [r gadd cf:edge:cls TEXT hello LABEL a]
            assert_equal ABSTAIN [lindex [r gpredict cf:edge:cls TEXT hello] 0]
            assert_equal ABSTAIN [lindex [r gpredict cf:edge:score SCORE 0.5] 0]
            # with a step of one example the first snapshot is there already
            assert_equal [expr {$examples == 1 ? 1 : 0}] [gcmd_info cf:edge:cls candidate]
        }
        r config set gate.snapshot-examples 1000 gate.feedback-timeout 300000
        r del cf:edge:cls cf:edge:score
    }

    gcmd_test {gate.snapshot-examples is read when a gate is created} {
        r config set gate.snapshot-examples 5
        r gcreate cf:five ALPHA 0.1 CLASSES 2 spam ham
        r config set gate.snapshot-examples 8
        r gcreate cf:eight ALPHA 0.1 CLASSES 2 spam ham
        r config set gate.snapshot-examples 1000
        r gcreate cf:thousand ALPHA 0.1 CLASSES 2 spam ham

        foreach key {cf:five cf:eight cf:thousand} {gcmd_teach $key 4}
        foreach key {cf:five cf:eight cf:thousand} {
            assert_equal COLD [gcmd_info $key state]
            assert_equal 0 [gcmd_info $key candidate]
        }
        foreach key {cf:five cf:eight cf:thousand} {gcmd_teach $key 1}
        assert_equal CALIBRATING [gcmd_info cf:five state]
        assert_equal 1 [gcmd_info cf:five candidate]
        assert_equal COLD [gcmd_info cf:eight state]
        assert_equal COLD [gcmd_info cf:thousand state]
        foreach key {cf:five cf:eight cf:thousand} {gcmd_teach $key 3}
        assert_equal 1 [gcmd_info cf:five candidate]
        assert_equal CALIBRATING [gcmd_info cf:eight state]
        assert_equal 1 [gcmd_info cf:eight candidate]
        assert_equal COLD [gcmd_info cf:thousand state]
        assert_equal 8 [gcmd_info cf:thousand learned]

        # GRESET keeps what the gate was created with, not what the server
        # is set to now; snapshot numbers go on.
        assert_equal OK [r greset cf:five]
        gcmd_teach cf:five 4
        assert_equal COLD [gcmd_info cf:five state]
        gcmd_teach cf:five 1
        assert_equal CALIBRATING [gcmd_info cf:five state]
        assert_equal 2 [gcmd_info cf:five candidate]
        r del cf:five cf:eight cf:thousand
    }

    gcmd_test {gate.feedback-timeout is read when a gate is created; GLEARN replies 0 to an id that is too old} {
        r config set gate.feedback-timeout 1
        r gcreate cf:fast ALPHA 0.1 SCORE
        r gcreate cf:fast:cls ALPHA 0.1 CLASSES 2 a b
        r config set gate.feedback-timeout 300000
        r gcreate cf:slow ALPHA 0.1 SCORE

        lassign [r gpredict cf:fast SCORE 0.5] action fast
        lassign [r gpredict cf:fast:cls TEXT hello] action fast_cls
        lassign [r gpredict cf:slow SCORE 0.5] action slow
        after 20
        assert_equal 0 [r glearn cf:fast $fast CORRECT 1]
        assert_equal 0 [r glearn cf:fast:cls $fast_cls LABEL a]
        assert_equal 1 [r glearn cf:slow $slow CORRECT 1]

        # A score gate that proves nothing yet needs every answer as proof:
        # this one never got its GLEARN.
        set info [r ginfo cf:fast]
        assert_equal 1 [dict get $info missing]
        assert_equal 0 [dict get $info pending]
        assert_equal 0 [gcmd_info cf:fast:cls pending]
        assert_equal 0 [gcmd_info cf:fast:cls learned]
        set info [r ginfo cf:slow]
        assert_equal 0 [dict get $info missing]
        assert_equal 1 [dict get $info calibration-samples]

        # GRESET keeps the timeout the gate was created with.
        r greset cf:fast
        lassign [r gpredict cf:fast SCORE 0.5] action id
        after 20
        assert_equal 0 [r glearn cf:fast $id CORRECT 1]
        r del cf:fast cf:fast:cls cf:slow
    }

    gcmd_test {gate.test-seed: the same seed gives the same ids and the same draws} {
        # CONFIG SET of the value a setting already has is not a change and
        # does not restart the sequence; going through 0 does.
        set runs {}
        foreach seed {424242 424242 424243} {
            r del cf:seed
            r config set gate.test-seed 0
            r config set gate.test-seed $seed
            r gcreate cf:seed ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
            set log {}
            for {set i 0} {$i < 150} {incr i} {
                set reply [r gpredict cf:seed SCORE 0.9]
                lappend log $reply
                if {[lindex $reply 1] ne {}} {r glearn cf:seed [lindex $reply 1] CORRECT 1}
            }
            lappend runs $log
        }
        lassign $runs first second other
        assert_equal $first $second
        assert {$first ne $other}
        # With AUDIT 0.5 the draws decided between ACCEPT and AUDIT.
        set actions [lsort -unique [lmap reply $first {lindex $reply 0}]]
        assert_equal {ABSTAIN ACCEPT AUDIT} $actions
        r config set gate.test-seed 0
        r del cf:seed
    }

    gcmd_test {gate.test-seed 0: ids are not predictable} {
        set ids {}
        foreach run {1 2 3} {
            r del cf:seed
            r config set gate.test-seed 0
            r gcreate cf:seed ALPHA 0.2 SCORE
            lappend ids [lindex [r gpredict cf:seed SCORE 0.5] 1]
        }
        assert_equal 3 [llength [lsort -unique $ids]]
        r del cf:seed
    }

    gcmd_test {the server is still up (settings)} {
        assert_equal PONG [r ping]
    }
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule \
        gate.snapshot-examples 3 gate.feedback-timeout 60000 gate.test-seed 99]] {

    gcmd_test {the settings can be given in the configuration file} {
        assert_equal {gate.snapshot-examples 3} [r config get gate.snapshot-examples]
        assert_equal {gate.feedback-timeout 60000} [r config get gate.feedback-timeout]
        assert_equal {gate.test-seed 99} [r config get gate.test-seed]
        r gcreate cf:file ALPHA 0.1 CLASSES 2 spam ham
        gcmd_teach cf:file 2
        assert_equal COLD [gcmd_info cf:file state]
        gcmd_teach cf:file 1
        assert_equal CALIBRATING [gcmd_info cf:file state]
        assert_equal 1 [gcmd_info cf:file candidate]
    }
}

# ============================================================================
# 5. MULTI/EXEC and Lua
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77003

    gcmd_test {setup: a gate that answers, one that only audits, one that is full} {
        r gcreate tx:proven ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score tx:proven
        r gcreate tx:audit ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score tx:audit
        r gcreate tx:full ALPHA 0.2 SCORE
        gcmd_pipeline_predict tx:full 4096 0.5
        assert_equal ACTIVE [gcmd_info tx:proven state]
        assert_equal ACTIVE [gcmd_info tx:audit state]
        assert_equal 4096 [gcmd_info tx:full pending]
    }

    gcmd_test {MULTI/EXEC: gate commands are queued and reply as usual} {
        catch {r discard}
        r multi
        assert_equal QUEUED [r gcreate tx:cls ALPHA 0.1 CLASSES 2 a b]
        assert_equal QUEUED [r gcreate tx:score ALPHA 0.1 SCORE]
        r gpredict tx:cls TEXT hello
        r gpredict tx:score SCORE 0.5
        r gadd tx:cls TEXT hello LABEL a
        r gpause tx:cls
        r ginfo tx:cls
        r gresume tx:cls
        r ginfo tx:score
        set replies [r exec]
        assert_equal 9 [llength $replies]
        lassign $replies create_cls create_score predict_cls predict_score add pause info_cls resume info_score
        assert_equal OK $create_cls
        assert_equal OK $create_score
        foreach reply [list $predict_cls $predict_score] {
            assert_equal 2 [llength $reply]
            assert_equal ABSTAIN [lindex $reply 0]
            assert {[lindex $reply 1] ne {}}
        }
        assert_equal 1 $add
        assert_equal OK $pause
        assert_equal OK $resume
        assert_equal PAUSED [dict get $info_cls state]
        assert_equal 1 [dict get $info_cls learned]
        assert_equal 1 [dict get $info_cls pending]
        assert_equal score [dict get $info_score kind]
        assert_equal $info_score [r ginfo tx:score]

        # The answers, in a second transaction.
        r multi
        r glearn tx:cls [lindex $predict_cls 1] LABEL b
        r glearn tx:cls [lindex $predict_cls 1] LABEL b
        r glearn tx:score [lindex $predict_score 1] CORRECT 1
        r glearn tx:score [lindex $predict_cls 1] CORRECT 1
        r greset tx:cls
        r ginfo tx:cls
        set replies [r exec]
        assert_equal {1 0 1 0 OK} [lrange $replies 0 4]
        assert_equal 0 [dict get [lindex $replies 5] learned]
    }

    gcmd_test {MULTI/EXEC: DISCARD drops the queued gate commands} {
        catch {r discard}
        set before [gcmd_state tx:score]
        r multi
        r gpredict tx:score SCORE 0.5
        r gpause tx:score
        r greset tx:score
        r gcreate tx:never ALPHA 0.1 SCORE
        assert_equal OK [r discard]
        assert_equal $before [gcmd_state tx:score]
        assert_equal 0 [r exists tx:never]
    }

    foreach proto [gcmd_protocols] {
        set fd {}
        gcmd_test "RESP$proto: a connection for transactions and scripts" {
            set fd [gcmd_raw_open $proto]
        }
        if {$fd eq {}} continue

        gcmd_test "RESP$proto: EXEC carries each reply in the shape it has outside" {
            gcmd_raw $fd DEL tx:t:cls tx:t:score
            assert_equal {status OK} [gcmd_raw $fd MULTI]
            foreach cmd {
                {GCREATE tx:t:cls ALPHA 0.1 CLASSES 2 a b}
                {GCREATE tx:t:score ALPHA 0.1 SCORE}
                {GPREDICT tx:t:cls TEXT hello}
                {GPREDICT tx:t:score SCORE 0.5}
                {GADD tx:t:cls TEXT hello LABEL a}
                {GLEARN tx:t:cls not-an-id LABEL a}
                {GINFO tx:t:cls}
                {GINFO tx:t:score}
                {GPAUSE tx:t:cls}
                {GRESUME tx:t:cls}
                {GPREDICT tx:proven SCORE 0.05}
                {GPREDICT tx:audit SCORE 0.9}
                {GPREDICT tx:full SCORE 0.5}
                {GRESET tx:t:score}
            } {
                assert_equal {status QUEUED} [gcmd_raw $fd {*}$cmd]
            }
            set reply [gcmd_raw $fd EXEC]
            assert_equal array [lindex $reply 0]
            set items [lindex $reply 1]
            assert_equal 14 [llength $items]
            assert_equal {status OK} [lindex $items 0]
            assert_equal {status OK} [lindex $items 1]
            lassign [gcmd_check_predict [lindex $items 2]] action cls_id
            assert_equal ABSTAIN $action
            assert {$cls_id ne {}}
            lassign [gcmd_check_predict [lindex $items 3]] action score_id
            assert_equal ABSTAIN $action
            assert {$score_id ne {}}
            assert_equal {int 1} [lindex $items 4]
            assert_equal {int 0} [lindex $items 5]
            set info [gcmd_check_info $proto [lindex $items 6]]
            assert_equal classifier [dict get $info kind]
            assert_equal {a b} [dict get $info classes]
            assert_equal 1 [dict get $info pending]
            set info [gcmd_check_info $proto [lindex $items 7]]
            assert_equal score [dict get $info kind]
            assert_equal {{}} [dict get $info thresholds]
            assert_equal {status OK} [lindex $items 8]
            assert_equal {status OK} [lindex $items 9]
            # below the threshold: ABSTAIN, with a null unless it is a spot check
            assert_equal ABSTAIN [lindex [gcmd_check_predict [lindex $items 10]] 0]
            lassign [gcmd_check_predict [lindex $items 11]] action audit_id
            assert_equal AUDIT $action
            assert {$audit_id ne {}}
            assert_equal {ABSTAIN {}} [gcmd_check_predict [lindex $items 12]]
            assert_equal null [lindex [lindex [lindex $items 12] 1] 1]
            assert_equal {status OK} [lindex $items 13]

            # the ids are good outside the transaction
            assert_equal {int 1} [gcmd_raw $fd GLEARN tx:t:cls $cls_id LABEL a]
            assert_equal {int 1} [gcmd_raw $fd GLEARN tx:audit $audit_id CORRECT 1]
            # ... but not the one from before the GRESET
            assert_equal {int 0} [gcmd_raw $fd GLEARN tx:t:score $score_id CORRECT 1]
            set arg [lindex [gcmd_check_predict [lindex $items 10]] 1]
            if {$arg ne {}} {gcmd_raw $fd GLEARN tx:proven $arg CORRECT 0}
        }

        gcmd_test "RESP$proto: an error inside EXEC is one reply; the other commands run" {
            assert_equal {status OK} [gcmd_raw $fd MULTI]
            foreach cmd {
                {GPREDICT tx:t:score TEXT wrong-form}
                {GPREDICT tx:t:missing SCORE 0.5}
                {GLEARN tx:t:cls some-id LABEL nosuch}
                {GADD tx:t:score TEXT hello LABEL a}
                {GCREATE tx:t:cls ALPHA 0.1 SCORE}
                {GCREATE tx:t:bad ALPHA 7 SCORE}
                {GPREDICT tx:t:score SCORE 0.5}
            } {
                assert_equal {status QUEUED} [gcmd_raw $fd {*}$cmd]
            }
            set items [lindex [gcmd_raw $fd EXEC] 1]
            assert_equal 7 [llength $items]
            foreach i {0 2 3 5} {
                assert_equal error [lindex [lindex $items $i] 0]
                assert_match "ERR *" [lindex [lindex $items $i] 1]
            }
            assert_equal {error {ERR no such key}} [lindex $items 1]
            assert_match "BUSYKEY*" [lindex [lindex $items 4] 1]
            lassign [gcmd_check_predict [lindex $items 6]] action id
            assert_equal ABSTAIN $action
            assert_equal {int 1} [gcmd_raw $fd GLEARN tx:t:score $id CORRECT 1]
            assert_equal {int 0} [gcmd_raw $fd EXISTS tx:t:missing tx:t:bad]

            # a wrong number of arguments is refused when queued and aborts
            assert_equal {status OK} [gcmd_raw $fd MULTI]
            assert_equal {status QUEUED} [gcmd_raw $fd GPAUSE tx:t:score]
            assert_equal error [lindex [gcmd_raw $fd GINFO] 0]
            set reply [gcmd_raw $fd EXEC]
            assert_equal error [lindex $reply 0]
            assert_match "EXECABORT*" [lindex $reply 1]
            # nothing of it ran
            assert_equal CALIBRATING [gcmd_info tx:t:score state]
        }

        gcmd_test "RESP$proto: redis.call returns the replies of gate commands" {
            gcmd_raw $fd DEL lua:cls lua:score
            assert_equal {status OK} [gcmd_raw $fd EVAL \
                {return redis.call('GCREATE', KEYS[1], 'ALPHA', '0.1', 'CLASSES', '2', 'a', 'b')} 1 lua:cls]
            assert_equal {status OK} [gcmd_raw $fd EVAL \
                {return redis.call('GCREATE', KEYS[1], 'ALPHA', ARGV[1], 'SCORE')} 1 lua:score 0.1]

            set predict {return redis.call('GPREDICT', KEYS[1], unpack(ARGV))}
            lassign [gcmd_check_predict [gcmd_raw $fd EVAL $predict 1 lua:cls TEXT hello]] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            set learn {return redis.call('GLEARN', KEYS[1], unpack(ARGV))}
            assert_equal {int 1} [gcmd_raw $fd EVAL $learn 1 lua:cls $id LABEL a]
            assert_equal {int 0} [gcmd_raw $fd EVAL $learn 1 lua:cls $id LABEL a]
            assert_equal {int 2} [gcmd_raw $fd EVAL \
                {return redis.call('GADD', KEYS[1], 'TEXT', ARGV[1], 'LABEL', ARGV[2])} 1 lua:cls hello b]

            lassign [gcmd_check_predict [gcmd_raw $fd EVAL $predict 1 lua:score SCORE 0.5]] action id
            assert_equal ABSTAIN $action
            assert_equal {int 1} [gcmd_raw $fd EVAL $learn 1 lua:score $id CORRECT 1]

            foreach name {GPAUSE GRESUME GRESET} {
                assert_equal {status OK} [gcmd_raw $fd EVAL \
                    "return redis.call('$name', KEYS\[1\])" 1 lua:cls]
            }

            # the other actions and the null
            lassign [gcmd_check_predict [gcmd_raw $fd EVAL $predict 1 tx:audit SCORE 0.9]] action id
            assert_equal AUDIT $action
            assert_equal {int 1} [gcmd_raw $fd EVAL $learn 1 tx:audit $id CORRECT 1]
            set typed [gcmd_raw $fd EVAL $predict 1 tx:full SCORE 0.5]
            assert_equal {ABSTAIN {}} [gcmd_check_predict $typed]
            assert_equal null [lindex [lindex $typed 1] 1]
            set accepts 0
            for {set i 0} {$i < 100 && $accepts < 3} {incr i} {
                set typed [gcmd_raw $fd EVAL $predict 1 tx:proven SCORE 0.9]
                lassign [gcmd_check_predict $typed] action arg
                if {$action eq "ACCEPT"} {
                    assert_equal null [lindex [lindex $typed 1] 1]
                    incr accepts
                } else {
                    assert_equal AUDIT $action
                    gcmd_raw $fd GLEARN tx:proven $arg CORRECT 1
                }
            }
            assert_equal 3 $accepts
        }

        gcmd_test "RESP$proto: GINFO from a script" {
            # A script speaks RESP2 to Redis unless it asks for RESP3: it
            # gets, and returns, the flat field/value array with numbers as
            # strings, whatever the protocol of the client.
            set fd2 [gcmd_raw_open 2]
            foreach key {lua:cls lua:score tx:proven} {
                set direct [gcmd_raw $fd2 GINFO $key]
                gcmd_check_info 2 $direct
                assert_equal $direct [gcmd_raw $fd EVAL {return redis.call('GINFO', KEYS[1])} 1 $key]
            }
            close $fd2

            # A script that asks for RESP3 gets the map.
            set script {
                redis.setresp(3)
                local i = redis.call('GINFO', KEYS[1])
                return {i.map.kind, i.map.state, tostring(i.map.alpha.double), i.map.generation,
                        #i.map.classes, #i.map.alphas, #i.map.thresholds, i.map.pending}
            }
            assert_equal {array {{bulk score} {bulk ACTIVE} {bulk 0.2} {int 1} {int 0} {int 1} {int 1} {int 0}}} \
                [gcmd_raw $fd EVAL $script 1 tx:proven]
            assert_equal {array {{bulk classifier} {bulk COLD} {bulk 0.1} {int 0} {int 2} {int 2} {int 0} {int 0}}} \
                [gcmd_raw $fd EVAL $script 1 lua:cls]

            # Returned by such a script, the map reaches a RESP3 client as a
            # map and a RESP2 client as its flat array. (Lua keeps no order
            # of fields and ends an array at its first null, hence a gate
            # that has its threshold.)
            set direct [gcmd_raw $fd GINFO tx:proven]
            set scripted [gcmd_raw $fd EVAL {redis.setresp(3); return redis.call('GINFO', KEYS[1])} 1 tx:proven]
            assert_equal [lindex $scripted 0] [expr {$proto == 3 ? "map" : "array"}]
            assert_equal [llength [lindex $scripted 1]] 46
            set direct [gcmd_typed_dict $direct]
            set scripted [gcmd_typed_dict $scripted]
            assert_equal [lsort [dict keys $scripted]] [lsort [dict keys $direct]]
            foreach field [dict keys $direct] {
                assert_equal [dict get $scripted $field] [dict get $direct $field] \
                    "GINFO $field through a script in RESP3 mode"
            }
        }

        gcmd_test "RESP$proto: a script can ask and answer in one step" {
            set script {
                local reply = redis.call('GPREDICT', KEYS[1], 'SCORE', ARGV[1])
                if reply[1] == 'ACCEPT' then return 'accepted' end
                if reply[2] then return redis.call('GLEARN', KEYS[1], reply[2], 'CORRECT', ARGV[2]) end
                return 'no feedback wanted'
            }
            set samples [gcmd_info lua:score calibration-samples]
            assert_equal {int 1} [gcmd_raw $fd EVAL $script 1 lua:score 0.5 1]
            assert_equal {int 1} [gcmd_raw $fd EVAL $script 1 lua:score 0.6 0]
            assert_equal {bulk {no feedback wanted}} [gcmd_raw $fd EVAL $script 1 tx:full 0.5 1]
            assert_equal [expr {$samples + 2}] [gcmd_info lua:score calibration-samples]
            assert_equal 0 [gcmd_info lua:score pending]
        }

        gcmd_test "RESP$proto: errors of gate commands in a script" {
            foreach {script key pattern} {
                {return redis.pcall('GPREDICT', KEYS[1], 'SCORE', '0.5')} lua:missing {ERR no such key*}
                {return redis.call('GPREDICT', KEYS[1], 'SCORE', '0.5')} lua:missing {ERR no such key*}
                {return redis.pcall('GINFO', KEYS[1])} lua:missing {ERR no such key*}
                {return redis.pcall('GPREDICT', KEYS[1], 'TEXT', 'hello')} lua:score {ERR *}
                {return redis.pcall('GPREDICT', KEYS[1], 'SCORE', '7')} lua:score {ERR *}
                {return redis.pcall('GLEARN', KEYS[1], 'x', 'LABEL', 'nosuch')} lua:cls {ERR *}
                {return redis.pcall('GADD', KEYS[1], 'TEXT', 'x', 'LABEL', 'a')} lua:score {ERR *}
                {return redis.pcall('GCREATE', KEYS[1], 'ALPHA', '0.1', 'SCORE')} lua:score {BUSYKEY*}
                {return redis.pcall('GINFO', KEYS[1])} tx:string {WRONGTYPE*}
                {return redis.pcall('GINFO')} lua:score {ERR *}
            } {
                gcmd_raw $fd SET tx:string value
                set reply [gcmd_raw $fd EVAL $script 1 $key]
                assert_equal [lindex $reply 0] error "script: $script"
                assert_match $pattern [lindex $reply 1]
            }
            # a script that caught the error goes on
            assert_equal {array {{bulk {ERR no such key}} {bulk score}}} [gcmd_raw $fd EVAL {
                local e = redis.pcall('GINFO', KEYS[1])
                local i = redis.call('GINFO', KEYS[2])
                return {e.err, i[2]}
            } 2 lua:missing lua:score]
        }

        gcmd_test "RESP$proto: a read-only script may run GINFO and no other gate command" {
            set reply [gcmd_raw $fd EVAL_RO {return redis.call('GINFO', KEYS[1])} 1 lua:score]
            assert_equal array [lindex $reply 0]
            assert_equal 46 [llength [lindex $reply 1]]
            set before [list [gcmd_state lua:score] [gcmd_state lua:cls]]
            foreach {script key} {
                {return redis.call('GPREDICT', KEYS[1], 'SCORE', '0.5')} lua:score
                {return redis.call('GPREDICT', KEYS[1], 'TEXT', 'hello')} lua:cls
                {return redis.call('GLEARN', KEYS[1], 'x', 'CORRECT', '1')} lua:score
                {return redis.call('GADD', KEYS[1], 'TEXT', 'x', 'LABEL', 'a')} lua:cls
                {return redis.call('GPAUSE', KEYS[1])} lua:score
                {return redis.call('GRESUME', KEYS[1])} lua:score
                {return redis.call('GRESET', KEYS[1])} lua:score
                {return redis.call('GCREATE', KEYS[1], 'ALPHA', '0.1', 'SCORE')} lua:ro:new
            } {
                set reply [gcmd_raw $fd EVAL_RO $script 1 $key]
                assert_equal [lindex $reply 0] error "script: $script"
                assert_match "*Write commands are not allowed*" [lindex $reply 1]
            }
            assert_equal $before [list [gcmd_state lua:score] [gcmd_state lua:cls]]
            assert_equal 0 [r exists lua:ro:new]
        }

        catch {close $fd}
    }

    gcmd_test {the server is still up (MULTI/EXEC and Lua)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 6. Many keys
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77004

    gcmd_test {many gates: commands on one key leave every other key alone} {
        set n 40
        for {set i 1} {$i <= $n} {incr i} {
            if {$i % 2 == 0} {
                r gcreate [gcmd_many_key $i] ALPHA [gcmd_many_alpha $i] SCORE
            } else {
                r gcreate [gcmd_many_key $i] ALPHA [gcmd_many_alpha $i] CLASSES 2 yes$i no$i
            }
        }
        # Interleaved: gate i is asked i times ...
        unset -nocomplain many_ids
        array set many_ids {}
        for {set round 1} {$round <= $n} {incr round} {
            for {set i $round} {$i <= $n} {incr i} {
                if {$i % 2 == 0} {
                    set reply [r gpredict [gcmd_many_key $i] SCORE 0.5]
                } else {
                    set reply [r gpredict [gcmd_many_key $i] TEXT "request $round for gate $i"]
                }
                assert_equal ABSTAIN [lindex $reply 0]
                lappend many_ids($i) [lindex $reply 1]
            }
        }
        # ... gets the answers of its first i/2 requests, a classifier is
        # taught i mod 4 examples, every fifth gate is paused.
        for {set i $n} {$i >= 1} {incr i -1} {
            set key [gcmd_many_key $i]
            foreach id [lrange $many_ids($i) 0 [expr {$i / 2 - 1}]] {
                assert_equal 1 [r glearn $key $id {*}[gcmd_many_answer $i]]
            }
            if {$i % 2} {
                for {set k 0} {$k < $i % 4} {incr k} {r gadd $key TEXT "example $k" LABEL no$i}
            }
            if {$i % 5 == 0} {r gpause $key}
        }

        for {set i 1} {$i <= $n} {incr i} {
            set info [r ginfo [gcmd_many_key $i]]
            set shown "gate $i"
            assert {[dict get $info alpha] == [gcmd_many_alpha $i]}
            assert_equal [dict get $info requests] $i $shown
            assert_equal [dict get $info abstained] $i $shown
            assert_equal [dict get $info accepted] 0 $shown
            assert_equal [dict get $info pending] [expr {$i - $i / 2}] $shown
            if {$i % 2 == 0} {
                assert_equal [dict get $info kind] score $shown
                assert_equal [dict get $info classes] {} $shown
                assert_equal [dict get $info learned] 0 $shown
                assert_equal [dict get $info calibration-samples] [expr {$i / 2}] $shown
                assert_equal [dict get $info state] [expr {$i % 5 ? "CALIBRATING" : "PAUSED"}] $shown
            } else {
                assert_equal [dict get $info kind] classifier $shown
                assert_equal [dict get $info classes] [list yes$i no$i] $shown
                assert_equal [dict get $info learned] [expr {$i / 2 + $i % 4}] $shown
                assert_equal [dict get $info state] [expr {$i % 5 ? "COLD" : "PAUSED"}] $shown
            }
        }

        # An id is good for the gate that issued it and for no other.
        for {set i 2} {$i <= $n} {incr i} {
            set id [lindex $many_ids($i) end]
            foreach j [list [expr {$i - 1}] [expr {$i % $n + 1}] [expr {($i + 1) % $n + 1}]] {
                if {$j == $i} continue
                assert_equal [r glearn [gcmd_many_key $j] $id {*}[gcmd_many_answer $j]] 0 \
                    "an id of gate $i sent to gate $j"
            }
        }
        for {set i 1} {$i <= $n} {incr i} {
            assert_equal [gcmd_info [gcmd_many_key $i] pending] [expr {$i - $i / 2}] "gate $i"
        }

        # Deleting, resetting, pausing and filling some leaves the rest as it is.
        set keep {}
        for {set i 1} {$i <= $n} {incr i} {
            if {$i % 4 == 3} {dict set keep $i [gcmd_state [gcmd_many_key $i]]}
        }
        for {set i 1} {$i <= $n} {incr i} {
            set key [gcmd_many_key $i]
            switch [expr {$i % 4}] {
                0 {assert_equal 1 [r del $key]}
                1 {assert_equal OK [r greset $key]}
                2 {
                    assert_equal OK [r gpause $key]
                    foreach id $many_ids($i) {r glearn $key $id {*}[gcmd_many_answer $i]}
                    r gpredict $key SCORE 0.9
                }
            }
        }
        dict for {i state} $keep {
            assert_equal [gcmd_state [gcmd_many_key $i]] $state "gate $i"
            # and its waiting requests are still its own
            assert_equal 1 [r glearn [gcmd_many_key $i] [lindex $many_ids($i) end] {*}[gcmd_many_answer $i]]
        }
        for {set i 1} {$i <= $n} {incr i} {
            if {$i % 4 == 0} {assert_equal 0 [r exists [gcmd_many_key $i]]}
            if {$i % 4 == 1} {assert_equal 0 [gcmd_info [gcmd_many_key $i] requests]}
        }
    }

    gcmd_test {many gates: one pipeline over many keys} {
        set n 30
        for {set i 0} {$i < $n} {incr i} {r gcreate pipe:$i ALPHA 0.1 SCORE}
        set rd [redis_deferring_client]
        set sent 0
        for {set round 0} {$round < 20} {incr round} {
            for {set i 0} {$i < $n} {incr i} {
                $rd gpredict pipe:$i SCORE 0.5
                incr sent
            }
        }
        unset -nocomplain pipe_ids
        array set pipe_ids {}
        for {set round 0} {$round < 20} {incr round} {
            for {set i 0} {$i < $n} {incr i} {
                set reply [$rd read]
                assert_equal ABSTAIN [lindex $reply 0]
                lappend pipe_ids($i) [lindex $reply 1]
            }
        }
        for {set i 0} {$i < $n} {incr i} {
            foreach id $pipe_ids($i) {$rd glearn pipe:$i $id CORRECT 1}
        }
        set used 0
        for {set k 0} {$k < $sent} {incr k} {incr used [$rd read]}
        $rd close
        assert_equal $sent $used
        for {set i 0} {$i < $n} {incr i} {
            assert_equal 20 [llength [lsort -unique $pipe_ids($i)]]
            set info [r ginfo pipe:$i]
            assert_equal 20 [dict get $info requests]
            assert_equal 0 [dict get $info pending]
            assert_equal 20 [dict get $info calibration-samples]
        }
    }

    gcmd_test {the server is still up (many keys)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 7. Settings at the edge of what GCREATE accepts
# ============================================================================
#
# Each of these is in a server of its own: they are the tests most likely to
# find arithmetic that does not hold at the edges.

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77005
    r config set gate.snapshot-examples 5

    gcmd_test {GCREATE: gates with extreme but valid settings can be used} {
        foreach {alpha delta audit} {
            1e-300 1e-300 1e-300
            4.9e-324 4.9e-324 4.9e-324
            0.9999999999999999 0.9999999999999999 1
            1e-9 0.5 0.5
            0.5 1e-9 1e-9
            0.999 0.999 0.999
        } {
            set shown "ALPHA $alpha DELTA $delta AUDIT $audit"
            r del ex:score ex:cls
            assert_equal OK [r gcreate ex:score ALPHA $alpha DELTA $delta AUDIT $audit SCORE] $shown
            assert_equal OK [r gcreate ex:cls ALPHA $alpha DELTA $delta AUDIT $audit CLASSES 2 spam ham] $shown
            foreach key {ex:score ex:cls} {
                set info [r ginfo $key]
                assert {[dict get $info alpha] == $alpha}
                assert {[dict get $info delta] == $delta}
                assert {[dict get $info audit] == $audit}
            }
            gcmd_teach ex:cls 10
            for {set i 0} {$i < 60} {incr i} {
                set reply [r gpredict ex:score SCORE 0.9]
                assert_equal [llength $reply] 2 $shown
                assert {[lindex $reply 0] in {ACCEPT ABSTAIN AUDIT}}
                if {[lindex $reply 1] ne {}} {
                    assert_equal [r glearn ex:score [lindex $reply 1] CORRECT 1] 1 $shown
                }
                lassign [gcmd_example $i] text label
                set reply [r gpredict ex:cls TEXT $text]
                assert_equal [llength $reply] 2 $shown
                assert {[lindex $reply 0] in {ACCEPT ABSTAIN AUDIT}}
                if {[lindex $reply 0] eq "ACCEPT"} {
                    assert {[lindex $reply 1] in {spam ham}}
                } elseif {[lindex $reply 1] ne {}} {
                    assert_equal [r glearn ex:cls [lindex $reply 1] LABEL $label] 1 $shown
                }
            }
            foreach key {ex:score ex:cls} {
                set info [r ginfo $key]
                assert_equal [dict get $info requests] 60 $shown
                assert_equal [dict get $info pending] 0 $shown
                assert_equal [dict get $info missing] 0 $shown
                assert {[dict get $info coverage] >= 0 && [dict get $info coverage] <= 1}
            }
        }
        assert_equal PONG [r ping]
    }
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77006

    gcmd_test {ALPHA just below 1: answers that agree with the teacher never revoke the gate} {
        # SPEC 3 "Getting worse": the drift detector fires when spot checks
        # show the gate got worse. Spot checks that all agree with the
        # teacher are not that, whatever ALPHA is.
        foreach alpha {0.9999999999999998 0.9999999999999999} {
            r del ex:high
            assert_equal OK [r gcreate ex:high ALPHA $alpha DELTA 0.5 AUDIT 1 SCORE]
            for {set i 0} {$i < 100} {incr i} {
                lassign [r gpredict ex:high SCORE 0.9] action id
                if {$id ne {}} {assert_equal 1 [r glearn ex:high $id CORRECT 1]}
            }
            set info [r ginfo ex:high]
            assert_equal [dict get $info revocations] 0 "ALPHA $alpha: revocations after 100 correct answers"
            assert_equal [dict get $info audit-errors] 0 "ALPHA $alpha"
            assert_equal [dict get $info state] ACTIVE "ALPHA $alpha"
        }
    }
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77007
    r config set gate.snapshot-examples 1

    gcmd_test {a tiny ALPHA: a snapshot is not dropped after one piece of proof} {
        # SPEC 3: "A classifier snapshot that has not taken over after a
        # generous amount of proof is dropped and the next one is tried."
        # Ten answers are not a generous amount for any ALPHA, least of all
        # for one that needs more proof than any other.
        foreach alpha {1e-3 1e-18 1e-19 1e-30 1e-300} {
            r del ex:tiny
            assert_equal OK [r gcreate ex:tiny ALPHA $alpha CLASSES 2 a b]
            r gadd ex:tiny TEXT "hello world" LABEL a
            assert_equal 1 [gcmd_info ex:tiny candidate]
            for {set i 0} {$i < 10} {incr i} {
                lassign [r gpredict ex:tiny TEXT "hello world"] action id
                assert_equal 1 [r glearn ex:tiny $id LABEL a]
            }
            set info [r ginfo ex:tiny]
            assert_equal [dict get $info candidate] 1 "ALPHA $alpha: snapshot being tested after 10 answers"
            assert_equal [dict get $info calibration-samples] 10 "ALPHA $alpha"
        }
    }
}

# ============================================================================
# 8. Random arguments
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77008
    r config set gate.snapshot-examples 5

    gcmd_test {random argument vectors: every reply is well formed and the server stays up} {
        expr {srand(20261128)}
        set fd [gcmd_raw_open 3]
        set words [list ALPHA DELTA AUDIT SCORE CLASSES ALWAYSDEFER CLASSALPHA TEXT FEATURES LABEL \
                       CORRECT alpha score text label features 0 1 2 3 64 65 -1 0.1 0.2 0.5 0.05 \
                       0.999 1.0 1e-5 nan inf 1e999 a b c {} { } x hello "hello world" \
                       18446744073709551615 4294967296 0.0 -0 +1 01 "a\x00b" \
                       [binary format H* ff] [binary format H* c285] [string repeat z 300]]
        set keys {rnd:1 rnd:2 rnd:3 rnd:4 rnd:string}
        gcmd_raw $fd SET rnd:string value
        set ids {}
        set seen {}
        for {set it 0} {$it < 8000} {incr it} {
            set key [gcmd_pick $keys]
            switch [gcmd_rnd 14] {
                0 {set cmd [list GCREATE $key {*}[gcmd_picks $words 0 12]]}
                1 {set cmd [list GCREATE $key ALPHA [gcmd_pick $words] CLASSES [gcmd_pick $words] \
                                {*}[gcmd_picks $words 0 6]]}
                2 {set cmd [list GCREATE $key ALPHA 0.2 SCORE {*}[gcmd_picks $words 0 [gcmd_rnd 3]]]}
                3 {set cmd [list GCREATE $key ALPHA 0.2 CLASSES 3 a b c {*}[gcmd_picks $words 0 [gcmd_rnd 3]]]}
                4 {set cmd [list GPREDICT $key {*}[gcmd_picks $words 0 8]]}
                5 {set cmd [list GPREDICT $key TEXT [gcmd_pick $words] {*}[gcmd_picks $words 0 [gcmd_rnd 6]]]}
                6 {set cmd [list GPREDICT $key TEXT [gcmd_pick $words] FEATURES [gcmd_pick $words] \
                                {*}[gcmd_picks $words 0 6]]}
                7 {set cmd [list GPREDICT $key SCORE [gcmd_pick $words]]}
                8 {set cmd [list GLEARN $key [gcmd_pick [concat $ids [list x]]] \
                                [gcmd_pick {LABEL CORRECT label BOGUS}] [gcmd_pick {a b c 0 1 2 {}}]]}
                9 {set cmd [list GLEARN $key {*}[gcmd_picks $words 0 5]]}
                10 {set cmd [list GADD $key TEXT [gcmd_pick $words] {*}[gcmd_picks $words 0 [gcmd_rnd 6]]]}
                11 {set cmd [list GADD $key TEXT [gcmd_pick $words] LABEL [gcmd_pick {a b c d}]]}
                12 {set cmd [list [gcmd_pick {GINFO GINFO GPAUSE GRESUME GRESET}] $key \
                                 {*}[gcmd_picks $words 0 [gcmd_rnd 2]]]}
                13 {set cmd [list [gcmd_pick {DEL GINFO}] [gcmd_pick {rnd:1 rnd:2 rnd:3 rnd:4}]]}
            }
            set shown "command $it: [string range $cmd 0 200]"
            set reply [gcmd_raw $fd {*}$cmd]
            set name [lindex $cmd 0]
            if {[lindex $reply 0] eq "error"} {
                set class [lindex [split [lindex $reply 1] " "] 0]
                if {$class ni {ERR BUSYKEY WRONGTYPE}} {
                    fail "$shown: unexpected error '[lindex $reply 1]'"
                }
                # an error reply is one line
                assert_no_match "*\n*" [lindex $reply 1]
                dict incr seen "$name $class"
                continue
            }
            dict incr seen "$name ok"
            switch $name {
                GCREATE - GPAUSE - GRESUME - GRESET {assert_equal $reply {status OK} $shown}
                GPREDICT {
                    lassign [gcmd_check_predict $reply] action arg
                    if {$action ne "ACCEPT" && $arg ne {}} {
                        set ids [lrange [linsert $ids end $arg] end-40 end]
                    }
                }
                GLEARN {
                    assert_equal [lindex $reply 0] int $shown
                    assert {[lindex $reply 1] in {0 1}}
                }
                GADD {
                    assert_equal [lindex $reply 0] int $shown
                    assert {[lindex $reply 1] >= 1}
                }
                GINFO {
                    set info [gcmd_check_info 3 $reply]
                    assert_equal [dict get $info requests] [expr {[dict get $info accepted] + \
                        [dict get $info audited] + [dict get $info abstained]}] $shown
                    assert {[dict get $info pending] <= 4096}
                    assert {[dict get $info kind] in {classifier score}}
                    assert {[dict get $info state] in {COLD CALIBRATING ACTIVE PAUSED}}
                    assert_equal [llength [dict get $info thresholds]] \
                        [llength [dict get $info alphas]] $shown
                }
                DEL {assert_equal [lindex $reply 0] int $shown}
            }
        }
        close $fd
        # Every kind of reply was met, so the loop was not all errors.
        foreach want {"GCREATE ok" "GCREATE ERR" "GCREATE BUSYKEY" "GPREDICT ok" "GPREDICT ERR"
                      "GPREDICT WRONGTYPE" "GLEARN ok" "GLEARN ERR" "GADD ok" "GADD ERR"
                      "GINFO ok" "GINFO ERR" "GPAUSE ok" "GRESUME ok" "GRESET ok" "DEL ok"} {
            assert {[dict exists $seen $want]}
        }
        assert_equal value [r get rnd:string]
        assert_equal PONG [r ping]
    }
}
