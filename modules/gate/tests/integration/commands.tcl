# Gate module, integration suite "commands": the command surface.
#
# Every test states the contract: SPEC.md (section 2 unless said otherwise)
# and, for the revised core, CHANGES.md. Where the contract is silent the
# test says so and asserts the most reasonable behaviour.
#
#    1. Syntax, replies and errors of the eight G* commands, RESP2 and RESP3
#    2. A gate is a key: TYPE, DEL, EXPIRE, RENAME, DUMP / RESTORE, COPY, ...
#    3. COMMAND, ACL, memory limits, and that clients cannot call _GATE.APPLY
#    4. Server settings
#    5. MULTI/EXEC, WATCH, client-side caching and Lua
#    6. Many keys
#    7. Settings at the edge of what GCREATE accepts
#    8. The fixed choices of SPEC.md section 5, as far as commands show them
#    9. Late and lost answers (real time)
#   10. Random arguments and random sessions
#   11. _GATE.APPLY called the way a replication link calls it, and the way
#       an append only file does
#
# Run from the root of a Redis tree (or of a copy of its test harness) whose
# modules/gate is the module under test:
#
#   ./runtest --single ../modules/gate/tests/integration/commands \
#             --baseport 12000 --portcount 1500 --clients 1
#
# DETERMINISM. The servers of this file run with gate.test-seed set (the one
# of section 4 sets it test by test), so the draws and the ids of the module
# are the same on every run, and the random choices of the tests come from
# generators with a fixed seed. Where an exact outcome is asserted it follows
# from the contract, not from one draw. A few tests need real time to pass
# (section 9, and one RESTORE test of section 2 that needs a lost answer):
# they measure how long they took and start over, at most twice, when the
# machine was too slow for the timing they need. The second test of random
# sessions (section 10) and the test that follows what the server sends on
# (section 11) sleep on purpose: which of their requests time out when
# depends on the clock, what they assert does not.
#
# This file is plain ASCII on purpose: byte strings that are not ASCII are
# built with [binary format H* ...] so that nothing depends on the encoding
# the file is read with.

set gatemodule [file normalize modules/gate/gate.so]
# (To run the file against another build of the module, as mutation testing
# does, name it in the environment.)
if {[info exists ::env(GATE_TEST_MODULE)] && $::env(GATE_TEST_MODULE) ne {}} {
    set gatemodule [file normalize $::env(GATE_TEST_MODULE)]
}

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

# Restart the seeded sequence of draws and ids. Redis does not call the
# setter of a setting whose value does not change, so the seed goes through 0.
# (Two gates created after the same seed get the same ids: every call in a
# server uses a seed of its own unless that is what the test wants.)
proc gcmd_seed {seed} {
    r config set gate.test-seed 0
    r config set gate.test-seed $seed
}

# 1 if DEBUG DIGEST-VALUE can tell two values apart in this run.
proc gcmd_has_digest {} {
    expr {[lsearch $::denytags "needs:debug"] < 0 && !$::ignoredigest}
}

# What one GADD teaches: the digest of the classifier 'key' after a GRESET
# and that one GADD. The seed is the same every time, so the reset gate is
# the same gate every time (its new ids start with the same number), and two
# examples leave the same digest exactly when the model learned the same
# from them. The key must be used for nothing else. Afterwards the server
# goes on with a seed no test has used, so that no two gates of a server
# ever hand out the same ids.
set ::gcmd_fresh_seed 900000
proc gcmd_effect {key args} {
    gcmd_seed 31337
    r greset $key
    r gadd $key {*}$args
    set digest [debug_digest_value $key]
    gcmd_seed [incr ::gcmd_fresh_seed]
    return $digest
}

# A gate must survive being saved and loaded as it is. DUMP it, RESTORE the
# payload under another name (the module checks every rule a gate must obey
# when it loads one) and compare the two values.
proc gcmd_check_saveable {key {detail {}}} {
    set payload [r dump $key]
    r del gcmd:copy
    if {[catch {r restore gcmd:copy 0 $payload} err]} {
        fail "the gate '$key' cannot be loaded from its own DUMP ($err) $detail"
    }
    set same [expr {[debug_digest_value $key] eq [debug_digest_value gcmd:copy]}]
    r del gcmd:copy
    if {!$same} {fail "the gate '$key' is another gate after DUMP and RESTORE $detail"}
}

# The DUMP payload of a key as it is, not compressed: the bytes of the value
# can be looked for in it.
proc gcmd_dump_plain {key} {
    r config set rdbcompression no
    set rc [catch {r dump $key} payload]
    r config set rdbcompression yes
    if {$rc} {error $payload}
    return $payload
}

# The checksum at the end of a DUMP payload: CRC-64 "Jones", the low bit
# first, as Redis computes it over everything before it. (A byte at a time,
# with the 256 remainders worked out once: a classifier is 100 KB.)
set ::gcmd_crc64_table {}
proc gcmd_crc64 {data} {
    if {$::gcmd_crc64_table eq {}} {
        for {set n 0} {$n < 256} {incr n} {
            set crc $n
            for {set i 0} {$i < 8} {incr i} {
                set crc [expr {$crc & 1 ? ($crc >> 1) ^ 0x95AC9329AC4BC9B5 : $crc >> 1}]
            }
            lappend ::gcmd_crc64_table $crc
        }
    }
    set table $::gcmd_crc64_table
    set crc 0
    binary scan $data cu* bytes
    foreach b $bytes {
        set crc [expr {[lindex $table [expr {($crc ^ $b) & 255}]] ^ ($crc >> 8)}]
    }
    return $crc
}

# SPEC 3 "Redis": a gate that was loaded (here: a RESTOREd copy of 'key')
# keeps its model, drops its proof and its waiting requests at its first
# write, and proves itself again. Until then GINFO shows it as that write
# will leave it. Checks both on the copy, with a write that does nothing by
# itself.
proc gcmd_check_recovery {key {detail {}}} {
    set was [r ginfo $key]
    set payload [r dump $key]
    r del gcmd:copy
    r restore gcmd:copy 0 $payload
    set shown [r ginfo gcmd:copy]
    if {[dict get $shown state] eq "PAUSED"} {r gpause gcmd:copy} else {r gresume gcmd:copy}
    set after [r ginfo gcmd:copy]
    r del gcmd:copy
    # (the bytes are those the copy holds, before and after)
    if {[dict remove $shown memory] ne [dict remove $after memory]} {
        fail "GINFO of a loaded copy of '$key' before its first write:\n$shown\nand after it:\n$after\n$detail"
    }
    foreach field {kind alpha delta audit classes alphas learned requests accepted audited abstained
                   missing revocations} {
        if {[dict get $after $field] ne [dict get $was $field]} {
            fail "a loaded copy of '$key' did not keep '$field': [dict get $after $field], was\
                [dict get $was $field] $detail"
        }
    }
    if {([dict get $after state] eq "PAUSED") != ([dict get $was state] eq "PAUSED")} {
        fail "a loaded copy of '$key' did not keep its pause $detail"
    }
    foreach field {generation pending audit-samples audit-errors calibration-samples} {
        if {[dict get $after $field] != 0} {
            fail "a loaded copy of '$key' kept '$field' [dict get $after $field] $detail"
        }
    }
    foreach threshold [dict get $after thresholds] {
        if {$threshold ne {}} {fail "a loaded copy of '$key' kept a threshold $detail"}
    }
    # the snapshot it will test has a number that was never used
    set candidate [dict get $after candidate]
    if {$candidate != 0 && $candidate <= max([dict get $was generation], [dict get $was candidate])} {
        fail "a loaded copy of '$key' tests snapshot $candidate again $detail"
    }
}

# 1 if 'cmd', run on the main connection, aborts a transaction of the
# connection 'w' that WATCHes 'key', else 0. The reply of the command, or
# its error, goes to the variable named 'replyvar'.
proc gcmd_aborts_watch {w key cmd {replyvar {}}} {
    $w watch $key
    set rc [catch {r {*}$cmd} reply]
    $w multi
    $w ping
    set done [$w exec]
    if {$replyvar ne {}} {
        upvar 1 $replyvar out
        set out $reply
    }
    expr {$done eq {} ? 1 : 0}
}

# Client-side caching. The key named by the next invalidation message read
# from 'inv': the connection invalidations are redirected to (RESP2) or the
# tracking connection itself, which gets them as pushes (RESP3).
proc gcmd_invalidated {inv proto} {
    set msg [$inv read]
    if {$proto == 2} {return [lindex $msg 2]}
    if {[lindex $msg 0] ne "invalidate"} {fail "expected an invalidation, got '$msg'"}
    lindex $msg 1
}

# Let the connection 'c' read (and so track) 'key' with GINFO, run 'cmd' on
# the main connection, and return 1 if that invalidated the key, else 0. The
# marker key 'marker', tracked too and changed afterwards, shows where the
# invalidations of 'cmd' end.
proc gcmd_invalidates {c inv proto key marker cmd} {
    catch {$c ginfo $key}
    $c get $marker
    catch {r {*}$cmd}
    r incr $marker
    set first [gcmd_invalidated $inv $proto]
    if {$first eq $marker} {return 0}
    if {$first ne $key} {fail "an invalidation of '$first' instead of '$key'"}
    set second [gcmd_invalidated $inv $proto]
    if {$second ne $marker} {fail "an invalidation of '$second' instead of '$marker'"}
    return 1
}

# Wait until the clock of the test shows 'deadline' (in milliseconds).
proc gcmd_sleep_until {deadline} {
    set left [expr {$deadline - [clock milliseconds]}]
    if {$left > 0} {after $left}
}

# SPEC 5, "betting test" and "proven": how many right answers prove a
# threshold of snapshot number 'generation' after 'errors' wrong ones. The
# wealth starts at 1 and is multiplied by 1 + c alpha / (1 - alpha) for a
# right guess and by 1 - c for a wrong one; the average over the four c
# must reach 32 x labels x g (g + 1) / DELTA, where 'labels' counts the
# labels the gate may answer (1 for a score gate).
proc gcmd_proof_needed {alpha delta {generation 1} {labels 1} {errors 0}} {
    set bar [expr {32.0 * $labels * $generation * ($generation + 1) / $delta}]
    for {set n 1} {$n <= 100000} {incr n} {
        set wealth 0.0
        foreach c {0.1 0.25 0.5 0.9} {
            set wealth [expr {$wealth + 0.25 * pow(1 - $c, $errors) *
                              pow(1 + $c * $alpha / (1 - $alpha), $n)}]
        }
        if {$wealth >= $bar} {return $n}
    }
    error "gcmd_proof_needed: no proof within 100000 right answers"
}

# SPEC 5, "thresholds per label": the 32 thresholds lie evenly spaced in
# log(1 - threshold) between 'lo' and 'hi'. Returns which of them 'value'
# is (0 to 31), or -1 if it is none.
proc gcmd_grid_index {lo hi value} {
    for {set t 0} {$t < 32} {incr t} {
        set threshold [expr {1 - (1 - $lo) * pow((1 - $hi) / (1 - $lo), $t / 31.0)}]
        if {abs($value - $threshold) < 1e-9} {return $t}
    }
    return -1
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

# The same in one pipeline; returns the reply of the last GADD.
proc gcmd_teach_fast {key n} {
    set rd [redis_deferring_client]
    for {set i 0} {$i < $n} {incr i} {
        lassign [gcmd_example $i] text label
        $rd gadd $key TEXT $text LABEL $label
    }
    set learned 0
    for {set i 0} {$i < $n} {incr i} {set learned [$rd read]}
    $rd close
    return $learned
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

# 'n' requests at 'score' to a score gate, each answered at once with
# 'correct' if the gate wants feedback. Returns the actions.
proc gcmd_ask_score {key n {score 0.9} {correct 1}} {
    set actions {}
    for {set i 0} {$i < $n} {incr i} {
        lassign [r gpredict $key SCORE $score] action id
        if {$id ne {}} {r glearn $key $id CORRECT $correct}
        lappend actions $action
    }
    return $actions
}

# Right answers at 'score' until the score gate has a threshold; returns how
# many it took. With nothing but right answers that threshold is 0: the
# gate answers everything.
proc gcmd_prove_all {key {score 0.9} {max 2000}} {
    for {set n 0} {$n < $max} {incr n} {
        if {[lindex [gcmd_info $key thresholds] 0] ne {}} {return $n}
        gcmd_ask_score $key 1 $score 1
    }
    fail "score gate $key proved nothing in $max right answers: [r ginfo $key]"
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

# rp:cls and rp:score as new gates, for the typed reply tests; lua:cls and
# lua:score for the script tests.
proc gcmd_typed_fresh {fd} {
    gcmd_raw $fd DEL rp:cls rp:score
    gcmd_raw $fd GCREATE rp:cls ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 b CLASSALPHA 1 c 0.05
    gcmd_raw $fd GCREATE rp:score ALPHA 0.1 SCORE
}

proc gcmd_lua_fresh {fd} {
    gcmd_raw $fd DEL lua:cls lua:score
    gcmd_raw $fd GCREATE lua:cls ALPHA 0.1 CLASSES 2 a b
    gcmd_raw $fd GCREATE lua:score ALPHA 0.1 SCORE
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
    # of this block is the same. Setting a seed again restarts the sequence
    # and would hand out the same ids twice: only gcmd_effect does it, and
    # leaves the server with a seed of its own afterwards.
    r config set gate.test-seed 20261128

    # ------------------------------------------------------------- GCREATE

    gcmd_test {GCREATE: a classifier gate; the reply is OK} {
        r del gc:cls
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
        r del gc:score
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
        r del gc:cls gc:score
        r gcreate gc:cls ALPHA 0.05 CLASSES 2 ignore respond
        r gcreate gc:score ALPHA 0.05 SCORE
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

    # The two gates the option order and option case tests compare with.
    catch {
        r gcreate gc:ref ALPHA 0.1 CLASSES 3 a b c AUDIT 0.25 DELTA 0.02 \
            ALWAYSDEFER 1 b CLASSALPHA 1 c 0.03
        r gcreate gc:ref:score ALPHA 0.3 SCORE AUDIT 1 DELTA 0.5
    }

    gcmd_test {GCREATE: every option is stored} {
        r del gc:all gc:all:score
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
        # the gates the next tests compare with are made the same way
        assert_equal [gcmd_config_of gc:all] [gcmd_config_of gc:ref]
        assert_equal [gcmd_config_of gc:all:score] [gcmd_config_of gc:ref:score]
    }

    gcmd_test {GCREATE: options may come in any order (classifier, all 720 orders)} {
        set groups {{ALPHA 0.1} {CLASSES 3 a b c} {AUDIT 0.25} {DELTA 0.02}
                    {ALWAYSDEFER 1 b} {CLASSALPHA 1 c 0.03}}
        set want [gcmd_config_of gc:ref]
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
        set want [gcmd_config_of gc:ref:score]
        foreach order [gcmd_permutations $groups] {
            set args [concat {*}$order]
            r del gc:order
            assert_equal OK [r gcreate gc:order {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:order] $want "GCREATE key $args"
        }
        r del gc:order
    }

    gcmd_test {GCREATE: option words are case-insensitive} {
        # "Option words (ALPHA, TEXT, LABEL, ...) are not case-sensitive;
        # labels and feature names are."
        set want [gcmd_config_of gc:ref]
        foreach args {
            {alpha 0.1 classes 3 a b c audit 0.25 delta 0.02 alwaysdefer 1 b classalpha 1 c 0.03}
            {Alpha 0.1 Classes 3 a b c Audit 0.25 Delta 0.02 AlwaysDefer 1 b ClassAlpha 1 c 0.03}
            {aLPHA 0.1 cLASSES 3 a b c aUDIT 0.25 dELTA 0.02 aLWAYSDEFER 1 b cLASSALPHA 1 c 0.03}
        } {
            r del gc:case
            assert_equal OK [r gcreate gc:case {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:case] $want "GCREATE key $args"
        }
        set want [gcmd_config_of gc:ref:score]
        foreach args {
            {alpha 0.3 score audit 1 delta 0.5}
            {Alpha 0.3 Score Audit 1 Delta 0.5}
        } {
            r del gc:case
            assert_equal OK [r gcreate gc:case {*}$args] "GCREATE key $args"
            assert_equal [gcmd_config_of gc:case] $want "GCREATE key $args"
        }
        r del gc:case
        # The labels keep the case they were given in, and a label in
        # another case is another label.
        assert_equal OK [r gcreate gc:case alpha 0.1 classes 3 Spam SPAM spam alwaysdefer 1 SPAM \
                             classalpha 1 spam 0.05]
        set info [r ginfo gc:case]
        assert_equal {Spam SPAM spam} [dict get $info classes]
        lassign [dict get $info alphas] a b c
        assert {$a == 0.1 && $b == 0 && $c == 0.05}
        r del gc:case
        gcmd_create_fails ALPHA 0.1 CLASSES 2 spam ham ALWAYSDEFER 1 Spam
        gcmd_create_fails ALPHA 0.1 CLASSES 2 spam ham CLASSALPHA 1 HAM 0.05
    }

    gcmd_test {GCREATE: labels may spell option words} {
        set words {ALPHA SCORE CLASSES AUDIT DELTA ALWAYSDEFER CLASSALPHA}
        r del gc:words gc:words2 gc:words3
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
        r del gc:bytes
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
        # "... each listed once; a label is in ALWAYSDEFER or in CLASSALPHA,
        # not both, and ALWAYSDEFER cannot list every label."
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
        foreach type {string list hash set zset stream volatile score gate} {r del gc:busy:$type}
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

    gcmd_test {errors: a label or a name that looks like a format is shown as it is, or not at all} {
        # An error is one line of text; what a client sent goes into it as
        # bytes, never as something to interpret.
        r del gc:fmt
        r gcreate gc:fmt ALPHA 0.1 CLASSES 2 %s%s%s%n %d
        assert_equal {%s%s%s%n %d} [gcmd_info gc:fmt classes]
        foreach odd {%s%s%s%s%n %n%n%n%n %99999999d %p%p%p%p {%1$s} %c%c 100% {%.*s} {{0}} {$(id)} {`id`}} {
            foreach cmd [list \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 2 a b ALWAYSDEFER 1 $odd] \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 2 a b CLASSALPHA 1 $odd 0.05] \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 3 $odd $odd b] \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 3 a $odd b CLASSALPHA 1 $odd 7] \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 3 a $odd b ALWAYSDEFER 2 $odd $odd] \
                [list gcreate gc:bad ALPHA 0.1 CLASSES 3 a $odd b ALWAYSDEFER 1 $odd CLASSALPHA 1 $odd 0.05] \
                [list gcreate gc:bad ALPHA 0.1 $odd] \
                [list gcreate gc:bad $odd 0.1 SCORE] \
                [list gpredict gc:fmt TEXT hello FEATURES 1 $odd nan] \
                [list gpredict gc:fmt TEXT hello FEATURES 2 $odd 1 $odd 2] \
                [list gpredict gc:fmt $odd hello] \
                [list gadd gc:fmt TEXT hello LABEL $odd] \
                [list gadd gc:fmt TEXT hello $odd a] \
                [list glearn gc:fmt some-id LABEL $odd] \
                [list glearn gc:fmt some-id $odd a] \
            ] {
                set rc [catch {r {*}$cmd} err]
                assert_equal $rc 1 "command: $cmd"
                assert_match "ERR *" $err
                assert_no_match "*\n*" $err
                assert {[string length $err] < 250}
                # whatever of the argument is shown is shown literally
                if {[string first % $err] >= 0 || [string first \$ $err] >= 0 || [string first ` $err] >= 0 ||
                    [string first \{ $err] >= 0} {
                    assert {[string first $odd $err] >= 0}
                }
            }
        }
        assert_equal 0 [r exists gc:bad]
        assert_equal 0 [gcmd_info gc:fmt learned]
        assert_equal PONG [r ping]
        r del gc:fmt
    }

    # ------------------------------------------------------------ GPREDICT

    # The two gates most GPREDICT tests ask. (Set-up that later tests rely on
    # is done here, between the tests, so that a test still finds it when it
    # is run alone with --only.)
    catch {
        r gcreate gp:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gp:score ALPHA 0.1 SCORE
    }

    gcmd_test {GPREDICT: the reply is the action and its argument} {
        r del gp:new:cls gp:new:score
        r gcreate gp:new:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gp:new:score ALPHA 0.1 SCORE
        foreach {key form answer} {gp:new:cls {TEXT {thanks everyone!}} {LABEL a}
                                   gp:new:score {SCORE 0.93} {CORRECT 1}} {
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
        r del gp:new:cls gp:new:score
    }

    gcmd_test {GPREDICT: a retried request is a new request} {
        foreach {key form a1 a2} {gp:cls {TEXT same} {LABEL a} {LABEL b}
                                  gp:score {SCORE 0.5} {CORRECT 1} {CORRECT 0}} {
            set requests [gcmd_info $key requests]
            set pending [gcmd_info $key pending]
            lassign [r gpredict $key {*}$form] action1 id1
            lassign [r gpredict $key {*}$form] action2 id2
            assert {$id1 ne {} && $id2 ne {}}
            assert {$id1 ne $id2}
            assert_equal [expr {$requests + 2}] [gcmd_info $key requests]
            assert_equal [expr {$pending + 2}] [gcmd_info $key pending]
            # Each one takes its own answer.
            assert_equal 1 [r glearn $key $id1 {*}$a1]
            assert_equal 1 [r glearn $key $id2 {*}$a2]
            assert_equal $pending [gcmd_info $key pending]
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
        # "Names follow the rule for labels and may not repeat."
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
        # "FEATURES optionally adds up to 64 named numbers"
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
        # 65 pairs are refused whatever the count says
        gcmd_fails_unchanged gp:cls "ERR *" \
            [list gpredict gp:cls TEXT hello FEATURES 64 {*}$features f64 0.5]
        gcmd_fails_unchanged gp:cls "ERR *" \
            [list gpredict gp:cls TEXT hello FEATURES 66 {*}$features f64 0.5]
    }

    gcmd_test {GPREDICT: FEATURES 0 is accepted and adds nothing} {
        # "up to 64 named numbers": none is up to 64. (The syntax line of
        # SPEC.md shows at least one pair; a client that builds the list
        # from what it has may well have nothing to list.)
        foreach form {{TEXT hello FEATURES 0} {text hello features 0} {TEXT {} FEATURES 0}} {
            lassign [r gpredict gp:cls {*}$form] action id
            assert_equal $action ABSTAIN "GPREDICT key $form"
            assert_equal [r glearn gp:cls $id LABEL a] 1 "GPREDICT key $form"
        }
        set learned [gcmd_info gp:cls learned]
        assert_equal [expr {$learned + 1}] [r gadd gp:cls TEXT hello FEATURES 0 LABEL a]
        # a count of 0 takes no pair
        gcmd_fails_unchanged gp:cls "ERR *" {gpredict gp:cls TEXT hello FEATURES 0 x 1}
        gcmd_fails_unchanged gp:cls "ERR *" {gadd gp:cls TEXT hello FEATURES 0 x 1 LABEL a}

        # The example teaches what the same text without FEATURES teaches.
        r del gp:effect
        r gcreate gp:effect ALPHA 0.1 CLASSES 2 a b
        set plain [gcmd_effect gp:effect TEXT hello LABEL a]
        assert_equal $plain [gcmd_effect gp:effect TEXT hello LABEL a]
        assert_equal $plain [gcmd_effect gp:effect TEXT hello FEATURES 0 LABEL a]
        # (the comparison can tell: another input teaches something else)
        if {[gcmd_has_digest]} {
            assert {$plain ne [gcmd_effect gp:effect TEXT hello FEATURES 1 x 1 LABEL a]}
            assert {$plain ne [gcmd_effect gp:effect TEXT hullo LABEL a]}
            assert {$plain ne [gcmd_effect gp:effect TEXT hello LABEL b]}
        }
        r del gp:effect
    }

    gcmd_test {GPREDICT: feature names follow the rule for labels} {
        # 1 to 64 bytes of UTF-8 without control characters
        set good [list \
            x 0 -1 {thread depth} {a,b;c} "it's" {"quoted"} TEXT FEATURES LABEL \
            [string repeat x 64] \
            [binary format H* e697a5e69cace8aa9e] \
            [binary format H* [string repeat f09f9880 16]] \
            [binary format H* c2a0] \
            [binary format H* f48fbfbf]]
        foreach name $good {
            binary scan $name H* hex
            lassign [r gpredict gp:cls TEXT hello FEATURES 1 $name 0.5] action id
            assert_equal $action ABSTAIN "feature name 0x$hex"
            assert_equal [r glearn gp:cls $id LABEL a] 1 "feature name 0x$hex"
            set learned [gcmd_info gp:cls learned]
            assert_equal [r gadd gp:cls TEXT hello FEATURES 2 first 1 $name -0.5 LABEL b] \
                [expr {$learned + 1}] "feature name 0x$hex"
        }
        set bad [list \
            {} \
            [string repeat x 65] \
            [binary format H* [string repeat f09f9880 17]] \
            "a\x00b" "a\x00" "\x00" "a\nb" "a\rb" "a\tb" "\x1b" "\x7f" "a\x7f" \
            [binary format H* c280] [binary format H* c29f] \
            [binary format H* ff] [binary format H* 80] [binary format H* c0af] \
            [binary format H* eda080] [binary format H* f4908080] [binary format H* e282]]
        foreach name $bad {
            gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls TEXT hello FEATURES 1 $name 0.5]
            gcmd_fails_unchanged gp:cls "ERR *" \
                [list gpredict gp:cls TEXT hello FEATURES 3 a 1 $name 0.5 c 1]
            gcmd_fails_unchanged gp:cls "ERR *" \
                [list gadd gp:cls TEXT hello FEATURES 1 $name 0.5 LABEL a]
        }
    }

    gcmd_test {GPREDICT: feature names are case-sensitive, the order of the features does not matter} {
        r del gp:effect
        r gcreate gp:effect ALPHA 0.1 CLASSES 2 a b
        set lower [gcmd_effect gp:effect TEXT hello FEATURES 1 depth 1 LABEL a]
        assert_equal $lower [gcmd_effect gp:effect TEXT hello features 1 depth 1 label a]
        if {[gcmd_has_digest]} {
            assert {$lower ne [gcmd_effect gp:effect TEXT hello FEATURES 1 Depth 1 LABEL a]}
            assert {$lower ne [gcmd_effect gp:effect TEXT hello FEATURES 1 DEPTH 1 LABEL a]}
        }
        # SPEC.md does not say that the order of the pairs is free; named
        # numbers that depended on their position would not be named.
        set ab [gcmd_effect gp:effect TEXT hello FEATURES 3 mentioned 1 depth 0.25 hour -0.5 LABEL a]
        assert_equal $ab [gcmd_effect gp:effect TEXT hello FEATURES 3 hour -0.5 mentioned 1 depth 0.25 LABEL a]
        assert_equal $ab [gcmd_effect gp:effect TEXT hello FEATURES 3 depth 0.25 hour -0.5 mentioned 1 LABEL a]
        if {[gcmd_has_digest]} {
            assert {$ab ne [gcmd_effect gp:effect TEXT hello FEATURES 3 mentioned 0.25 depth 1 hour -0.5 LABEL a]}
        }
        r del gp:effect
    }

    gcmd_test {GPREDICT: a feature value beyond 4 counts as 4} {
        r del gp:effect
        r gcreate gp:effect ALPHA 0.1 CLASSES 2 a b
        # (Compared with a second feature next to it. The named features
        # of a request are scaled down together when they are large, which
        # SPEC.md does not describe: one feature alone counts the same from
        # about 2.83 up, so that 3 and 4 could not be told apart.)
        foreach sign {{} -} {
            set four [gcmd_effect gp:effect TEXT hello FEATURES 2 x ${sign}4 y 1 LABEL a]
            foreach value {4.0 4.000001 4.5 5 64 1e9 1e300 17976931348623157e292} {
                assert_equal [gcmd_effect gp:effect TEXT hello FEATURES 2 x $sign$value y 1 LABEL a] \
                    $four "a feature value of $sign$value"
            }
            if {[gcmd_has_digest]} {
                # ... and what is not beyond 4 does not
                foreach value {3 2 1 0.5 0} {
                    assert {$four ne [gcmd_effect gp:effect TEXT hello FEATURES 2 x $sign$value y 1 LABEL a]}
                }
            }
        }
        set plus [gcmd_effect gp:effect TEXT hello FEATURES 2 x 1e300 y 1 LABEL a]
        set minus [gcmd_effect gp:effect TEXT hello FEATURES 2 x -1e300 y 1 LABEL a]
        if {[gcmd_has_digest]} {assert {$plus ne $minus}}
        # one feature alone
        set four [gcmd_effect gp:effect TEXT hello FEATURES 1 x 4 LABEL a]
        foreach value {5 1e300} {
            assert_equal [gcmd_effect gp:effect TEXT hello FEATURES 1 x $value LABEL a] $four \
                "a feature value of $value"
        }
        # GPREDICT takes what GADD takes
        foreach value {4 5 1e300 -1e300} {
            lassign [r gpredict gp:cls TEXT hello FEATURES 1 x $value] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn gp:cls $id LABEL a]
        }
        r del gp:effect
    }

    gcmd_test {GPREDICT and GADD: a TEXT of exactly 1 MiB is taken, one byte more is refused} {
        set mib [string repeat "sixteen bytes!! " 65536]
        assert_equal 1048576 [string length $mib]
        lassign [r gpredict gp:cls TEXT $mib] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gp:cls $id LABEL a]
        lassign [r gpredict gp:cls TEXT $mib FEATURES 2 x 1 y -1] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gp:cls $id LABEL b]
        set learned [gcmd_info gp:cls learned]
        assert_equal [expr {$learned + 1}] [r gadd gp:cls TEXT $mib LABEL a]
        assert_equal [expr {$learned + 2}] [r gadd gp:cls TEXT $mib FEATURES 1 x 1 LABEL b]
        assert_equal 0 [gcmd_info gp:cls pending]

        foreach over [list "${mib}x" "x${mib}" "${mib}${mib}" [string repeat "\x00" 1048577]] {
            set shown "TEXT of [string length $over] bytes"
            set before [gcmd_state gp:cls]
            assert_error "ERR *" {r gpredict gp:cls TEXT $over} $shown
            assert_error "ERR *" {r gpredict gp:cls TEXT $over FEATURES 1 x 1} $shown
            assert_error "ERR *" {r gadd gp:cls TEXT $over LABEL a} $shown
            assert_error "ERR *" {r gadd gp:cls TEXT $over FEATURES 1 x 1 LABEL a} $shown
            assert_equal [gcmd_state gp:cls] $before $shown
        }
        # the error is one line that does not carry the megabyte back
        catch {r gpredict gp:cls TEXT "${mib}x"} err
        assert {[string length $err] < 200}
    }

    gcmd_test {GPREDICT: only the first 4,096 bytes of a TEXT are read} {
        r del gp:effect
        r gcreate gp:effect ALPHA 0.1 CLASSES 2 a b
        set head [string range [string repeat "alpha beta gamma delta epsilon " 200] 0 4095]
        assert_equal 4096 [string length $head]
        set base [gcmd_effect gp:effect TEXT $head LABEL a]
        # whatever follows the first 4,096 bytes teaches nothing
        foreach tail [list " omega" "omega" "!" "\x00" " http://example.com/ ``` code" \
                          [string repeat " something else entirely" 1000] \
                          [string repeat z [expr {1048576 - 4096}]]] {
            assert_equal [gcmd_effect gp:effect TEXT "$head$tail" LABEL a] $base \
                "a TEXT of [string length "$head$tail"] bytes"
        }
        if {[gcmd_has_digest]} {
            # ... and every one of the 4,096 bytes can
            foreach at {0 2048 4095} {
                set other [string replace $head $at $at "!"]
                assert_equal 4096 [string length $other]
                assert {$base ne [gcmd_effect gp:effect TEXT $other LABEL a]}
            }
        }
        r del gp:effect
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

    gcmd_test {GPREDICT: a score a rounding error outside 0..1 is taken as the end of the range} {
        # "(A rounding error outside the range, such as 1.0000001, is taken
        # as the end of the range.)" That is what a cosine similarity
        # computed in floating point looks like, not a caller error.
        foreach score {1.0000001 1.00000001 1.0000000000000002 -0.0000001 -1e-12 -1e-300} {
            set reply [r gpredict gp:score SCORE $score]
            assert_equal [lindex $reply 0] ABSTAIN "SCORE $score"
            assert_equal [r glearn gp:score [lindex $reply 1] CORRECT 1] 1 "SCORE $score"
        }
        # Taken as 0, not as a score below 0: a gate that answers from 0 up
        # (every answer it ever got was right) answers it, as it answers 0.
        r del gp:zero
        r gcreate gp:zero ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_all gp:zero 0.5
        assert {[lindex [gcmd_info gp:zero thresholds] 0] == 0}
        foreach score {0 -0 0.0 -0.0000001 -1e-12 -1e-300 1 1.0000001 1.0000000000000002} {
            foreach action [gcmd_ask_score gp:zero 10 $score 1] {
                if {$action ni {ACCEPT AUDIT}} {
                    fail "SCORE $score to a gate that answers every score: $action"
                }
            }
        }
        r del gp:zero
        # What is further out is an error (the next test has more of them).
        foreach score {1.001 -0.001 1.01 -0.01} {
            gcmd_fails_unchanged gp:score "ERR *" [list gpredict gp:score SCORE $score]
        }
    }

    gcmd_test {numbers are written in decimal, with or without an exponent} {
        # "Numbers are written in decimal, with or without an exponent."
        r del gc:num gc:num:cls
        r gcreate gc:num:cls ALPHA 0.1 CLASSES 2 a b
        # every usual way to write one half ...
        foreach half {0.5 0.50 +0.5 5e-1 5E-1 5e-01 0.5e0 0.5E+0 50e-2 0.05e1 0.005E+2 500000e-6} {
            r del gc:num
            assert_equal [r gcreate gc:num ALPHA $half DELTA $half AUDIT $half SCORE] OK "the number $half"
            set info [r ginfo gc:num]
            assert {[dict get $info alpha] == 0.5 && [dict get $info delta] == 0.5 && [dict get $info audit] == 0.5}
            lassign [r gpredict gc:num SCORE $half] action id
            assert_equal $action ABSTAIN "SCORE $half"
            assert_equal 1 [r glearn gc:num $id CORRECT 1]
            lassign [r gpredict gc:num:cls TEXT hello FEATURES 1 x $half] action id
            assert_equal $action ABSTAIN "a feature value of $half"
            assert_equal 1 [r glearn gc:num:cls $id LABEL a]
            r del gc:num
            assert_equal [r gcreate gc:num ALPHA 0.5 CLASSES 2 a b CLASSALPHA 1 a $half] OK \
                "CLASSALPHA $half"
            assert {[lindex [gcmd_info gc:num alphas] 0] == 0.5}
        }
        # ... and of the ends of a range
        foreach one {1 1.0 1.000 1e0 1E0 +1 10e-1 0.1e1 1.0E+00} {
            r del gc:num
            assert_equal [r gcreate gc:num ALPHA 0.5 AUDIT $one SCORE] OK "AUDIT $one"
            assert {[gcmd_info gc:num audit] == 1}
            lassign [r gpredict gc:num SCORE $one] action id
            assert_equal $action ABSTAIN "SCORE $one"
            assert_equal 1 [r glearn gc:num $id CORRECT 1]
        }
        foreach zero {0 0.0 0.000 0e0 +0 -0 -0.0 0e5 0.0e-7 0E+00} {
            lassign [r gpredict gc:num SCORE $zero] action id
            assert_equal $action ABSTAIN "SCORE $zero"
            assert_equal 1 [r glearn gc:num $id CORRECT 0]
        }
        # What is not a decimal number is not a number: other bases, words,
        # separators, space around it, a second number.
        set before [list [gcmd_state gc:num] [gcmd_state gc:num:cls]]
        foreach bad [list 0x1p-1 0X1P-1 0x0.8 0x8 0b1 0o7 1/2 50% inf +inf -inf Inf INF infinity \
                         nan NaN NAN -nan nan(1) 1e 1e+ 1e- e1 .e1 . + - +-0.5 --0.5 0.5.0 0..5 \
                         0,5 1_0 1'000 0.5f 0.5d 0.5L 5e-1.0 0.5e0e0 1d0 {0.5 } { 0.5} "\t0.5" \
                         "0.5\n" "0.5\r\n" "0.5\x00" "\x000.5" "0.5\x000" {0.5 0.5} {} \
                         [binary format H* d9a02ed9a5] [binary format H* efbc90efbc8eefbc95] \
                         [binary format H* c2bd]] {
            binary scan $bad H* hex
            set shown "the number 0x$hex"
            assert_error "ERR *" {r gcreate gc:bad ALPHA $bad SCORE} $shown
            assert_error "ERR *" {r gcreate gc:bad ALPHA 0.5 DELTA $bad SCORE} $shown
            assert_error "ERR *" {r gcreate gc:bad ALPHA 0.5 AUDIT $bad SCORE} $shown
            assert_error "ERR *" {r gcreate gc:bad ALPHA 0.5 CLASSES 2 a b CLASSALPHA 1 a $bad} $shown
            assert_error "ERR *" {r gpredict gc:num SCORE $bad} $shown
            assert_error "ERR *" {r gpredict gc:num:cls TEXT hello FEATURES 1 x $bad} $shown
            assert_error "ERR *" {r gadd gc:num:cls TEXT hello FEATURES 1 x $bad LABEL a} $shown
        }
        assert_equal 0 [r exists gc:bad]
        assert_equal $before [list [gcmd_state gc:num] [gcmd_state gc:num:cls]]
        r del gc:num gc:num:cls
    }

    gcmd_test {counts are plain decimal integers} {
        # SPEC.md does not say how a count is written. A count that is not
        # a whole number, has a sign or an exponent, or is in another
        # base cannot say how many arguments follow.
        r del gc:num
        foreach count {2.0 2. 2e0 +2 -2 0x2 0b10 { 2} {2 } "2\x00" two {} 2,0 1+1} {
            binary scan $count H* hex
            set shown "the count 0x$hex"
            assert_error "ERR *" {r gcreate gc:num ALPHA 0.1 CLASSES $count a b} $shown
            assert_error "ERR *" {r gcreate gc:num ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER $count a b} $shown
            assert_error "ERR *" {r gcreate gc:num ALPHA 0.1 CLASSES 3 a b c CLASSALPHA $count a 0.05 b 0.05} $shown
            gcmd_fails_unchanged gp:cls "ERR *" [list gpredict gp:cls TEXT hello FEATURES $count x 1 y 2]
            gcmd_fails_unchanged gp:cls "ERR *" [list gadd gp:cls TEXT hello FEATURES $count x 1 y 2 LABEL a]
        }
        assert_equal 0 [r exists gc:num]
    }

    gcmd_test {numbers: a numeral of 1,099 bytes is read, one of 1,100 bytes is refused} {
        # CHANGES 6: numerals of up to 1,099 bytes are numbers, so that the
        # exact decimal expansion of any double is one.
        set fits 0.5[string repeat 0 1096]
        set minus -0.5[string repeat 0 1095]
        set long 0.5[string repeat 0 1097]
        assert_equal 1099 [string length $fits]
        assert_equal 1099 [string length $minus]
        assert_equal 1100 [string length $long]
        r del gc:digits gc:digits:cls
        assert_equal OK [r gcreate gc:digits ALPHA $fits DELTA $fits AUDIT $fits SCORE]
        set info [r ginfo gc:digits]
        assert {[dict get $info alpha] == 0.5 && [dict get $info delta] == 0.5 && [dict get $info audit] == 0.5}
        assert_equal OK [r gcreate gc:digits:cls ALPHA 0.5 CLASSES 2 a b CLASSALPHA 1 b $fits]
        assert {[lindex [gcmd_info gc:digits:cls alphas] 1] == 0.5}
        lassign [r gpredict gc:digits SCORE $fits] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gc:digits $id CORRECT 1]
        lassign [r gpredict gc:digits:cls TEXT hello FEATURES 1 x $fits] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gc:digits:cls $id LABEL a]
        assert_equal 2 [r gadd gc:digits:cls TEXT hello FEATURES 1 x $minus LABEL a]
        # it is the number, not just a numeral that was let through
        r del gp:effect
        r gcreate gp:effect ALPHA 0.1 CLASSES 2 a b
        assert_equal [gcmd_effect gp:effect TEXT hello FEATURES 1 x 0.5 LABEL a] \
            [gcmd_effect gp:effect TEXT hello FEATURES 1 x $fits LABEL a]
        assert_equal [gcmd_effect gp:effect TEXT hello FEATURES 1 x -0.5 LABEL a] \
            [gcmd_effect gp:effect TEXT hello FEATURES 1 x $minus LABEL a]
        r del gp:effect

        foreach args [list [list ALPHA $long SCORE] [list ALPHA 0.5 DELTA $long SCORE] \
                           [list ALPHA 0.5 AUDIT $long SCORE] \
                           [list ALPHA 0.5 CLASSES 2 a b CLASSALPHA 1 b $long]] {
            gcmd_create_fails {*}$args
        }
        gcmd_fails_unchanged gc:digits "ERR *" [list gpredict gc:digits SCORE $long]
        gcmd_fails_unchanged gc:digits:cls "ERR *" \
            [list gpredict gc:digits:cls TEXT hello FEATURES 1 x $long]
        gcmd_fails_unchanged gc:digits:cls "ERR *" \
            [list gadd gc:digits:cls TEXT hello FEATURES 1 x $long LABEL a]
        # a megabyte of digits is refused as well
        gcmd_fails_unchanged gc:digits "ERR *" [list gpredict gc:digits SCORE 0.[string repeat 5 1048576]]

        # The exact decimal expansions of the smallest double (2^-1074:
        # 1,076 bytes) and of the largest (309 digits).
        set tiny "0.[string repeat 0 323][expr {5 ** 1074}]"
        set huge [expr {(2 ** 53 - 1) * 2 ** 971}]
        assert_equal 1076 [string length $tiny]
        assert_equal 309 [string length $huge]
        r del gc:digits
        assert_equal OK [r gcreate gc:digits ALPHA $tiny DELTA $tiny AUDIT $tiny SCORE]
        set info [r ginfo gc:digits]
        assert {[dict get $info alpha] == 4.9406564584124654e-324}
        assert {[dict get $info delta] == 4.9406564584124654e-324}
        assert {[dict get $info audit] == 4.9406564584124654e-324}
        lassign [r gpredict gc:digits SCORE $tiny] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gc:digits $id CORRECT 1]
        foreach value [list $tiny -$tiny $huge -$huge] {
            lassign [r gpredict gc:digits:cls TEXT hello FEATURES 1 x $value] action id
            assert_equal $action ABSTAIN "a feature value of [string length $value] bytes"
            assert_equal 1 [r glearn gc:digits:cls $id LABEL b]
        }
        # one more digit than the largest double is not a finite number
        gcmd_fails_unchanged gc:digits:cls "ERR *" \
            [list gpredict gc:digits:cls TEXT hello FEATURES 1 x ${huge}0]
        r del gc:digits gc:digits:cls
    }

    gcmd_test {numbers written with many digits are numbers} {
        # These are the exact decimal expansions of the doubles 0.1 and
        # 0.0001 (what an arbitrary precision decimal type prints: 57 and
        # 68 bytes) and 0.5 printed with 70 decimals.
        set tenth 0.1000000000000000055511151231257827021181583404541015625
        set small 0.000100000000000000004792173602385929598312941379845142364501953125
        set padded 0.5[string repeat 0 69]
        r del gc:digits gc:digits:cls
        r gcreate gc:digits ALPHA 0.1 SCORE
        r gcreate gc:digits:cls ALPHA 0.1 CLASSES 2 a b
        foreach score [list $tenth $small $padded] {
            set shown "SCORE of [string length $score] bytes"
            set rc [catch {r gpredict gc:digits SCORE $score} reply]
            assert_equal $rc 0 "$shown: $reply"
            assert_equal [lindex $reply 0] ABSTAIN $shown
            assert_equal [r glearn gc:digits [lindex $reply 1] CORRECT 1] 1 $shown
        }
        foreach value [list $tenth $small $padded -$small] {
            set shown "feature value of [string length $value] bytes"
            set rc [catch {r gpredict gc:digits:cls TEXT hello FEATURES 1 x $value} reply]
            assert_equal $rc 0 "$shown: $reply"
            assert_equal [r glearn gc:digits:cls [lindex $reply 1] LABEL a] 1 $shown
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
        r del gc:digits gc:digits:cls
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
        r del gp:full
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

    gcmd_test {GPREDICT: requests that got their GLEARN do not keep a gate full} {
        # SPEC 3 "Full": a gate holds at most 4,096 requests waiting for
        # feedback. A request whose GLEARN replied 1 has its feedback, also
        # while its turn as proof has not come because an older request is
        # still unanswered.
        r del gp:answered
        # (with the longest feedback timeout, so that proof waits for the
        # oldest answer however long this test takes)
        r config set gate.feedback-timeout 604800000
        r gcreate gp:answered ALPHA 0.2 SCORE
        r config set gate.feedback-timeout 60000
        set replies [gcmd_pipeline_predict gp:answered 4096 0.5]
        assert_equal {ABSTAIN {}} [r gpredict gp:answered SCORE 0.5]
        # every answer arrives but that of the oldest request
        set rd [redis_deferring_client]
        foreach reply [lrange $replies 1 end] {$rd glearn gp:answered [lindex $reply 1] CORRECT 1}
        set used 0
        for {set i 1} {$i < 4096} {incr i} {incr used [$rd read]}
        $rd close
        assert_equal 4095 $used
        # One request waits for feedback (GINFO pending); the answers of
        # the others wait for their turn as proof (CHANGES 2).
        set info [r ginfo gp:answered]
        assert_equal 1 [dict get $info pending]
        assert_equal 0 [dict get $info calibration-samples]
        # the gate takes new requests
        set reply [r gpredict gp:answered SCORE 0.5]
        assert_equal ABSTAIN [lindex $reply 0]
        set pending [gcmd_info gp:answered pending]
        if {[lindex $reply 1] eq {}} {
            fail "4,095 of 4,096 requests have their GLEARN, yet the gate is full (pending $pending)"
        }
        assert_equal 2 $pending
        # ... 4,094 more of them, and is full again
        set more [gcmd_pipeline_predict gp:answered 4096 0.5]
        for {set i 0} {$i < 4096} {incr i} {
            if {$i < 4094} {
                assert {[lindex [lindex $more $i] 1] ne {}}
            } else {
                assert_equal [lindex $more $i] {ABSTAIN {}} "request [expr {$i + 1}]"
            }
        }
        assert_equal 4096 [gcmd_info gp:answered pending]
        # The oldest answer arrives: every answer that waited for it counts.
        assert_equal 1 [r glearn gp:answered [lindex [lindex $replies 0] 1] CORRECT 1]
        set info [r ginfo gp:answered]
        assert_equal 4096 [dict get $info calibration-samples]
        assert_equal 4095 [dict get $info pending]
        gcmd_check_saveable gp:answered
        r del gp:answered
    }

    gcmd_test {GPREDICT: proof stops waiting for one answer when more than 16,384 answers wait behind it} {
        # CHANGES 2: proof is counted in request order, so an answer waits
        # for the answers of older requests; when more than 16,384 answered
        # requests wait behind an unanswered one, proof moves on without it
        # and counts it as a wrong guess. It stays a waiting request with a
        # valid id until its time is up.
        r del gp:waiting
        # (with the longest feedback timeout: it is the number of answers
        # that makes proof move on here, not the time they took)
        r config set gate.feedback-timeout 604800000
        r gcreate gp:waiting ALPHA 0.2 DELTA 0.2 SCORE
        r config set gate.feedback-timeout 60000
        set first [lindex [r gpredict gp:waiting SCORE 0.9] 1]
        set rd [redis_deferring_client]
        set used 0
        for {set chunk 0} {$chunk < 16} {incr chunk} {
            for {set i 0} {$i < 1024} {incr i} {$rd gpredict gp:waiting SCORE 0.9}
            set ids {}
            for {set i 0} {$i < 1024} {incr i} {lappend ids [lindex [$rd read] 1]}
            foreach id $ids {$rd glearn gp:waiting $id CORRECT 1}
            for {set i 0} {$i < 1024} {incr i} {incr used [$rd read]}
        }
        $rd close
        assert_equal 16384 $used
        # 16,384 answers wait for the one that is missing: nothing counts yet
        set info [r ginfo gp:waiting]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal 1 [dict get $info pending]
        assert_equal 16385 [dict get $info requests]
        set held [dict get $info memory]
        gcmd_check_saveable gp:waiting
        # one more answer
        lassign [r gpredict gp:waiting SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn gp:waiting $id CORRECT 1]
        set info [r ginfo gp:waiting]
        assert_equal 16386 [dict get $info calibration-samples]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info pending]
        assert_equal 0 [dict get $info missing]
        # the answers that waited are no longer held
        assert {[dict get $info memory] < $held / 10}
        # The late answer is still taken, once.
        assert_equal 1 [r glearn gp:waiting $first CORRECT 1]
        assert_equal 0 [r glearn gp:waiting $first CORRECT 1]
        set info [r ginfo gp:waiting]
        assert_equal 0 [dict get $info pending]
        assert_equal 16386 [dict get $info calibration-samples]
        assert_equal 0 [dict get $info missing]
        gcmd_check_saveable gp:waiting
        r del gp:waiting
    }

    gcmd_test {GPREDICT: a classifier keeps 3,072 requests for training only, 4,096 as proof} {
        # SPEC 5, "waiting requests: 4,096 (3,072 while they are only wanted
        # as training examples ...)". A classifier that tests nothing wants
        # its requests as examples only.
        r del gp:full:cls
        gcmd_create_fast gp:full:cls ALPHA 0.2 CLASSES 2 spam ham
        set rd [redis_deferring_client]
        for {set i 0} {$i < 3100} {incr i} {$rd gpredict gp:full:cls TEXT "request $i"}
        set ids {}
        for {set i 0} {$i < 3100} {incr i} {
            set reply [$rd read]
            assert_equal 2 [llength $reply]
            assert_equal ABSTAIN [lindex $reply 0]
            # the first 3,072 get an id, the others none
            if {$i < 3072} {
                assert {[lindex $reply 1] ne {}}
                lappend ids [lindex $reply 1]
            } else {
                assert_equal [lindex $reply 1] {} "request [expr {$i + 1}] of a classifier that tests nothing"
            }
        }
        set info [r ginfo gp:full:cls]
        assert_equal COLD [dict get $info state]
        assert_equal 3072 [dict get $info pending]
        assert_equal 3100 [dict get $info requests]
        assert_equal 3100 [dict get $info abstained]
        # one answer makes room for one request
        assert_equal 1 [r glearn gp:full:cls [lindex $ids 0] LABEL spam]
        assert {[lindex [r gpredict gp:full:cls TEXT again] 1] ne {}}
        assert_equal {ABSTAIN {}} [r gpredict gp:full:cls TEXT again]

        # With a snapshot to test, a request is proof and is kept whatever
        # waits already, up to 4,096.
        gcmd_teach gp:full:cls 40
        assert_equal CALIBRATING [gcmd_info gp:full:cls state]
        for {set i 0} {$i < 1100} {incr i} {$rd gpredict gp:full:cls TEXT [lindex [gcmd_example $i] 0]}
        for {set i 0} {$i < 1100} {incr i} {
            set reply [$rd read]
            assert_equal ABSTAIN [lindex $reply 0]
            if {$i < 1024} {
                assert {[lindex $reply 1] ne {}}
            } else {
                assert_equal [lindex $reply 1] {} "request [expr {$i + 1}] behind 3,072 waiting ones"
            }
        }
        $rd close
        set info [r ginfo gp:full:cls]
        assert_equal 4096 [dict get $info pending]
        assert_equal 4202 [dict get $info requests]
        assert_equal 4202 [dict get $info abstained]
        r del gp:full:cls
    }

    gcmd_test {GPREDICT: a classifier keeps 16 MiB of clues for waiting requests} {
        # SPEC 5, "waiting requests: ... 16 MiB of stored clues". A long
        # message with many different words has many clues.
        r del gp:bytes
        r gcreate gp:bytes ALPHA 0.2 CLASSES 2 a b
        set words {}
        for {set i 0} {$i < 1024} {incr i} {lappend words [format %03x $i]}
        set text [join $words " "]
        set empty [gcmd_info gp:bytes memory]
        set first [lindex [r gpredict gp:bytes TEXT $text] 1]
        assert {$first ne {}}
        # what one waiting request takes
        set each [expr {[gcmd_info gp:bytes memory] - $empty}]
        assert {$each > 8192 && $each < 65536}
        # The gate is full when the requests that wait hold 16 MiB: the
        # request that takes them there is the last to get an id.
        set room [expr {int(ceil(16777216.0 / $each))}]
        assert {$room < 3072}
        set rd [redis_deferring_client]
        for {set i 1} {$i < $room + 20} {incr i} {$rd gpredict gp:bytes TEXT $text}
        set ids [list $first]
        for {set i 1} {$i < $room + 20} {incr i} {
            set reply [$rd read]
            assert_equal ABSTAIN [lindex $reply 0]
            if {$i < $room} {
                assert {[lindex $reply 1] ne {}}
                lappend ids [lindex $reply 1]
            } else {
                assert_equal [lindex $reply 1] {} "request [expr {$i + 1}] when $room requests fill 16 MiB"
            }
        }
        $rd close
        set info [r ginfo gp:bytes]
        assert_equal $room [dict get $info pending]
        assert {[dict get $info memory] - $empty >= 16777216}
        assert {[dict get $info memory] - $empty < 16777216 + $each}
        # a short request gets no id either: the gate is full, whatever is asked
        assert_equal {ABSTAIN {}} [r gpredict gp:bytes TEXT hi]
        # An answer frees the clues of its request, and a request is taken again.
        assert_equal 1 [r glearn gp:bytes [lindex $ids 7] LABEL a]
        assert {[lindex [r gpredict gp:bytes TEXT $text] 1] ne {}}
        assert_equal {ABSTAIN {}} [r gpredict gp:bytes TEXT $text]
        r del gp:bytes
    }

    # -------------------------------------------------------------- GLEARN

    catch {
        r gcreate gl:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:score ALPHA 0.1 SCORE
    }

    gcmd_test {GLEARN: 1 when the feedback is used, 0 when it is sent again} {
        r del gl:new:cls gl:new:score
        r gcreate gl:new:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gl:new:score ALPHA 0.1 SCORE
        lassign [r gpredict gl:new:cls TEXT hello] action id
        assert_equal 1 [r glearn gl:new:cls $id LABEL a]
        assert_equal 0 [r glearn gl:new:cls $id LABEL a]
        # A retry with another answer is not used either.
        assert_equal 0 [r glearn gl:new:cls $id LABEL b]
        assert_equal 1 [gcmd_info gl:new:cls learned]
        assert_equal 0 [gcmd_info gl:new:cls pending]

        lassign [r gpredict gl:new:score SCORE 0.7] action id
        assert_equal 1 [r glearn gl:new:score $id CORRECT 1]
        assert_equal 0 [r glearn gl:new:score $id CORRECT 1]
        assert_equal 0 [r glearn gl:new:score $id CORRECT 0]
        assert_equal 0 [gcmd_info gl:new:score pending]
        assert_equal 1 [gcmd_info gl:new:score calibration-samples]
        r del gl:new:cls gl:new:score
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
        r del gl:ids gl:ids:score
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
        r del gl:x1 gl:x2 gl:x3 gl:x4
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
        set pending [gcmd_info gl:cls pending]
        foreach label [list nosuch A B {} {a } { a} ab "a\x00" "a\x00b" 0 1 [string repeat a 65] \
                           [string repeat a 100000]] {
            gcmd_fails_unchanged gl:cls "ERR *" [list glearn gl:cls $id LABEL $label]
        }
        assert_equal $pending [gcmd_info gl:cls pending]
        assert_equal 1 [r glearn gl:cls $id LABEL a]
        assert_equal [expr {$pending - 1}] [gcmd_info gl:cls pending]
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
        set pending [gcmd_info gl:score pending]
        foreach value [list 2 -1 10 11 yes no true false {} { 1} {1 } 0.5 "1\x00" "0\x000" a] {
            gcmd_fails_unchanged gl:score "ERR *" [list glearn gl:score $id CORRECT $value]
        }
        assert_equal $pending [gcmd_info gl:score pending]
        assert_equal 1 [r glearn gl:score $id CORRECT 0]
        assert_equal 0 [r glearn gl:score $id CORRECT 0]
    }

    # ---------------------------------------------------------------- GADD

    gcmd_test {GADD: the reply counts the examples the gate has learned from} {
        r del ga:cls
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
        r del ga:score
        r gcreate ga:score ALPHA 0.1 SCORE
        foreach form {{TEXT hello LABEL a} {TEXT hello LABEL 1} {TEXT hello CORRECT 1}
                      {TEXT hello FEATURES 1 x 1 LABEL a} {SCORE 0.5 CORRECT 1} {SCORE 0.5 LABEL a}} {
            gcmd_fails_unchanged ga:score "ERR *" [list gadd ga:score {*}$form]
        }
        assert_equal 0 [gcmd_info ga:score learned]
    }

    gcmd_test {GADD: a bad label or bad syntax is an error and teaches nothing} {
        r del ga:bad
        r gcreate ga:bad ALPHA 0.1 CLASSES 2 a b
        assert_equal 1 [r gadd ga:bad TEXT hello LABEL b]
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
            gcmd_fails_unchanged ga:bad "ERR *" [list gadd ga:bad {*}$form]
        }
        gcmd_fails_unchanged ga:bad "ERR *" [list gadd ga:bad TEXT hello LABEL "a\x00"]
        gcmd_fails_unchanged ga:bad "ERR *" [list gadd ga:bad TEXT hello "LABEL\x00" a]
        assert_equal 2 [r gadd ga:bad TEXT hello LABEL a]
        r del ga:bad
    }

    gcmd_test {GADD: a label that is always deferred can still be taught} {
        r del ga:defer
        r gcreate ga:defer ALPHA 0.1 CLASSES 3 a b c ALWAYSDEFER 1 b
        assert_equal 1 [r gadd ga:defer TEXT hello LABEL b]
        lassign [r gpredict ga:defer TEXT hello] action id
        assert_equal 1 [r glearn ga:defer $id LABEL b]
        assert_equal 2 [gcmd_info ga:defer learned]
        r del ga:defer
    }

    # --------------------------------------------------------------- GINFO

    gcmd_test {GINFO: a new classifier gate} {
        r del gi:cls
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
        r del gi:score
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
        r del gi:cls gi:score
        r gcreate gi:cls ALPHA 0.1 CLASSES 3 a b c
        r gcreate gi:score ALPHA 0.3 SCORE
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
            r del $key
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
        r del gi:cls gi:score
        r gcreate gi:cls ALPHA 0.1 CLASSES 3 a b c
        r gcreate gi:score ALPHA 0.3 SCORE
        r gadd gi:cls TEXT hello LABEL a
        r gpredict gi:cls TEXT hello
        r gpredict gi:score SCORE 0.5
        foreach key {gi:cls gi:score} {
            set digest [debug_digest_value $key]
            set first [r ginfo $key]
            for {set i 0} {$i < 5} {incr i} {assert_equal $first [r ginfo $key]}
            assert_equal $digest [debug_digest_value $key]
        }
    }

    # ---------------------------------------------- GPAUSE, GRESUME, GRESET

    gcmd_test {GPAUSE and GRESUME: OK, and again OK} {
        r del gadm:cls gadm:score
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
        r del gadm:cls gadm:score
        r gcreate gadm:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate gadm:score ALPHA 0.1 SCORE
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
        r del gadm:reset
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
        r del gadm:rs
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
        # "GRESET forgets everything learned and proven and restarts the
        # counters, but keeps the configuration (and a pause)"
        r del gadm:pr gadm:pr:cls
        r gcreate gadm:pr ALPHA 0.1 SCORE
        r gcreate gadm:pr:cls ALPHA 0.1 CLASSES 2 a b
        foreach {key state} {gadm:pr CALIBRATING gadm:pr:cls COLD} {
            r gpause $key
            assert_equal OK [r greset $key]
            assert_equal PAUSED [gcmd_info $key state]
            assert_equal OK [r greset $key]
            assert_equal PAUSED [gcmd_info $key state]
            assert_equal OK [r gresume $key]
            assert_equal $state [gcmd_info $key state]
            # ... and a gate that is not paused is not paused by it
            assert_equal OK [r greset $key]
            assert_equal $state [gcmd_info $key state]
        }
        r del gadm:pr gadm:pr:cls
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
        r del ar:cls ar:score
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

    # Three gates that answer by themselves, for the tests below:
    #   p:score  a score gate with a threshold between 0.1 and 0.9
    #   p:audit  the same with AUDIT 1: all it could answer is a spot check
    #   p:cls    a classifier answering both of its labels
    catch {
        r gcreate p:score ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score p:score
        r gcreate p:audit ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score p:audit
        gcmd_create_fast p:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        gcmd_prove_classifier p:cls
    }

    gcmd_test {a teacher that is right makes a gate answer by itself} {
        # (the three gates set up above)
        set info [r ginfo p:score]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info generation]
        assert_equal 0 [dict get $info candidate]
        set threshold [lindex [dict get $info thresholds] 0]
        assert {$threshold > 0.1 && $threshold <= 0.9}

        assert_equal ACTIVE [gcmd_info p:audit state]

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
        set before [r ginfo p:audit]
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
        assert_equal [expr {[dict get $before audited] + 60}] [dict get $info audited]
        assert_equal [expr {[dict get $before audit-samples] + 60}] [dict get $info audit-samples]
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
        r del p:defer
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
        r del p:strict
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
        r del p:feat
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
        r del p:busy
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
        r del p:errors
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
        # one request each, answered if it is a spot check
        foreach {key form answer} {p:score {SCORE 0.9} {CORRECT 1} p:audit {SCORE 0.9} {CORRECT 1}
                                   p:cls {TEXT {buy cheap pills now}} {LABEL spam}} {
            lassign [r gpredict $key {*}$form] action arg
            assert {$action in {ACCEPT AUDIT}}
            if {$action eq "AUDIT"} {assert_equal 1 [r glearn $key $arg {*}$answer]}
        }
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
        assert {[gcmd_info p:cls learned] >= 40}
        assert {[gcmd_info p:score calibration-samples] > 0}
    }

    gcmd_test {GINFO: coverage follows the share of requests the gate can answer} {
        # "coverage | estimated share of requests the gate can answer now";
        # a measurement of the primary, counted when requests are asked.
        r del p:cov
        r gcreate p:cov ALPHA 0.2 DELTA 0.2 SCORE
        assert {[gcmd_info p:cov coverage] == 0}
        gcmd_prove_score p:cov
        set threshold [lindex [gcmd_info p:cov thresholds] 0]
        assert {$threshold > 0.1 && $threshold <= 0.9}
        # every request can be answered
        gcmd_pipeline_predict p:cov 300 0.9
        assert {[gcmd_info p:cov coverage] > 0.9}
        # as many again that cannot: about half
        gcmd_pipeline_predict p:cov 300 0.05
        set coverage [gcmd_info p:cov coverage]
        assert {$coverage > 0.3 && $coverage < 0.7}
        # the estimate follows the traffic: none can be answered any more
        gcmd_pipeline_predict p:cov 3500 0.05
        assert {[gcmd_info p:cov coverage] < 0.1}
        # ... all of them can again
        gcmd_pipeline_predict p:cov 3500 0.9
        assert {[gcmd_info p:cov coverage] > 0.9}
        assert {[gcmd_info p:cov coverage] <= 1}
        # a gate that was reset answers nothing
        r greset p:cov
        assert {[gcmd_info p:cov coverage] == 0}
        r del p:cov
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

    gcmd_test {GPAUSE: where the gate would have replied AUDIT it replies ABSTAIN and still checks itself} {
        # "where it would have replied ACCEPT or AUDIT it replies ABSTAIN.
        # Everything else keeps running": the spot check is still wanted,
        # asked for by its id, and counted when its answer comes.
        r del p:paudit
        r gcreate p:paudit ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score p:paudit
        set before [r ginfo p:paudit]
        assert_equal ACTIVE [dict get $before state]
        assert_equal OK [r gpause p:paudit]
        for {set i 0} {$i < 20} {incr i} {
            set reply [r gpredict p:paudit SCORE 0.9]
            assert_equal ABSTAIN [lindex $reply 0]
            assert {[lindex $reply 1] ne {}}
            assert_equal 1 [r glearn p:paudit [lindex $reply 1] CORRECT [expr {$i != 7}]]
        }
        set paused [r ginfo p:paudit]
        assert_equal PAUSED [dict get $paused state]
        assert_equal OK [r gresume p:paudit]
        set after [r ginfo p:paudit]
        r del p:paudit
        assert_equal ACTIVE [dict get $after state]
        # 20 requests, none answered by the gate, none called AUDIT ...
        assert_equal [expr {[dict get $before requests] + 20}] [dict get $after requests]
        assert_equal [expr {[dict get $before abstained] + 20}] [dict get $after abstained]
        assert_equal [dict get $before audited] [dict get $after audited]
        assert_equal [dict get $before accepted] [dict get $after accepted]
        # ... 20 spot checks answered, one of them a disagreement
        assert_equal [expr {[dict get $before audit-samples] + 20}] [dict get $after audit-samples]
        assert_equal [expr {[dict get $before audit-errors] + 1}] [dict get $after audit-errors]
        assert_equal [dict get $before thresholds] [dict get $after thresholds]
        assert_equal [dict get $before generation] [dict get $after generation]
        assert_equal 0 [dict get $after pending]
    }

    gcmd_test {GPAUSE: a paused gate proves itself and answers as soon as it is resumed} {
        # "a paused gate keeps learning and proving itself (shadow mode)"
        r del p:shadow
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
            r del $key
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

    # A gate that is full, for the null id, and a string. (With the longest
    # feedback timeout there is: its requests must still wait however slow
    # the machine is today.)
    catch {
        r config set gate.feedback-timeout 604800000
        r gcreate rp:full ALPHA 0.2 SCORE
        r config set gate.feedback-timeout 60000
        gcmd_pipeline_predict rp:full 4096 0.5
        r set rp:string value
    }

    foreach proto [gcmd_protocols] {
        # The test client and a raw connection, both speaking this protocol.
        if {[catch {
            r hello $proto
            set fd [gcmd_raw_open $proto]
        } err]} {
            gcmd_test "RESP$proto: HELLO $proto" {fail "cannot switch to RESP$proto: $err"}
            continue
        }

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
            gcmd_typed_fresh $fd
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
            gcmd_typed_fresh $fd
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
            gcmd_typed_fresh $fd
            set learned [r gadd rp:cls TEXT hello LABEL a]
            assert_equal [list int [expr {$learned + 1}]] [gcmd_raw $fd GADD rp:cls TEXT hello LABEL b]
            assert_equal [list int [expr {$learned + 2}]] \
                [gcmd_raw $fd GADD rp:cls TEXT hello FEATURES 1 x 1 LABEL c]
        }

        gcmd_test "RESP$proto: GPAUSE, GRESUME and GRESET reply the simple string OK" {
            gcmd_typed_fresh $fd
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
            gcmd_typed_fresh $fd
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
            gcmd_typed_fresh $fd
            set info [gcmd_check_info $proto [gcmd_raw $fd GINFO rp:score]]
            assert_equal score [dict get $info kind]
            assert_equal CALIBRATING [dict get $info state]
            assert_equal {} [dict get $info classes]
            assert_equal 1 [llength [dict get $info alphas]]
            assert {[lindex [dict get $info alphas] 0] == 0.1}
            assert_equal {{}} [dict get $info thresholds]
            assert_equal 0 [dict get $info generation]
            assert_equal 1 [dict get $info candidate]
            assert_equal [dict keys $info] [dict keys [r ginfo rp:score]]
        }

        gcmd_test "RESP$proto: GINFO of gates that answer: thresholds are numbers" {
            # one request each, so that there is a coverage to report
            foreach {key form answer} {p:score {SCORE 0.9} {CORRECT 1}
                                       p:cls {TEXT {buy cheap pills now}} {LABEL spam}} {
                lassign [gcmd_check_predict [gcmd_raw $fd GPREDICT $key {*}$form]] action arg
                assert {$action in {ACCEPT AUDIT}}
                if {$action eq "AUDIT"} {gcmd_raw $fd GLEARN $key $arg {*}$answer}
            }
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
            gcmd_typed_fresh $fd
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
        r del kt:cls kt:score kt:string
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
        assert {"kt:cls" in $found && "kt:score" in $found}
        assert {"kt:string" ni $found}
        foreach key $found {assert_equal gate-type [r type $key]}
        assert_equal {kt:cls kt:score kt:string} [lsort [r keys kt:*]]
        r del kt:cls kt:score kt:string
    }

    gcmd_test {commands of other types reply WRONGTYPE on a gate and leave it alone} {
        r del kt:cls
        r gcreate kt:cls ALPHA 0.1 CLASSES 2 a b
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
            r del kt:vset kt:score
            r gcreate kt:score ALPHA 0.1 SCORE
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
        r del kt:over
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
        r del kd:cls kd:score
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
        r del ke:gate
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
        r del ke:gone
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
            r del ke:lazy ke:lazy:gate
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
        r del kr:src kr:dst kr:other kr:string kr:final
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

    gcmd_test {DUMP and RESTORE: the gate comes back with its model and without its proof} {
        # "DUMP / RESTORE ... work as for any key". SPEC 3 "Redis": Redis
        # cannot show that nothing was lost, so the gate keeps its model,
        # drops its proof and its waiting requests, and proves itself again.
        foreach kind {score classifier} {
            r del kdr:src kdr:dst kdr:ttl
            if {$kind eq "score"} {
                r gcreate kdr:src ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
                gcmd_prove_score kdr:src
                set ask {SCORE 0.9}
                set answer {CORRECT 1}
            } else {
                gcmd_create_fast kdr:src ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham
                gcmd_prove_classifier kdr:src
                set ask {TEXT {buy cheap pills now}}
                set answer {LABEL spam}
            }
            # a spot check that waits
            lassign [r gpredict kdr:src {*}$ask] action waiting
            assert_equal AUDIT $action
            set before [r ginfo kdr:src]
            assert_equal ACTIVE [dict get $before state]
            assert_equal 1 [dict get $before pending]

            set payload [r dump kdr:src]
            assert_equal OK [r restore kdr:dst 0 $payload]
            assert_equal gate-type [r type kdr:dst]
            assert_equal [debug_digest_value kdr:src] [debug_digest_value kdr:dst]
            assert {[r memory usage kdr:dst] > 0}
            # A command that is refused or finds nothing to do is not a
            # write: the copy is still what was dumped.
            assert_error "ERR *" {r gpredict kdr:dst BOGUS x}
            assert_error "ERR *" {r glearn kdr:dst $waiting BOGUS x}
            assert_equal 0 [r glearn kdr:dst not-an-id {*}$answer]
            assert_error "BUSYKEY*" {r gcreate kdr:dst ALPHA 0.2 SCORE}
            r ginfo kdr:dst
            assert_equal [debug_digest_value kdr:src] [debug_digest_value kdr:dst]

            # GINFO shows the copy as its first write will leave it: what
            # was configured, learned and counted is there, what was
            # proven and what waited is not.
            set shown [r ginfo kdr:dst]
            foreach field {kind alpha delta audit classes alphas learned requests accepted audited
                           abstained missing revocations} {
                assert_equal [dict get $shown $field] [dict get $before $field] "$kind: $field after RESTORE"
            }
            assert_equal CALIBRATING [dict get $shown state]
            assert_equal 0 [dict get $shown generation]
            assert {[dict get $shown candidate] > [dict get $before generation]}
            assert {[dict get $shown candidate] > [dict get $before candidate]}
            foreach threshold [dict get $shown thresholds] {assert_equal {} $threshold}
            foreach field {pending audit-samples audit-errors calibration-samples} {
                assert_equal [dict get $shown $field] 0 "$kind: $field after RESTORE"
            }
            assert {[dict get $shown coverage] == 0}

            # The first write: the request that waited is not known to the copy.
            assert_equal 0 [r glearn kdr:dst $waiting {*}$answer]
            assert_equal [dict remove [r ginfo kdr:dst] memory] [dict remove $shown memory]
            # It answers nothing, asks for the teacher, and proves itself again.
            lassign [r gpredict kdr:dst {*}$ask] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn kdr:dst $id {*}$answer]
            for {set i 0} {$i < 400 && [gcmd_info kdr:dst state] ne "ACTIVE"} {incr i} {
                if {$kind eq "score"} {
                    gcmd_ask_score kdr:dst 1 0.9 1
                } else {
                    lassign [gcmd_example $i] text label
                    lassign [r gpredict kdr:dst TEXT $text] action id
                    if {$id ne {}} {r glearn kdr:dst $id LABEL $label}
                }
            }
            set again [r ginfo kdr:dst]
            assert_equal ACTIVE [dict get $again state]
            # under a snapshot number of its own
            assert {[dict get $again generation] > [dict get $before generation]}

            # The gate that was dumped is as it was.
            assert_equal $before [r ginfo kdr:src]
            assert_equal 1 [r glearn kdr:src $waiting {*}$answer]

            # RESTORE keeps its own rules: an existing key, REPLACE, a time to live.
            assert_error "BUSYKEY*" {r restore kdr:dst 0 $payload}
            assert_equal ACTIVE [gcmd_info kdr:dst state]
            assert_equal OK [r restore kdr:dst 0 $payload REPLACE]
            assert_equal CALIBRATING [gcmd_info kdr:dst state]
            assert_equal OK [r restore kdr:ttl 100000 $payload]
            assert_range [r pttl kdr:ttl] 1 100000
            assert_equal ABSTAIN [lindex [r gpredict kdr:ttl {*}$ask] 0]
            assert_range [r pttl kdr:ttl] 1 100000
            # a payload that is not a dump of a gate creates nothing
            assert_error "ERR*" {r restore kdr:bad 0 [string range $payload 0 end-12]}
            assert_error "ERR*" {r restore kdr:bad 0 garbage}
            assert_equal 0 [r exists kdr:bad]
        }
        r del kdr:src kdr:dst kdr:ttl
    }

    gcmd_test {RESTORE: GINFO shows a loaded gate as its first write will leave it, whatever it was} {
        r del kdr:a kdr:b kdr:c kdr:d kdr:e kdr:f kdr:g
        # a new score gate; one that answers; one that answers and is paused
        r gcreate kdr:a ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate kdr:b ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score kdr:b
        r gcreate kdr:c ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score kdr:c
        r gpredict kdr:c SCORE 0.9
        r gpause kdr:c
        # a classifier that has not learned enough for a snapshot (paused),
        # one that tests its first snapshot with requests waiting, one that
        # answers and tests its second
        gcmd_create_fast kdr:d ALPHA 0.2 DELTA 0.2 CLASSES 3 spam ham other ALWAYSDEFER 1 other
        gcmd_teach kdr:d 7
        r gpredict kdr:d TEXT "buy cheap pills now"
        r gpause kdr:d
        gcmd_create_fast kdr:e ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham CLASSALPHA 1 ham 0.1
        gcmd_teach kdr:e 45
        for {set i 0} {$i < 9} {incr i} {r gpredict kdr:e TEXT [lindex [gcmd_example $i] 0]}
        gcmd_create_fast kdr:f ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        gcmd_prove_classifier kdr:f
        # a gate that is full
        r gcreate kdr:g ALPHA 0.2 SCORE
        gcmd_pipeline_predict kdr:g 4096 0.5
        foreach {key state} {kdr:a CALIBRATING kdr:b ACTIVE kdr:c PAUSED kdr:d PAUSED kdr:e CALIBRATING
                             kdr:f ACTIVE kdr:g CALIBRATING} {
            assert_equal [gcmd_info $key state] $state "the gate $key"
            gcmd_check_saveable $key
            gcmd_check_recovery $key
        }
        # (what the copies will be: a classifier is COLD again only if it
        # has not learned enough for a snapshot)
        r restore kdr:copy 0 [r dump kdr:d]
        assert_equal PAUSED [gcmd_info kdr:copy state]
        r gresume kdr:copy
        assert_equal COLD [gcmd_info kdr:copy state]
        assert_equal 7 [gcmd_info kdr:copy learned]
        r del kdr:copy
        r restore kdr:copy 0 [r dump kdr:g]
        assert_equal 0 [gcmd_info kdr:copy pending]
        lassign [r gpredict kdr:copy SCORE 0.5] action id
        assert {$id ne {}}
        r del kdr:copy kdr:a kdr:b kdr:c kdr:d kdr:e kdr:f kdr:g
    }

    gcmd_test {RESTORE: a payload at the end of what the module accepts stays a gate that can be loaded} {
        # A DUMP payload is bytes anybody may write: the value, the RDB
        # version and a checksum. What the module cannot hold it refuses:
        # no counter above 2^62, no clock after the last moment the gate
        # takes (7 days before the end of 64 bits), no setting outside its
        # range (CHANGES 9d and 9g). What it accepts must stay a gate while
        # clients use it, "whatever the core did with it in between": a
        # gate that its own DUMP cannot bring back turns the next RDB file
        # into one the server cannot load.
        r del kdr:edge kdr:edge:copy
        assert_equal 0xe9c6d914c4b8d9ca [format 0x%x [gcmd_crc64 123456789]]
        r config set rdbcompression no
        r config set gate.snapshot-examples 777 gate.feedback-timeout 54321
        r gcreate kdr:edge ALPHA 0.2 DELTA 0.2 SCORE
        r config set gate.snapshot-examples 1000 gate.feedback-timeout 60000
        set id [lindex [r gpredict kdr:edge SCORE 0.5] 1]
        set payload [r dump kdr:edge]
        r config set rdbcompression yes
        binary scan [string range $payload end-7 end] wu checksum
        assert_equal $checksum [gcmd_crc64 [string range $payload 0 end-8]]
        set body [string range $payload 0 end-8]

        # Where things are in the value (gate_codec.c), found in this
        # payload by what it must hold. Around the number the ids of the
        # gate start with: before it the two settings the gate was created
        # with and the number of its ledger; after it the time it was
        # created, its clock, how many snapshots it has taken, the pause,
        # the examples at the last snapshot, the outage flag, the answers
        # missing in a row, and the number the next request had when the
        # last answer was used.
        set at [string first [binary format w 0x[string range $id 0 15]] $body]
        assert {$at > 20}
        binary scan [string range $body [expr {$at - 20}] [expr {$at + 53}]] iwwwwwwcwciw \
            step timeout ledger epoch created clock snapshots paused examples blind run mark
        set wall [clock milliseconds]
        if {abs($created - $wall) > 600000 || abs($clock - $wall) > 600000 || $clock < $created} {
            fail "the clock of a gate is not where this test looks for it in a DUMP payload"
        }
        if {[list $step $timeout $snapshots $paused $examples $blind $run $mark] ne {777 54321 1 0 0 0 0 0}} {
            fail "the settings and the ledger of a gate are not where this test looks for them in a DUMP\
                payload: $step $timeout $snapshots $paused $examples $blind $run $mark"
        }
        # Further on: the number of the next request, how many wait, and
        # the first of them with its number and its time (the last such
        # pair: a snapshot taken in the same millisecond has the number 1
        # and that time too), its flags, guesses and answer, and its
        # number as a spot check.
        set waiting [string last [binary format ww 1 $clock] $body]
        set next_number -1
        set waiting_count -1
        if {$waiting >= 12} {
            binary scan [string range $body [expr {$waiting - 12}] [expr {$waiting - 1}]] wi next_number waiting_count
            binary scan [string range $body [expr {$waiting + 16}] [expr {$waiting + 75}]] iiiwwwwiwi \
                flags cand_label serv_label cand_score serv_score cand_snapshot serv_snapshot answer spot clues
        }
        if {$next_number != 2 || $waiting_count != 1} {
            fail "the request counter of a gate is not where this test looks for it in a DUMP payload"
        }
        # (proof for the snapshot being tested, number 1, bit 4; the draw
        # may have set bit 1; no answer yet)
        if {[list [expr {$flags & ~1}] $cand_label $serv_label $cand_snapshot $serv_snapshot $answer $spot $clues] ne
            {4 -1 -1 1 0 -1 0 0}} {
            fail "the waiting request of a gate is not where this test looks for it in a DUMP payload:\
                $flags $cand_label $serv_label $cand_snapshot $serv_snapshot $answer $spot $clues"
        }
        # At the end of the value, before the three bytes that close it:
        # the counters. Requests, those accepted, audited and left to the
        # teacher, those refused because the gate was full, the requests
        # behind the coverage and the coverage; then the ids handed out,
        # the answers used and not used, those missing, the requests
        # dropped, and how often a snapshot took over, was withdrawn, or
        # was given up.
        set counters [expr {[string length $body] - 123}]
        binary scan [string range $body $counters end-3] w15 counted
        if {$counted ne {1 0 0 1 0 0 0 1 0 0 0 0 0 0 0}} {
            fail "the counters of a gate are not where this test looks for them in a DUMP payload: $counted"
        }
        set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
        set last_count [expr {1 << 62}]

        # what, where, as which kind of number(s), the value(s), and how
        # many of the next two requests of a client get an id. -1: the
        # payload must be refused. With a "!": it must be taken, it is
        # what a gate of this server can come to hold (the settings at the
        # end of their ranges, 31 answers missing in a row); the others
        # the module may refuse as well.
        set forgeries [list \
            "the clock one millisecond past its last value" [expr {$at + 16}]      w [expr {$last_clock + 1}] -1 \
            "the next request number 2^62 + 1"              [expr {$waiting - 12}] w [expr {$last_count + 1}] -1 \
            "2^62 + 1 snapshots taken"                      [expr {$at + 24}]      w [expr {$last_count + 1}] -1 \
            "2^62 + 1 examples at the last snapshot"        [expr {$at + 33}]      w [expr {$last_count + 1}] -1 \
            "a snapshot step of 1,000,000,001 examples"     [expr {$at - 20}]      i 1000000001               -1 \
            "a snapshot step of 0 examples"                 [expr {$at - 20}]      i 0                        -1 \
            "a feedback timeout of 7 days and 1 ms"         [expr {$at - 16}]      w 604800001                -1 \
            "a feedback timeout of 0 ms"                    [expr {$at - 16}]      w 0                        -1 \
            "32 answers missing in a row"                   [expr {$at + 42}]      i 32                       -1 \
            "the last answer used at request number 3"      [expr {$at + 46}]      w 3                        -1 \
            "an outage, with a snapshot being tested"       [expr {$at + 41}]      c 1                        -1 \
            "a spot check number on a request that is none" [expr {$waiting + 64}] w 1                        -1 \
            "2^62 + 1 ids handed out"                       [expr {$counters + 56}] w [expr {$last_count + 1}] -1 \
            "2^62 + 1 answers used"                         [expr {$counters + 64}] w [expr {$last_count + 1}] -1 \
            "2^62 + 1 requests, all left to the teacher"    $counters w4 [list [expr {$last_count + 1}] 0 0 [expr {$last_count + 1}]] -1 \
            "the clock $last_clock"                         [expr {$at + 16}]      w $last_clock              2 \
            "the next request number 2^62"                  [expr {$waiting - 12}] w $last_count              0 \
            "the next request number 2^62 - 1"              [expr {$waiting - 12}] w [expr {$last_count - 1}] 1 \
            "2^62 snapshots taken"                          [expr {$at + 24}]      w $last_count              0 \
            "2^62 - 1 snapshots taken"                      [expr {$at + 24}]      w [expr {$last_count - 1}] 2 \
            "a snapshot step of 1,000,000,000 examples"     [expr {$at - 20}]      i 1000000000               2! \
            "a feedback timeout of 7 days"                  [expr {$at - 16}]      w 604800000                2! \
            "31 answers missing in a row"                   [expr {$at + 42}]      i 31                       2! \
            "the last answer used at request number 2"      [expr {$at + 46}]      w 2                        2! \
            "2^62 ids handed out"                           [expr {$counters + 56}] w $last_count             2 \
            "2^62 answers used"                             [expr {$counters + 64}] w $last_count             2 \
            "2^62 answers missing"                          [expr {$counters + 80}] w $last_count             2 \
            "2^62 requests, all left to the teacher"        $counters w4 [list $last_count 0 0 $last_count]   2 \
        ]
        set failures {}
        foreach {what offset kind value ids} $forgeries {
            r del kdr:edge:copy
            set raw [binary format $kind $value]
            set forged [string replace $body $offset [expr {$offset + [string length $raw] - 1}] $raw]
            append forged [binary format w [gcmd_crc64 $forged]]
            set refused [catch {r restore kdr:edge:copy 0 $forged} err]
            set must [regsub {!$} $ids {} ids]
            if {$ids < 0} {
                if {!$refused} {lappend failures "RESTORE of a payload with $what is accepted"}
                continue
            }
            if {$refused} {
                if {$must} {lappend failures "RESTORE of a payload with $what is refused: $err"}
                continue
            }
            if {[catch {
                gcmd_check_saveable kdr:edge:copy "as restored"
                # GINFO shows a loaded gate as its first write will leave
                # it (here: a write that does nothing by itself)
                set shown [r ginfo kdr:edge:copy]
                assert_equal OK [r gresume kdr:edge:copy]
                set after [r ginfo kdr:edge:copy]
                if {[dict remove $shown memory] ne [dict remove $after memory]} {
                    fail "GINFO before its first write:\n$shown\nand after it:\n$after"
                }
                # a client uses the gate
                lassign [r gpredict kdr:edge:copy SCORE 0.5] action first
                assert_equal ABSTAIN $action
                gcmd_check_saveable kdr:edge:copy "after one GPREDICT"
                after 5
                lassign [r gpredict kdr:edge:copy SCORE 0.5] action second
                assert_equal ABSTAIN $action
                # A gate that has no request number left answers ABSTAIN
                # without an id, like a gate that is full; one that has no
                # snapshot number left tests nothing and wants no feedback
                # (CHANGES 9d).
                assert_equal $ids [expr {($first ne {}) + ($second ne {})}] "requests that got an id"
                foreach answered [list $second $first] {
                    if {$answered eq {}} continue
                    assert_equal 1 [r glearn kdr:edge:copy $answered CORRECT 1]
                    break
                }
                after 5
                assert_equal OK [r gpause kdr:edge:copy]
                gcmd_check_saveable kdr:edge:copy "after GPREDICT, GPREDICT, GLEARN and GPAUSE some milliseconds apart"
                assert_equal OK [r greset kdr:edge:copy]
                gcmd_check_saveable kdr:edge:copy "after GRESET"
            } err]} {
                regsub {^assertion:} $err {} err
                lappend failures "RESTORE of a payload with $what is accepted, and then: $err"
            }
        }
        r del kdr:edge kdr:edge:copy
        if {[llength $failures]} {fail [join $failures "\n"]}
    }

    gcmd_test {RESTORE: a spot check or a drift detector that cannot be is refused} {
        # CHANGES 9, codec: a request keeps the number it has as a spot
        # check, and each label of an answering snapshot its detector: how
        # many spot checks were asked, how many disagreed, and for each of
        # the three open blocks the disagreements and the lost answers.
        # Refused: "wrong + lost of an open block above its size, errors
        # above asked"; "a spot number on a request that was not a spot
        # check, or above what its row's detector has handed out".
        #
        # The gate: four spot checks. The first gets no answer and is lost
        # (the feedback timeout of the gate is 300 ms), the second agrees,
        # the third disagrees, the fourth waits. If the machine was too
        # slow for exactly one answer to be lost, the gate is made again.
        r config set rdbcompression no
        for {set attempt 1} {$attempt <= 3} {incr attempt} {
            r del kdr:spot kdr:spot:copy
            r config set gate.feedback-timeout 300
            r gcreate kdr:spot ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
            r config set gate.feedback-timeout 60000
            gcmd_prove_all kdr:spot
            assert_equal AUDIT [lindex [r gpredict kdr:spot SCORE 0.9] 0]
            after 320
            assert_equal AUDIT [gcmd_ask_score kdr:spot 1 0.9 1]
            assert_equal AUDIT [gcmd_ask_score kdr:spot 1 0.9 0]
            lassign [r gpredict kdr:spot SCORE 0.9] action id
            assert_equal AUDIT $action
            set info [r ginfo kdr:spot]
            set payload [r dump kdr:spot]
            if {[dict get $info missing] == 1 && [dict get $info audit-samples] == 2 &&
                [dict get $info pending] == 1} break
        }
        r config set rdbcompression yes
        assert_equal 1 [dict get $info missing] "answers lost (after three attempts)"
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info audit-errors]
        set body [string range $payload 0 end-8]

        # The waiting request, found by its number and the clock of the
        # gate (it was the last write); before it the number of the next
        # request and how many wait; before those the detector of the
        # snapshot that answers (a score gate has one). And in the ledger,
        # 46 bytes after the number the ids start with: the number the
        # next request had when the last answer was used, which is the
        # number of the request that waits.
        set number [lindex [split $id -] 1]
        set at [string first [binary format w 0x[string range $id 0 15]] $body]
        assert {$at > 0}
        binary scan [string range $body [expr {$at + 16}] [expr {$at + 23}]] w clock
        binary scan [string range $body [expr {$at + 41}] [expr {$at + 53}]] ciw blind run mark
        set waiting [string last [binary format ww $number $clock] $body]
        assert {$waiting > 52}
        binary scan [string range $body [expr {$waiting - 52}] [expr {$waiting - 1}]] wwiiiiiiwi \
            asked errors wrong64 wrong512 wrong4096 lost64 lost512 lost4096 next_number waiting_count
        binary scan [string range $body [expr {$waiting + 16}] [expr {$waiting + 75}]] iiiwwwwiwi \
            flags cand_label serv_label cand_score serv_score cand_snapshot serv_snapshot answer spot clues
        if {[list $asked $errors $wrong64 $wrong512 $wrong4096 $lost64 $lost512 $lost4096 $next_number $waiting_count] ne
            [list 4 1 1 1 1 1 1 1 [expr {$number + 1}] 1]} {
            fail "the drift detector of a gate is not where this test looks for it in a DUMP payload:\
                $asked $errors $wrong64 $wrong512 $wrong4096 $lost64 $lost512 $lost4096 $next_number $waiting_count"
        }
        # (a spot check: bits 1, 2 and 8, proof for the snapshot that
        # answers, number 1; the fourth of its label)
        if {[list $flags $serv_snapshot $answer $spot $clues] ne {11 1 -1 4 0}} {
            fail "the waiting spot check of a gate is not where this test looks for it in a DUMP payload:\
                $flags $serv_snapshot $answer $spot $clues"
        }
        # (one answer was lost, and one has been used since: no outage)
        if {[list $blind $run $mark] ne [list 0 0 $number]} {
            fail "the answers missing in a row are not where this test looks for them in a DUMP payload:\
                $blind $run $mark"
        }
        # (The copies made below get a feedback timeout of a minute, so
        # that nothing they are asked from here on depends on the clock:
        # the setting is 16 bytes before the number the ids start with.)
        binary scan [string range $body [expr {$at - 16}] [expr {$at - 9}]] w timeout
        assert_equal 300 $timeout
        set body [string replace $body [expr {$at - 16}] [expr {$at - 9}] [binary format w 60000]]
        set detector [expr {$waiting - 52}]
        set failures {}
        foreach {what offset kind value accepted} [list \
            "the payload as it is"                                       $detector               w 4  1 \
            "spot check number 5 of 4 asked"                             [expr {$waiting + 64}]  w 5  0 \
            "a spot check number on a request the gate would not answer" [expr {$waiting + 16}]  i 9  0 \
            "5 disagreements among 4 spot checks"                        [expr {$detector + 8}]  w 5  0 \
            "4 disagreements and 1 lost answer in an open block of 4"    [expr {$detector + 16}] i 4  0 \
            "1 disagreement and 4 lost answers in an open block of 4"    [expr {$detector + 28}] i 4  0 \
            "5 lost answers in an open block of 4 (blocks of 4,096)"     [expr {$detector + 36}] i 5  0 \
            "the last answer used at a request number not reached yet"   [expr {$at + 46}]       w [expr {$number + 2}] 0 \
            "1 disagreement and 3 lost answers in an open block of 4"    [expr {$detector + 28}] i 3  1 \
            "3 disagreements and 1 lost answer in an open block of 4"    [expr {$detector + 24}] i 3  1 \
            "4 disagreements among 4 spot checks"                        [expr {$detector + 8}]  w 4  1 \
            "spot check number 1 of 4 asked"                             [expr {$waiting + 64}]  w 1  1 \
        ] {
            r del kdr:spot:copy
            set raw [binary format $kind $value]
            set forged [string replace $body $offset [expr {$offset + [string length $raw] - 1}] $raw]
            append forged [binary format w [gcmd_crc64 $forged]]
            set refused [catch {r restore kdr:spot:copy 0 $forged} err]
            if {!$accepted} {
                if {!$refused} {lappend failures "RESTORE of a payload with $what is accepted"}
                continue
            }
            if {$refused} {
                lappend failures "RESTORE of a payload with $what is refused: $err"
                continue
            }
            if {[catch {
                gcmd_check_saveable kdr:spot:copy "as restored"
                set shown [r ginfo kdr:spot:copy]
                assert_equal CALIBRATING [dict get $shown state]
                assert_equal 0 [dict get $shown audit-samples]
                # the copy starts over; its spot checks are numbered from 1 again
                assert_equal 0 [r glearn kdr:spot:copy $id CORRECT 1]
                gcmd_prove_all kdr:spot:copy
                assert_equal AUDIT [lsort -unique [gcmd_ask_score kdr:spot:copy 6 0.9 0]]
                assert_equal ACTIVE [gcmd_info kdr:spot:copy state]
                assert_equal AUDIT [gcmd_ask_score kdr:spot:copy 1 0.9 0]
                assert_equal 1 [gcmd_info kdr:spot:copy revocations]
                gcmd_check_saveable kdr:spot:copy "after its seventh wrong spot check"
            } err]} {
                regsub {^assertion:} $err {} err
                lappend failures "RESTORE of a payload with $what is accepted, and then: $err"
            }
        }
        r del kdr:spot kdr:spot:copy
        if {[llength $failures]} {fail [join $failures "\n"]}
    }

    gcmd_test {a proven gate keeps answering after RENAME} {
        r del kr:proven kr:proven2
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
        r del km:gate
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
        r del ks:gate
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
        r del kdb:gate
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
        r del kmem:score kmem:cls
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
        r del ko:gate
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
        r del kc:cls kc:score kc:string kc:dst
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
        r del acl:cls acl:score
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
        r del acl:cls acl:score
        r gcreate acl:cls ALPHA 0.1 CLASSES 2 a b
        r gcreate acl:score ALPHA 0.1 SCORE
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
        r del app:gate other:gate
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
        r del ap:gate
        r gcreate ap:gate ALPHA 0.1 SCORE
        set before [gcmd_state ap:gate]
        set attempts {
            {_gate.apply ap:gate TICK 0}
            {_GATE.APPLY ap:gate TICK 0}
            {_GATE.APPLY ap:gate PAUSE 0}
            {_GATE.APPLY ap:gate RESET 0 0000000000000001}
            {_GATE.APPLY ap:gate RECOVER 0 0000000000000001}
            {_GATE.APPLY ap:new CREATE 0 0000000000000001 0000000000000002 1000 60000 ALPHA 0.1 SCORE}
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
            '0000000000000001', '0000000000000002', '1000', '60000', 'ALPHA', '0.1', 'SCORE')} 1 ap:new}

        assert_equal $before [gcmd_state ap:gate]
        assert_equal 0 [r exists ap:new]
    }

    gcmd_test {_GATE.APPLY is not listed among the commands} {
        assert_equal {{}} [r command info _gate.apply]
        assert_equal {{}} [r command info _GATE.APPLY]
        assert_equal {} [r command docs _gate.apply]
        assert_equal {gadd gcreate ginfo glearn gpause gpredict greset gresume} \
            [lsort [string tolower [r command list filterby module gate]]]
        assert_equal {} [r command list filterby pattern _gate*]
        assert_equal {} [r command list filterby pattern _GATE*]
        foreach name [r command list] {
            assert {![string match -nocase "_gate*" $name]}
        }
        # no ACL category gives it away
        foreach category [r acl cat] {
            foreach name [r acl cat $category] {
                assert {![string match -nocase "_gate*" $name]}
            }
        }
    }

    gcmd_test {out of memory: requests are refused with an error, answers and administration are taken} {
        # SPEC.md does not say what a gate does when the server is over
        # its memory limit. "If Redis ... returns an error, call the
        # teacher" covers the commands that make a gate hold more
        # (GCREATE, GPREDICT, GADD); what lets it hold less or stop must
        # get through: the answers it waits for, GPAUSE, GRESUME, GRESET,
        # DEL, and GINFO.
        foreach {name refused} {gcreate 1 gpredict 1 gadd 1 glearn 0 ginfo 0 gpause 0 gresume 0 greset 0} {
            set flags [lindex [lindex [r command info $name] 0] 2]
            assert_equal [expr {"denyoom" in $flags}] $refused "the denyoom flag of $name"
        }
        r del oom:score oom:cls oom:new
        r gcreate oom:score ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate oom:cls ALPHA 0.2 CLASSES 2 a b
        set score_id [lindex [r gpredict oom:score SCORE 0.5] 1]
        set cls_id [lindex [r gpredict oom:cls TEXT hello] 1]
        set before [list [gcmd_state oom:score] [gcmd_state oom:cls]]
        assert_equal noeviction [lindex [r config get maxmemory-policy] 1]
        r config set maxmemory 1
        set rc [catch {
            assert_error "OOM *" {r gpredict oom:score SCORE 0.5}
            assert_error "OOM *" {r gpredict oom:cls TEXT hello}
            assert_error "OOM *" {r gadd oom:cls TEXT hello LABEL a}
            assert_error "OOM *" {r gcreate oom:new ALPHA 0.2 SCORE}
            assert_equal $before [list [gcmd_state oom:score] [gcmd_state oom:cls]]
            assert_equal 0 [r exists oom:new]
            assert_equal 1 [r glearn oom:score $score_id CORRECT 1]
            assert_equal 1 [r glearn oom:cls $cls_id LABEL a]
            assert_equal 0 [r glearn oom:cls $cls_id LABEL a]
            foreach key {oom:score oom:cls} {
                assert_equal 0 [gcmd_info $key pending]
                assert_equal OK [r gpause $key]
                assert_equal PAUSED [gcmd_info $key state]
                assert_equal OK [r gresume $key]
                assert_equal OK [r greset $key]
            }
        } err]
        r config set maxmemory 0
        if {$rc} {error $err}
        # with memory to spare the gates take requests again
        assert_equal ABSTAIN [lindex [r gpredict oom:score SCORE 0.5] 0]
        assert_equal 1 [r gadd oom:cls TEXT hello LABEL a]
        assert_equal 2 [r del oom:score oom:cls]
    }

    gcmd_test {the server is still up (COMMAND and ACL)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 4. Server settings
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    gcmd_test {CONFIG GET: the two settings and their defaults; the testing seed is hidden} {
        # SPEC 5: 1,000 examples, 1 minute (milliseconds).
        assert_equal {gate.snapshot-examples 1000} [r config get gate.snapshot-examples]
        assert_equal {gate.feedback-timeout 60000} [r config get gate.feedback-timeout]
        # A pattern lists the two settings of SPEC.md and not the seed ...
        assert_equal {gate.feedback-timeout 60000 gate.snapshot-examples 1000} \
            [lsort -stride 2 [r config get gate.*]]
        assert {![dict exists [r config get *] gate.test-seed]}
        assert {![dict exists [r config get *seed*] gate.test-seed]}
        assert_equal {gate.feedback-timeout gate.snapshot-examples} \
            [lsort [dict keys [r config get gate.feedback-timeout gate.snapshot-examples gate.t*]]]
        # ... which is there for whoever asks for it by name: 0, not seeded.
        assert_equal {gate.test-seed 0} [r config get gate.test-seed]
    }

    gcmd_test {CONFIG SET: values in range are stored} {
        # SPEC.md names the settings and their defaults, not their ranges;
        # these are the ranges gate.c registers.
        foreach {name value} {
            gate.snapshot-examples 1          gate.snapshot-examples 20
            gate.snapshot-examples 1000000000 gate.snapshot-examples 1000
            gate.feedback-timeout 1           gate.feedback-timeout 1000
            gate.feedback-timeout 604800000   gate.feedback-timeout 60000
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
        # a seed that is set stays out of patterns all the same
        assert_equal {gate.feedback-timeout gate.snapshot-examples} [lsort [dict keys [r config get gate.*]]]
        assert_equal OK [r config set gate.snapshot-examples 1000 gate.feedback-timeout 60000 gate.test-seed 0]
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
        assert_equal {gate.feedback-timeout 60000} [r config get gate.feedback-timeout]
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
        r config set gate.snapshot-examples 1000 gate.feedback-timeout 60000
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
        r config set gate.feedback-timeout 60000
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
        # The request of a classifier that tests nothing was wanted as an
        # example only: it is forgotten, not missing ("requests that were
        # needed as proof and never got their GLEARN").
        assert_equal 0 [gcmd_info cf:fast:cls missing]
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
# 5. MULTI/EXEC, WATCH, client-side caching and Lua
# ============================================================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77003

    # A gate that answers, one that only audits, one that is full (and
    # stays full: its requests wait for a week).
    catch {
        r gcreate tx:proven ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score tx:proven
        r gcreate tx:audit ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_score tx:audit
        r config set gate.feedback-timeout 604800000
        r gcreate tx:full ALPHA 0.2 SCORE
        r config set gate.feedback-timeout 60000
        gcmd_pipeline_predict tx:full 4096 0.5
    }

    gcmd_test {the gates the transaction and script tests use are ready} {
        assert_equal ACTIVE [gcmd_info tx:proven state]
        assert_equal ACTIVE [gcmd_info tx:audit state]
        assert_equal 4096 [gcmd_info tx:full pending]
    }

    gcmd_test {MULTI/EXEC: gate commands are queued and reply as usual} {
        catch {r discard}
        r del tx:cls tx:score
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
        r del tx:score tx:never
        r gcreate tx:score ALPHA 0.1 SCORE
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

    gcmd_test {MULTI/EXEC: answers out of order inside one transaction} {
        # Proof is counted in request order (SPEC 3): an answer is used at
        # once (GLEARN replies 1, the request no longer waits for GLEARN)
        # and counts as proof when the older answers are there.
        catch {r discard}
        r del tx:order
        r gcreate tx:order ALPHA 0.2 DELTA 0.2 SCORE
        r multi
        for {set i 0} {$i < 4} {incr i} {r gpredict tx:order SCORE 0.9}
        r ginfo tx:order
        set replies [r exec]
        set ids [lmap reply [lrange $replies 0 3] {lindex $reply 1}]
        assert_equal 4 [llength [lsort -unique $ids]]
        assert_equal 4 [dict get [lindex $replies 4] pending]
        lassign $ids id1 id2 id3 id4

        r multi
        r glearn tx:order $id4 CORRECT 1
        r ginfo tx:order
        r glearn tx:order $id2 CORRECT 0
        r ginfo tx:order
        r glearn tx:order $id4 CORRECT 1
        r glearn tx:order $id1 CORRECT 1
        r ginfo tx:order
        r glearn tx:order $id3 CORRECT 1
        r ginfo tx:order
        r glearn tx:order $id1 CORRECT 0
        set replies [r exec]
        assert_equal 10 [llength $replies]
        assert_equal {1 1 0 1 1 0} [list [lindex $replies 0] [lindex $replies 2] [lindex $replies 4] \
                                        [lindex $replies 5] [lindex $replies 7] [lindex $replies 9]]
        foreach {at pending samples} {1 3 0  3 2 0  6 1 2  8 0 4} {
            set info [lindex $replies $at]
            assert_equal [dict get $info pending] $pending "pending in reply $at"
            assert_equal [dict get $info calibration-samples] $samples "calibration-samples in reply $at"
        }
        r del tx:order
    }

    # --------------------------------------- WATCH and client-side caching

    gcmd_test {WATCH: a command that is refused or finds nothing to do does not abort the transaction} {
        # CHANGES 6: keys are signalled as modified only when a command
        # changed the gate.
        catch {r discard}
        r del tx:w:score tx:w:cls tx:w:string tx:w:missing
        r gcreate tx:w:score ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate tx:w:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 a b
        r set tx:w:string value
        set score_id [lindex [r gpredict tx:w:score SCORE 0.5] 1]
        set cls_id [lindex [r gpredict tx:w:cls TEXT hello] 1]
        set w [redis_client]
        foreach {key cmd pattern} [list \
            tx:w:score {gcreate tx:w:score ALPHA 0.2 SCORE}                   {BUSYKEY*} \
            tx:w:score {gcreate tx:w:score ALPHA 7 BOGUS}                     {BUSYKEY*} \
            tx:w:cls   {gcreate tx:w:cls ALPHA 0.2 CLASSES 2 a b}             {BUSYKEY*} \
            tx:w:score {gpredict tx:w:score SCORE 7}                          {ERR *} \
            tx:w:score {gpredict tx:w:score SCORE nan}                        {ERR *} \
            tx:w:score {gpredict tx:w:score TEXT hello}                       {ERR *} \
            tx:w:score {gpredict tx:w:score SCORE 0.5 extra}                  {ERR *} \
            tx:w:score {gpredict tx:w:score}                                  {ERR wrong number*} \
            tx:w:cls   {gpredict tx:w:cls SCORE 0.5}                          {ERR *} \
            tx:w:cls   {gpredict tx:w:cls TEXT hello FEATURES 2 x 1 x 2}      {ERR *} \
            tx:w:cls   {gpredict tx:w:cls TEXT hello FEATURES 1 x inf}        {ERR *} \
            tx:w:cls   [list gpredict tx:w:cls TEXT [string repeat x 1048577]] {ERR *} \
            tx:w:score [list glearn tx:w:score $score_id CORRECT 2]           {ERR *} \
            tx:w:score [list glearn tx:w:score $score_id LABEL a]             {ERR *} \
            tx:w:score [list glearn tx:w:score $score_id CORRECT]             {ERR wrong number*} \
            tx:w:cls   [list glearn tx:w:cls $cls_id LABEL nosuch]            {ERR *} \
            tx:w:cls   [list glearn tx:w:cls $cls_id CORRECT 1]               {ERR *} \
            tx:w:score {gadd tx:w:score TEXT hello LABEL a}                   {ERR *} \
            tx:w:cls   {gadd tx:w:cls TEXT hello LABEL nosuch}                {ERR *} \
            tx:w:cls   {gadd tx:w:cls TEXT hello FEATURES 1 x LABEL a}        {ERR *} \
            tx:w:cls   {gadd tx:w:cls TEXT hello}                             {ERR wrong number*} \
            tx:w:score {ginfo tx:w:score extra}                               {ERR wrong number*} \
            tx:w:score {gpause tx:w:score extra}                              {ERR wrong number*} \
            tx:w:score {gresume tx:w:score extra}                             {ERR wrong number*} \
            tx:w:score {greset tx:w:score extra}                              {ERR wrong number*} \
            tx:w:string {gpredict tx:w:string SCORE 0.5}                      {WRONGTYPE*} \
            tx:w:string {glearn tx:w:string some-id CORRECT 1}                {WRONGTYPE*} \
            tx:w:string {gadd tx:w:string TEXT hello LABEL a}                 {WRONGTYPE*} \
            tx:w:string {ginfo tx:w:string}                                   {WRONGTYPE*} \
            tx:w:string {gpause tx:w:string}                                  {WRONGTYPE*} \
            tx:w:string {gresume tx:w:string}                                 {WRONGTYPE*} \
            tx:w:string {greset tx:w:string}                                  {WRONGTYPE*} \
            tx:w:string {gcreate tx:w:string ALPHA 0.2 SCORE}                 {BUSYKEY*} \
            tx:w:missing {gpredict tx:w:missing SCORE 0.5}                    {ERR no such key} \
            tx:w:missing {glearn tx:w:missing some-id CORRECT 1}              {ERR no such key} \
            tx:w:missing {gadd tx:w:missing TEXT hello LABEL a}               {ERR no such key} \
            tx:w:missing {ginfo tx:w:missing}                                 {ERR no such key} \
            tx:w:missing {gpause tx:w:missing}                                {ERR no such key} \
            tx:w:missing {gresume tx:w:missing}                               {ERR no such key} \
            tx:w:missing {greset tx:w:missing}                                {ERR no such key} \
            tx:w:missing {gcreate tx:w:missing ALPHA 7 SCORE}                 {ERR *} \
            tx:w:missing {gcreate tx:w:missing ALPHA 0.2 CLASSES 1 a}         {ERR *} \
        ] {
            set shown "command: [string range $cmd 0 100]"
            set aborted [gcmd_aborts_watch $w $key $cmd reply]
            assert_match $pattern $reply $shown
            assert_equal $aborted 0 "a WATCH on $key was aborted by a refused $shown"
        }
        # GLEARN with something that is not an id of any gate: reply 0,
        # nothing to do.
        foreach id [list garbage {} x-1 0123456789abcdef 0123456789abcdef- 0123456789abcdef-0 \
                        0123456789abcdef-01 0123456789abcdefg-1 "$score_id " "x$score_id" \
                        [string repeat 9 100]] {
            set aborted [gcmd_aborts_watch $w tx:w:score [list glearn tx:w:score $id CORRECT 1] reply]
            assert_equal $reply 0 "GLEARN with the id '$id'"
            assert_equal $aborted 0 "a WATCH was aborted by GLEARN with the malformed id '$id'"
        }
        # Reading is not a change either.
        foreach key {tx:w:score tx:w:cls} {
            set aborted [gcmd_aborts_watch $w $key [list ginfo $key] reply]
            assert_equal [dict get $reply pending] 1
            assert_equal $aborted 0 "a WATCH on $key was aborted by GINFO"
        }
        $w close
        # The requests that waited all along are still there.
        assert_equal 1 [r glearn tx:w:score $score_id CORRECT 1]
        assert_equal 1 [r glearn tx:w:cls $cls_id LABEL a]
        assert_equal 0 [r exists tx:w:missing]
        assert_equal value [r get tx:w:string]
    }

    gcmd_test {WATCH: a command that changes the gate aborts the transaction} {
        catch {r discard}
        r del tx:w:score tx:w:cls tx:w:new
        r gcreate tx:w:score ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate tx:w:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 a b
        set w [redis_client]
        # a request that is recorded, and its answer
        foreach {key ask answer} {tx:w:score {SCORE 0.5} {CORRECT 1} tx:w:cls {TEXT hello} {LABEL a}} {
            assert_equal 1 [gcmd_aborts_watch $w $key [list gpredict $key {*}$ask] reply] "GPREDICT $key"
            assert_equal ABSTAIN [lindex $reply 0]
            set id [lindex $reply 1]
            assert_equal 1 [gcmd_aborts_watch $w $key [list glearn $key $id {*}$answer] reply] "GLEARN $key"
            assert_equal 1 $reply
        }
        assert_equal 1 [gcmd_aborts_watch $w tx:w:cls {gadd tx:w:cls TEXT hello LABEL b} reply]
        assert_equal 2 $reply
        foreach key {tx:w:score tx:w:cls} {
            assert_equal 1 [gcmd_aborts_watch $w $key [list gpause $key] reply] "GPAUSE $key"
            assert_equal OK $reply
            assert_equal 1 [gcmd_aborts_watch $w $key [list gresume $key] reply] "GRESUME $key"
            assert_equal OK $reply
            assert_equal 1 [gcmd_aborts_watch $w $key [list greset $key] reply] "GRESET $key"
            assert_equal OK $reply
        }
        # GCREATE: a key that was watched while it did not exist
        assert_equal 1 [gcmd_aborts_watch $w tx:w:new {gcreate tx:w:new ALPHA 0.2 SCORE} reply]
        assert_equal OK $reply
        # ... and DEL, like any key
        assert_equal 1 [gcmd_aborts_watch $w tx:w:new {del tx:w:new} reply]

        # A request the gate answers by itself: GINFO no longer shows what
        # the transaction read (requests, accepted). Also a spot check, a
        # request below the threshold, and a request to a full gate.
        set seen {}
        for {set i 0} {$i < 200 && [llength [lsort -unique $seen]] < 2} {incr i} {
            set aborted [gcmd_aborts_watch $w tx:proven {gpredict tx:proven SCORE 0.9} reply]
            lassign $reply action id
            lappend seen $action
            assert_equal $aborted 1 "a WATCH survived GPREDICT replying $action"
            if {$action eq "AUDIT"} {r glearn tx:proven $id CORRECT 1}
        }
        assert_equal {ACCEPT AUDIT} [lsort -unique $seen]
        assert_equal 1 [gcmd_aborts_watch $w tx:proven {gpredict tx:proven SCORE 0.05} reply]
        assert_equal ABSTAIN [lindex $reply 0]
        if {[lindex $reply 1] ne {}} {r glearn tx:proven [lindex $reply 1] CORRECT 0}
        assert_equal 1 [gcmd_aborts_watch $w tx:full {gpredict tx:full SCORE 0.5} reply]
        assert_equal {ABSTAIN {}} $reply
        $w close
    }

    gcmd_test {WATCH: the transaction of the same connection sees its own gate commands} {
        # A connection may WATCH a gate, look at it and change it in a
        # transaction; only somebody else's change aborts it.
        catch {r discard}
        r del tx:w:score
        r gcreate tx:w:score ALPHA 0.2 DELTA 0.2 SCORE
        r watch tx:w:score
        set pending [gcmd_info tx:w:score pending]
        r multi
        r gpredict tx:w:score SCORE 0.5
        r ginfo tx:w:score
        set replies [r exec]
        assert_equal 2 [llength $replies]
        assert_equal [expr {$pending + 1}] [dict get [lindex $replies 1] pending]
        # the same, with a request of another connection in between
        set other [redis_client]
        r watch tx:w:score
        $other gpredict tx:w:score SCORE 0.5
        r multi
        r gpredict tx:w:score SCORE 0.5
        assert_equal {} [r exec]
        assert_equal [expr {$pending + 2}] [gcmd_info tx:w:score pending]
        # a refused command of the other connection changes nothing for us
        r watch tx:w:score
        catch {$other gpredict tx:w:score SCORE 9}
        catch {$other gcreate tx:w:score ALPHA 0.2 SCORE}
        $other glearn tx:w:score not-an-id CORRECT 1
        r multi
        r gpredict tx:w:score SCORE 0.5
        assert_equal 1 [llength [r exec]]
        $other close
        r del tx:w:score
    }

    gcmd_test {an ACCEPT, a refused command and GINFO leave nothing to replicate or to save} {
        # SPEC 3 "Redis": "An ACCEPT is not replicated and not written to
        # the AOF". What a server has to send on or to save shows, on a
        # server alone, in its count of changes since the last save.
        r del tx:d:score tx:d:cls tx:d:new
        r gcreate tx:d:score ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate tx:d:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 a b
        set waiting [lindex [r gpredict tx:d:score SCORE 0.5] 1]
        foreach cmd [list \
            {ginfo tx:d:score} {ginfo tx:d:cls} {ginfo tx:proven} {ginfo tx:d:new} \
            {gcreate tx:d:score ALPHA 0.2 SCORE} {gcreate tx:d:new ALPHA 7 SCORE} \
            {gpredict tx:d:score SCORE 7} {gpredict tx:d:score TEXT hello} {gpredict tx:d:new SCORE 0.5} \
            {gpredict tx:d:cls TEXT hello FEATURES 2 x 1 x 2} \
            [list glearn tx:d:score $waiting CORRECT 2] {glearn tx:d:score not-an-id CORRECT 1} \
            {gadd tx:d:score TEXT hello LABEL a} {gadd tx:d:cls TEXT hello LABEL nosuch} \
            {gpause tx:d:new} {gresume tx:d:new} {greset tx:d:new} {gpause tx:d:score extra} \
        ] {
            set before [s rdb_changes_since_last_save]
            catch {r {*}$cmd}
            assert_equal [expr {[s rdb_changes_since_last_save] - $before}] 0 "changes to save after: $cmd"
        }
        # the requests a gate answers by itself, against those it records
        set accepts 0
        for {set i 0} {$i < 200 && $accepts < 20} {incr i} {
            set before [s rdb_changes_since_last_save]
            lassign [r gpredict tx:proven SCORE 0.9] action id
            set changes [expr {[s rdb_changes_since_last_save] - $before}]
            if {$action eq "ACCEPT"} {
                incr accepts
                assert_equal $changes 0 "changes to save after an ACCEPT"
            } else {
                assert_equal AUDIT $action
                assert {$changes > 0}
                r glearn tx:proven $id CORRECT 1
            }
        }
        assert_equal 20 $accepts
        # ... and everything that changes a gate is a change to save
        foreach cmd [list \
            {gpredict tx:d:score SCORE 0.5} [list glearn tx:d:score $waiting CORRECT 1] \
            {gpredict tx:d:cls TEXT hello} {gadd tx:d:cls TEXT hello LABEL a} \
            {gpause tx:d:score} {gresume tx:d:score} {greset tx:d:score} \
            {gcreate tx:d:new ALPHA 0.2 SCORE} {gpredict tx:full SCORE 0.5} \
        ] {
            set before [s rdb_changes_since_last_save]
            r {*}$cmd
            if {$cmd eq {gpredict tx:full SCORE 0.5}} {
                # a full gate records nothing either
                assert_equal [expr {[s rdb_changes_since_last_save] - $before}] 0 "changes to save after: $cmd"
            } else {
                assert {[s rdb_changes_since_last_save] > $before}
            }
        }
        r del tx:d:score tx:d:cls tx:d:new
    }

    foreach proto [gcmd_protocols] {
        gcmd_test "client-side caching (RESP$proto): a tracked GINFO is invalidated by what changes the gate" {
            r del tx:t:score tx:t:other
            r gcreate tx:t:score ALPHA 0.2 DELTA 0.2 SCORE
            r set tx:t:other 1
            # 'c' reads and tracks; in RESP2 the invalidations go to a
            # second connection, in RESP3 they are pushed to 'c' itself.
            set c [redis_client]
            if {$proto == 2} {
                set inv [redis_deferring_client]
                $inv client id
                set redirect [$inv read]
                $inv subscribe __redis__:invalidate
                $inv read
                $c client tracking on redirect $redirect
            } else {
                $c hello 3
                $c client tracking on
                set inv $c
            }
            # requests and answers
            set id [lindex [r gpredict tx:t:score SCORE 0.5] 1]
            set m tx:t:other
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {gpredict tx:t:score SCORE 0.5}]
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m [list glearn tx:t:score $id CORRECT 1]]
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {gpause tx:t:score}]
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {gresume tx:t:score}]
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {greset tx:t:score}]
            # A request the gate answers by itself moves the counters GINFO
            # shows; so do a spot check, a request below the threshold and
            # a request to a full gate.
            set seen {}
            for {set i 0} {$i < 200 && [llength [lsort -unique $seen]] < 2} {incr i} {
                $c ginfo tx:proven
                lassign [r gpredict tx:proven SCORE 0.9] action id
                lappend seen $action
                assert_equal [gcmd_invalidated $inv $proto] tx:proven "GPREDICT replying $action"
                if {$action eq "AUDIT"} {r glearn tx:proven $id CORRECT 1}
            }
            assert_equal {ACCEPT AUDIT} [lsort -unique $seen]
            $c ginfo tx:proven
            lassign [r gpredict tx:proven SCORE 0.05] action id
            assert_equal ABSTAIN $action
            assert_equal tx:proven [gcmd_invalidated $inv $proto]
            if {$id ne {}} {r glearn tx:proven $id CORRECT 0}
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:full $m {gpredict tx:full SCORE 0.5}]

            # what is refused or finds nothing to do invalidates nothing
            foreach cmd {
                {gcreate tx:t:score ALPHA 0.2 SCORE}
                {gpredict tx:t:score SCORE 7}
                {gpredict tx:t:score TEXT hello}
                {glearn tx:t:score not-an-id CORRECT 1}
                {glearn tx:t:score 0123456789abcdef-1 CORRECT 2}
                {gadd tx:t:score TEXT hello LABEL a}
                {ginfo tx:t:score}
                {ginfo tx:t:score extra}
                {gpause tx:t:score extra}
            } {
                assert_equal [gcmd_invalidates $c $inv $proto tx:t:score $m $cmd] 0 "command: $cmd"
            }
            # DEL and GCREATE: the key goes and comes
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {del tx:t:score}]
            assert_equal 0 [gcmd_invalidates $c $inv $proto tx:t:score $m {gpredict tx:t:score SCORE 0.5}]
            assert_equal 0 [gcmd_invalidates $c $inv $proto tx:t:score $m {gcreate tx:t:score ALPHA 7 SCORE}]
            assert_equal 1 [gcmd_invalidates $c $inv $proto tx:t:score $m {gcreate tx:t:score ALPHA 0.2 SCORE}]

            if {$proto == 2} {$inv close}
            $c close
            r del tx:t:score tx:t:other
        }
    }

    foreach proto [gcmd_protocols] {
        if {[catch {set fd [gcmd_raw_open $proto]} err]} {
            gcmd_test "RESP$proto: a connection for transactions and scripts" {fail $err}
            continue
        }

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
            gcmd_raw $fd DEL tx:t:cls tx:t:score
            gcmd_raw $fd GCREATE tx:t:cls ALPHA 0.1 CLASSES 2 a b
            gcmd_raw $fd GCREATE tx:t:score ALPHA 0.1 SCORE
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
            gcmd_lua_fresh $fd
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
            gcmd_lua_fresh $fd
            set samples [gcmd_info lua:score calibration-samples]
            assert_equal {int 1} [gcmd_raw $fd EVAL $script 1 lua:score 0.5 1]
            assert_equal {int 1} [gcmd_raw $fd EVAL $script 1 lua:score 0.6 0]
            assert_equal {bulk {no feedback wanted}} [gcmd_raw $fd EVAL $script 1 tx:full 0.5 1]
            assert_equal [expr {$samples + 2}] [gcmd_info lua:score calibration-samples]
            assert_equal 0 [gcmd_info lua:score pending]
        }

        gcmd_test "RESP$proto: a script asks, answers out of order and reads GINFO" {
            # An answer is used at once and no longer waits for GLEARN
            # (pending); it counts as proof once the answers of the older
            # requests are there (calibration-samples).
            gcmd_lua_fresh $fd
            set script {
                local function field(name)
                    local info = redis.call('GINFO', KEYS[1])
                    for i = 1, #info, 2 do
                        if info[i] == name then return info[i + 1] end
                    end
                    return 'no field ' .. name
                end
                local ids = {}
                for i = 1, 3 do
                    ids[i] = redis.call('GPREDICT', KEYS[1], 'SCORE', ARGV[1])[2]
                end
                local out = {field('pending')}
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[3], 'CORRECT', '1')
                out[#out + 1] = field('pending')
                out[#out + 1] = field('calibration-samples')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[2], 'CORRECT', '0')
                out[#out + 1] = field('pending')
                out[#out + 1] = field('calibration-samples')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[3], 'CORRECT', '1')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[1], 'CORRECT', '1')
                out[#out + 1] = field('pending')
                out[#out + 1] = field('calibration-samples')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[1], 'CORRECT', '1')
                return out
            }
            set want {}
            foreach n {3  1 2 0  1 1 0  0  1 0 3  0} {lappend want [list int $n]}
            assert_equal [list array $want] [gcmd_raw $fd EVAL $script 1 lua:score 0.5]
            set info [r ginfo lua:score]
            assert_equal 3 [dict get $info requests]
            assert_equal 0 [dict get $info pending]
            # again: the gate goes on from where the script left it
            set want {}
            foreach n {3  1 2 3  1 1 3  0  1 0 6  0} {lappend want [list int $n]}
            assert_equal [list array $want] [gcmd_raw $fd EVAL $script 1 lua:score 0.5]

            # The same in a script that asks for RESP3 and reads the map; a
            # classifier that tests nothing learns from each answer as it comes.
            set script {
                redis.setresp(3)
                local function field(name)
                    return redis.call('GINFO', KEYS[1]).map[name]
                end
                local ids = {}
                for i = 1, 3 do
                    ids[i] = redis.call('GPREDICT', KEYS[1], 'TEXT', ARGV[i])[2]
                end
                local out = {field('pending'), field('learned')}
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[2], 'LABEL', 'b')
                out[#out + 1] = field('pending')
                out[#out + 1] = field('learned')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[3], 'LABEL', 'a')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[1], 'LABEL', 'a')
                out[#out + 1] = redis.call('GLEARN', KEYS[1], ids[2], 'LABEL', 'a')
                out[#out + 1] = field('pending')
                out[#out + 1] = field('learned')
                out[#out + 1] = field('state')
                return out
            }
            set want {}
            foreach n {3 0  1 2 1  1 1 0  0 3} {lappend want [list int $n]}
            lappend want {bulk COLD}
            assert_equal [list array $want] [gcmd_raw $fd EVAL $script 1 lua:cls one two three]
        }

        gcmd_test "RESP$proto: errors of gate commands in a script" {
            gcmd_lua_fresh $fd
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
            gcmd_lua_fresh $fd
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
# 8. The fixed choices of SPEC.md section 5, as far as commands show them
# ============================================================================
#
# (The limits on waiting requests are with GPREDICT in section 1; the
# feedback timeout and the answers missing in a row need real time and are
# in section 9.)

# Ask the examples of gcmd_example, answer each one right, and return for
# "spam" and for "ham" how many right answers that label had been given when
# its threshold appeared.
proc gcmd_count_proof {key {max 2000}} {
    set given {spam 0 ham 0}
    set at {}
    for {set i 0} {$i < $max} {incr i} {
        foreach label {spam ham} threshold [lrange [gcmd_info $key thresholds] 0 1] {
            if {$threshold ne {} && ![dict exists $at $label]} {
                dict set at $label [dict get $given $label]
            }
        }
        if {[dict size $at] == 2} {return $at}
        lassign [gcmd_example $i] text label
        lassign [r gpredict $key TEXT $text] action id
        if {$action eq "ACCEPT"} continue
        if {$id eq {}} {fail "$key: request $i got no id: [r ginfo $key]"}
        r glearn $key $id LABEL $label
        # a spot check of a label that is answered already proves nothing new
        if {$action eq "ABSTAIN"} {dict incr given $label}
    }
    fail "$key: a label is not proven after $max requests: [r ginfo $key]"
}

# SPEC 5, "drift detector": likelihood ratios for 2 and 4 times the error
# rate (at most halfway from the error rate to 1), averaged, over blocks of
# 64, 512 and 4,096 spot checks in the order they were asked; a spot check
# counts as right until its answer says otherwise; the alarm is at 1,000,
# and at 10^9 with lost answers counted as disagreements (CHANGES 9f).
#
# The logarithm of the statistic of a block in which 'asked' spot checks
# were asked so far and 'against' of them count against the gate: each of
# those multiplies a ratio by p / alpha, each of the others by
# (1 - p) / (1 - alpha).
proc gcmd_drift_log_stat {alpha asked against} {
    set logs {}
    foreach times {2 4} {
        set p [expr {min($times * $alpha, 0.5 * (1 + $alpha))}]
        lappend logs [expr {$against * log($p / $alpha) +
                            ($asked - $against) * log((1 - $p) / (1 - $alpha))}]
    }
    lassign $logs a b
    set top [expr {max($a, $b)}]
    expr {$top + log((exp($a - $top) + exp($b - $top)) / 2)}
}

# A detector of our own, written from that sentence and kept as a dict: the
# error rate, how many spot checks were asked, and for each of the three
# block lengths the disagreements and the lost answers of the block that is
# open.
proc gcmd_drift_new {alpha} {
    dict create alpha $alpha asked 0 wrong {0 0 0} lost {0 0 0}
}

# A spot check is asked. Returns its number: 1, 2, 3, ...
proc gcmd_drift_ask {name} {
    upvar 1 $name d
    set asked [dict get $d asked]
    set wrong {}
    set lost {}
    foreach length {64 512 4096} w [dict get $d wrong] l [dict get $d lost] {
        # the first spot check of a block opens it
        if {$asked % $length == 0} {
            set w 0
            set l 0
        }
        lappend wrong $w
        lappend lost $l
    }
    dict set d wrong $wrong
    dict set d lost $lost
    dict set d asked [incr asked]
    return $asked
}

# The answer of spot check 'number' disagreed ('what' is wrong) or was given
# up ('what' is lost). Returns "worse" when the disagreements of a block
# reach 1,000, else "unchecked" when its disagreements and lost answers
# together reach 10^9, else {}. An answer whose block has ended counts only
# in the longer blocks that are still open.
proc gcmd_drift_tell {name number what} {
    upvar 1 $name d
    set alpha [dict get $d alpha]
    set asked [dict get $d asked]
    set wrong {}
    set lost {}
    set verdict {}
    foreach length {64 512 4096} w [dict get $d wrong] l [dict get $d lost] {
        if {($number - 1) / $length == ($asked - 1) / $length} {
            set open [expr {($asked - 1) % $length + 1}]
            if {$what eq "wrong"} {incr w} else {incr l}
            if {$what eq "wrong" && [gcmd_drift_log_stat $alpha $open $w] >= log(1000)} {
                set verdict worse
            }
            if {$verdict ne "worse" && [gcmd_drift_log_stat $alpha $open [expr {$w + $l}]] >= log(1e9)} {
                set verdict unchecked
            }
        }
        lappend wrong $w
        lappend lost $l
    }
    dict set d wrong $wrong
    dict set d lost $lost
    return $verdict
}

# Spot checks asked and answered one after the other, the first of them the
# first of a block ('outcomes': 1 = the gate was wrong): which of them is
# the one that raises the alarm, 0 if none does.
proc gcmd_drift_alarm_at {alpha outcomes} {
    set d [gcmd_drift_new $alpha]
    set n 0
    foreach wrong $outcomes {
        incr n
        set number [gcmd_drift_ask d]
        if {$wrong && [gcmd_drift_tell d $number wrong] eq "worse"} {return $n}
    }
    return 0
}

# 'right' spot checks that were answered and agreed, then 'asked' more that
# nobody answers and that are given up one after the other, the oldest
# first: which of those lost answers is the one after which the gate can no
# longer be checked, 0 if none is.
proc gcmd_drift_lost_at {alpha asked {right 0}} {
    set d [gcmd_drift_new $alpha]
    for {set i 0} {$i < $right} {incr i} {gcmd_drift_ask d}
    set numbers {}
    for {set i 0} {$i < $asked} {incr i} {lappend numbers [gcmd_drift_ask d]}
    set n 0
    foreach number $numbers {
        incr n
        if {[gcmd_drift_tell d $number lost] eq "unchecked"} {return $n}
    }
    return 0
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77009

    gcmd_test {SPEC 5: the helpers of these tests compute what the table says} {
        # ALPHA 0.2: a right guess multiplies the four bets by 1.025,
        # 1.0625, 1.125 and 1.225. DELTA 0.2, snapshot 1, one label: the bar
        # is 32 x 1 x 2 / 0.2 = 320. After 34 right answers the average
        # wealth is 264.3, after 35 it is 322.0.
        assert_equal 35 [gcmd_proof_needed 0.2 0.2]
        # two labels: 640, three: 960; snapshot 2: g (g + 1) = 6 for 2
        assert_equal 39 [gcmd_proof_needed 0.2 0.2 1 2]
        assert_equal 41 [gcmd_proof_needed 0.2 0.2 1 3]
        assert_equal 41 [gcmd_proof_needed 0.2 0.2 2]
        # a wrong answer first leaves each bet 1 - c of its wealth
        assert_equal 47 [gcmd_proof_needed 0.2 0.2 1 1 1]
        # ---- the drift detector, by hand
        # 0.4 and 0.6 against 0.2: a wrong spot check multiplies the two
        # ratios by 2 and 3, a right one by 0.75 and 0.5. In a block of
        # nothing but wrong ones: (2^6 + 3^6) / 2 = 396.5, (2^7 + 3^7) / 2
        # = 1157.5.
        assert_equal 7 [gcmd_drift_alarm_at 0.2 {1 1 1 1 1 1 1 1 1 1}]
        assert_equal 0 [gcmd_drift_alarm_at 0.2 {1 1 1 1 1 1}]
        assert {abs(exp([gcmd_drift_log_stat 0.2 6 6]) - 396.5) < 1e-6}
        assert {abs(exp([gcmd_drift_log_stat 0.2 7 7]) - 1157.5) < 1e-6}
        # One right spot check in the block, wherever it is (a block does
        # not start over): (2^7 x 0.75 + 3^7 x 0.5) / 2 = 594.75 after the
        # seventh wrong one, (2^8 x 0.75 + 3^8 x 0.5) / 2 = 1736.25 after
        # the eighth, which is the ninth spot check.
        assert {abs(exp([gcmd_drift_log_stat 0.2 8 7]) - 594.75) < 1e-6}
        assert {abs(exp([gcmd_drift_log_stat 0.2 9 8]) - 1736.25) < 1e-6}
        assert_equal 9 [gcmd_drift_alarm_at 0.2 {0 1 1 1 1 1 1 1 1 1 1}]
        assert_equal 9 [gcmd_drift_alarm_at 0.2 {1 1 1 0 1 1 1 1 1 1 1}]
        assert_equal 9 [gcmd_drift_alarm_at 0.2 {1 1 1 1 1 1 0 1 1 1 1}]
        # Five right ones first: (2^9 x 0.75^5 + 3^9 x 0.5^5) / 2 = 368.3
        # after the ninth wrong one, (2^10 x 0.75^5 + 3^10 x 0.5^5) / 2 =
        # 1044.1 after the tenth.
        assert_equal 15 [gcmd_drift_alarm_at 0.2 {0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1}]
        # 0.2 and 0.4 against 0.1 (or 0.1 and 0.2 against 0.05): 2 and 4;
        # (2^5 + 4^5) / 2 = 528, (2^6 + 4^6) / 2 = 2080.
        assert_equal 6 [gcmd_drift_alarm_at 0.1 {1 1 1 1 1 1 1 1 1 1}]
        assert_equal 6 [gcmd_drift_alarm_at 0.05 {1 1 1 1 1 1 1 1 1 1}]
        # Against 0.5 both alternatives are 0.75, halfway to 1: 1.5 for a
        # wrong spot check; 1.5^17 = 985.3, 1.5^18 = 1477.9.
        assert_equal 18 [gcmd_drift_alarm_at 0.5 [lrepeat 30 1]]
        # Blocks of 64. After 60 right spot checks the four wrong ones
        # that end the block count for nothing in it ((2^4 x 0.75^60 +
        # 3^4 x 0.5^60) / 2 is less than a millionth); the next block
        # starts with spot check 65 and its seventh wrong one is the 71st.
        # In the blocks of 512 and 4,096 the 60 right ones still count.
        assert_equal 71 [gcmd_drift_alarm_at 0.2 [concat [lrepeat 60 0] [lrepeat 40 1]]]
        assert_equal 71 [gcmd_drift_alarm_at 0.2 [concat [lrepeat 64 0] [lrepeat 40 1]]]
        # (the same after 57 right ones: the seven wrong ones that end
        # the first block are forgotten when the next one begins)
        assert_equal 71 [gcmd_drift_alarm_at 0.2 [concat [lrepeat 57 0] [lrepeat 40 1]]]
        # A rate that is only moderately too high is for the longer
        # blocks. Three wrong ones in every eight: the first block of 64
        # gets no further than (2^24 x 0.75^35 + 3^24 x 0.5^35) / 2 = 359
        # (at its 59th spot check); the block of 512 has (2^30 x 0.75^45 +
        # 3^30 x 0.5^45) / 2 = 1284 at spot check 75, the 30th wrong one.
        assert {abs(exp([gcmd_drift_log_stat 0.2 59 24]) - 359) < 1}
        assert {abs(exp([gcmd_drift_log_stat 0.2 75 30]) - 1284) < 1}
        assert_equal 75 [gcmd_drift_alarm_at 0.2 [concat {*}[lrepeat 12 {1 1 1 0 0 0 0 0}]]]
        # ---- lost answers, counted as disagreements, against 10^9:
        # (2^19 + 3^19) / 2 = 5.8 x 10^8, (2^20 + 3^20) / 2 = 1.74 x 10^9.
        assert_equal 0 [gcmd_drift_lost_at 0.2 19]
        assert_equal 20 [gcmd_drift_lost_at 0.2 20]
        # Asked before any of them was given up, the others still count
        # as right: of 32, 24 lost are (2^24 x 0.75^8 + 3^24 x 0.5^8) / 2
        # = 5.5 x 10^8 and 25 lost are (2^25 x 0.75^7 + 3^25 x 0.5^7) / 2
        # = 3.3 x 10^9.
        assert_equal 25 [gcmd_drift_lost_at 0.2 32]
        # After five right ones: 22 lost are (2^22 x 0.75^5 + 3^22 x
        # 0.5^5) / 2 = 4.9 x 10^8, 23 lost are 1.47 x 10^9.
        assert_equal 0 [gcmd_drift_lost_at 0.2 22 5]
        assert_equal 23 [gcmd_drift_lost_at 0.2 23 5]
        # (2^15 + 4^15) / 2 = 5.4 x 10^8, (2^16 + 4^16) / 2 = 2.1 x 10^9
        assert_equal 0 [gcmd_drift_lost_at 0.05 15]
        assert_equal 16 [gcmd_drift_lost_at 0.05 16]
        # the 32 thresholds of a score gate: 0, 0.1817, 0.3303, ... 0.998
        assert_equal 0 [gcmd_grid_index 0 0.998 0]
        assert_equal 1 [gcmd_grid_index 0 0.998 0.18165496569273065]
        assert_equal 31 [gcmd_grid_index 0 0.998 0.998]
        assert_equal -1 [gcmd_grid_index 0 0.998 0.5]
    }

    gcmd_test {SPEC 5: a score gate is proven by exactly the right answers the betting test asks for} {
        # Nothing but right answers: every threshold is proven at the same
        # moment, and the gate answers from the lowest one, 0.
        foreach {alpha delta} {0.2 0.2  0.1 0.05  0.5 0.9  0.05 0.01} {
            set shown "ALPHA $alpha DELTA $delta"
            set need [gcmd_proof_needed $alpha $delta]
            r del fx:score
            r gcreate fx:score ALPHA $alpha DELTA $delta SCORE
            assert_equal [lsort -unique [gcmd_ask_score fx:score [expr {$need - 1}] 0.7 1]] ABSTAIN $shown
            set info [r ginfo fx:score]
            assert_equal [dict get $info state] CALIBRATING "$shown: after [expr {$need - 1}] right answers"
            assert_equal [dict get $info thresholds] {{}} "$shown: after [expr {$need - 1}] right answers"
            assert_equal [dict get $info calibration-samples] [expr {$need - 1}] $shown
            gcmd_ask_score fx:score 1 0.7 1
            set info [r ginfo fx:score]
            assert_equal [dict get $info state] ACTIVE "$shown: after $need right answers"
            assert {[lindex [dict get $info thresholds] 0] == 0}
            assert_equal [dict get $info generation] 1 $shown
            assert_equal [dict get $info candidate] 0 $shown
            assert_equal [dict get $info calibration-samples] $need $shown
        }
        r del fx:score
    }

    gcmd_test {SPEC 5: answers in any order prove what answers in request order prove; a retry changes nothing} {
        # "Proof ... is counted in request order, so a slow teacher cannot
        # bias it." GLEARN: "0 if the id is ... already used. Retrying is
        # always safe", also while the answer still waits for its turn.
        set need [gcmd_proof_needed 0.2 0.2]
        r del fx:order
        r gcreate fx:order ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        for {set i 0} {$i < $need} {incr i} {lappend ids [lindex [r gpredict fx:order SCORE 0.7] 1]}
        # the newest first, the oldest last; each one retried with the
        # opposite answer
        foreach id [lreverse [lrange $ids 1 end]] {
            assert_equal 1 [r glearn fx:order $id CORRECT 1]
            assert_equal 0 [r glearn fx:order $id CORRECT 0]
        }
        set info [r ginfo fx:order]
        assert_equal 1 [dict get $info pending]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal CALIBRATING [dict get $info state]
        gcmd_check_saveable fx:order
        assert_equal 1 [r glearn fx:order [lindex $ids 0] CORRECT 1]
        assert_equal 0 [r glearn fx:order [lindex $ids 0] CORRECT 0]
        # exactly the right answers a threshold needs, and it is proven:
        # none of them was turned into a wrong one
        set info [r ginfo fx:order]
        assert_equal $need [dict get $info calibration-samples]
        assert_equal ACTIVE [dict get $info state]
        assert {[lindex [dict get $info thresholds] 0] == 0}
        assert_equal 0 [dict get $info pending]
        r del fx:order
    }

    gcmd_test {SPEC 5: a wrong answer costs each bet its stake; a later snapshot has a higher bar} {
        r del fx:score
        r gcreate fx:score ALPHA 0.2 DELTA 0.2 SCORE
        # snapshot 1 after one and after three wrong answers; snapshots 2
        # and 3 (GRESET never hands budget back: g (g + 1) is 6, then 12)
        foreach {generation errors} {1 1  2 0  3 3  4 1} {
            set shown "snapshot $generation after $errors wrong answers"
            assert_equal [gcmd_info fx:score candidate] $generation $shown
            set need [gcmd_proof_needed 0.2 0.2 $generation 1 $errors]
            assert {$need > [gcmd_proof_needed 0.2 0.2]}
            gcmd_ask_score fx:score $errors 0.7 0
            gcmd_ask_score fx:score [expr {$need - 1}] 0.7 1
            assert_equal [gcmd_info fx:score thresholds] {{}} "$shown and [expr {$need - 1}] right ones"
            gcmd_ask_score fx:score 1 0.7 1
            set info [r ginfo fx:score]
            assert_equal [dict get $info generation] $generation "$shown and $need right ones"
            assert {[lindex [dict get $info thresholds] 0] == 0}
            assert_equal OK [r greset fx:score]
        }
        r del fx:score
    }

    gcmd_test {SPEC 5: the thresholds of a score gate are 32 steps from 0 to 0.998, even in log(1 - threshold)} {
        # The cheap result is wrong at the score 'wrong' and right at
        # 'right': the gate answers from the lowest of the 32 thresholds
        # above 'wrong', which is step 'step'. Only right answers count for
        # that threshold, so it takes as many as for a gate that is never
        # wrong.
        set need [gcmd_proof_needed 0.2 0.2]
        foreach {wrong right step} {
            0.1 0.9 1      0.18 0.19 1    0.19 0.9 2    0.5 0.9 4
            0.95 0.999 15  0.9975 1 30    0.9976 0.998 31
        } {
            set shown "wrong at $wrong, right at $right"
            r del fx:grid
            r gcreate fx:grid ALPHA 0.2 DELTA 0.2 SCORE
            for {set n 1} {$n <= $need} {incr n} {
                assert_equal [gcmd_info fx:grid thresholds] {{}} "$shown: after [expr {$n - 1}] right answers"
                gcmd_ask_score fx:grid 1 $wrong 0
                gcmd_ask_score fx:grid 1 $right 1
            }
            set threshold [lindex [gcmd_info fx:grid thresholds] 0]
            assert {$threshold ne {}}
            assert_equal [gcmd_grid_index 0 0.998 $threshold] $step "$shown: the threshold $threshold"
            assert {$threshold > $wrong && $threshold <= $right}
            # "lowest score the gate currently answers at": it answers
            # there, and not a hair below.
            foreach action [gcmd_ask_score fx:grid 20 $threshold 1] {
                assert {$action in {ACCEPT AUDIT}}
            }
            foreach action [gcmd_ask_score fx:grid 20 [expr {$threshold - 1e-9}] 0] {
                assert_equal $action ABSTAIN "$shown: just below the threshold $threshold"
            }
        }
        # the top step is 0.998 itself
        assert {$threshold == 0.998}
        r del fx:grid
    }

    gcmd_test {SPEC 3: a gate keeps testing lower thresholds, and answers from one as soon as it is proven} {
        # "The gate answers at the lowest proven threshold of each label,
        # and keeps testing lower ones, so coverage grows over time."
        # With AUDIT 1 every request is proof. At first the cheap result
        # is wrong at 0.5 and right at 0.9: the threshold is step 4.
        set need [gcmd_proof_needed 0.2 0.2]
        r del fx:down
        r gcreate fx:down ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        for {set n 0} {$n < $need} {incr n} {
            gcmd_ask_score fx:down 1 0.5 0
            gcmd_ask_score fx:down 1 0.9 1
        }
        set first [lindex [gcmd_info fx:down thresholds] 0]
        assert_equal 4 [gcmd_grid_index 0 0.998 $first]
        assert_equal ABSTAIN [gcmd_ask_score fx:down 1 0.5 1]
        # Then it is right at 0.5 as well. Every threshold below 0.5 has
        # seen the same answers: $need wrong ones, and one right one more
        # than the wrong ones so far. They are proven together when the
        # bets have made up for the wrong answers.
        set more [expr {[gcmd_proof_needed 0.2 0.2 1 1 $need] - $need - 1}]
        assert {$more > 100 && $more < 1000}
        assert_equal ABSTAIN [lsort -unique [gcmd_ask_score fx:down [expr {$more - 1}] 0.5 1]]
        set info [r ginfo fx:down]
        assert_equal [lindex [dict get $info thresholds] 0] $first "after [expr {$more - 1}] right answers at 0.5"
        assert_equal 1 [dict get $info generation]
        gcmd_ask_score fx:down 1 0.5 1
        set info [r ginfo fx:down]
        # the same snapshot answers, from 0 up now
        assert {[lindex [dict get $info thresholds] 0] == 0}
        assert_equal 1 [dict get $info generation]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 0 [dict get $info revocations]
        foreach score {0.5 0.3 0} {
            assert_equal AUDIT [lsort -unique [gcmd_ask_score fx:down 5 $score 1]]
        }
        r del fx:down
    }

    gcmd_test {SPEC 5: a classifier label is proven by exactly the right guesses the betting test asks for} {
        # The bar counts the labels the gate may answer: 2 here, also with
        # a third label that is always deferred, 3 with a third label that
        # is not. A label with a stricter error rate needs more.
        foreach {labels options spam ham} [list \
            {spam ham}       {}                          [gcmd_proof_needed 0.2 0.2 1 2] [gcmd_proof_needed 0.2 0.2 1 2] \
            {spam ham other} {ALWAYSDEFER 1 other}       [gcmd_proof_needed 0.2 0.2 1 2] [gcmd_proof_needed 0.2 0.2 1 2] \
            {spam ham other} {}                          [gcmd_proof_needed 0.2 0.2 1 3] [gcmd_proof_needed 0.2 0.2 1 3] \
            {spam ham}       {CLASSALPHA 1 spam 0.1}     [gcmd_proof_needed 0.1 0.2 1 2] [gcmd_proof_needed 0.2 0.2 1 2] \
            {spam ham}       {CLASSALPHA 2 ham 0.05 spam 0.1} [gcmd_proof_needed 0.1 0.2 1 2] [gcmd_proof_needed 0.05 0.2 1 2] \
        ] {
            set shown "CLASSES $labels $options"
            r del fx:cls
            gcmd_create_fast fx:cls ALPHA 0.2 DELTA 0.2 CLASSES [llength $labels] {*}$labels {*}$options
            gcmd_teach fx:cls 40
            set at [gcmd_count_proof fx:cls]
            assert_equal [dict get $at spam] $spam "$shown: right guesses of spam"
            assert_equal [dict get $at ham] $ham "$shown: right guesses of ham"
            # with nothing but right guesses the lowest threshold is proven
            set lowest [expr {max(1.0 / [llength $labels], 0.25)}]
            foreach threshold [lrange [gcmd_info fx:cls thresholds] 0 1] {
                assert_equal [gcmd_grid_index $lowest 0.999 $threshold] 0 "$shown: the threshold $threshold"
            }
            assert_equal [lindex [gcmd_info fx:cls thresholds] 2] {} $shown
        }
        r del fx:cls
    }

    gcmd_test {SPEC 5: the lowest threshold of a classifier is max(1 / classes, 0.25)} {
        foreach {classes lowest} {2 0.5  3 0.3333333333333333  4 0.25  5 0.25  16 0.25} {
            set labels {spam ham}
            for {set i 2} {$i < $classes} {incr i} {lappend labels extra$i}
            r del fx:cls
            gcmd_create_fast fx:cls ALPHA 0.2 DELTA 0.2 CLASSES $classes {*}$labels
            gcmd_teach fx:cls 40
            set at [gcmd_count_proof fx:cls]
            assert_equal [dict get $at spam] [gcmd_proof_needed 0.2 0.2 1 $classes] "$classes labels"
            lassign [gcmd_info fx:cls thresholds] spam ham
            assert {abs($spam - $lowest) < 1e-12 && abs($ham - $lowest) < 1e-12}
            assert_equal ACTIVE [gcmd_info fx:cls state]
        }
        r del fx:cls
    }

    gcmd_test {SPEC 5: the first snapshot is taken after 1,000 examples, the next after 1,000 more} {
        # (the default of gate.snapshot-examples)
        assert_equal {gate.snapshot-examples 1000} [r config get gate.snapshot-examples]
        r del fx:default
        r gcreate fx:default ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        assert_equal 999 [gcmd_teach_fast fx:default 999]
        set info [r ginfo fx:default]
        assert_equal COLD [dict get $info state]
        assert_equal 0 [dict get $info candidate]
        assert_equal 1000 [r gadd fx:default TEXT "buy cheap pills now" LABEL spam]
        set info [r ginfo fx:default]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 1 [dict get $info candidate]
        # While a snapshot is being tested no other is taken.
        assert_equal 2500 [gcmd_teach_fast fx:default 1500]
        assert_equal 1 [gcmd_info fx:default candidate]
        # It takes over; every answer on the way was an example too.
        gcmd_count_proof fx:default
        set info [r ginfo fx:default]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 1 [dict get $info generation]
        # 2,500 examples and the answers since the snapshot at 1,000: the
        # next one was due, and is taken as soon as nothing is being tested.
        assert_equal 2 [dict get $info candidate]
        set learned [dict get $info learned]
        assert {$learned > 2500 && $learned < 3000}
        # the one after that needs 1,000 examples more than the gate had then
        r greset fx:default
        assert_equal 1000 [gcmd_teach_fast fx:default 1000]
        assert_equal 3 [gcmd_info fx:default candidate]
        gcmd_count_proof fx:default
        set info [r ginfo fx:default]
        assert_equal 3 [dict get $info generation]
        assert_equal 0 [dict get $info candidate]
        set learned [dict get $info learned]
        gcmd_teach_fast fx:default [expr {1999 - $learned}]
        assert_equal 0 [gcmd_info fx:default candidate]
        assert_equal 2000 [r gadd fx:default TEXT "meeting at noon tomorrow" LABEL ham]
        set info [r ginfo fx:default]
        assert_equal 4 [dict get $info candidate]
        assert_equal 3 [dict get $info generation]
        assert_equal ACTIVE [dict get $info state]
        r del fx:default
    }

    gcmd_test {SPEC 5: a snapshot that proves nothing is dropped after 10 times the proof a flawless threshold needs, per label} {
        # ALPHA 0.2, DELTA 0.2, two labels: a threshold without a wrong
        # guess needs 39 right answers (the betting test, as above). The
        # snapshot here guesses "a" and the teacher says "b" every time.
        set need [gcmd_proof_needed 0.2 0.2 1 2]
        set budget [expr {10 * $need * 2}]
        r config set gate.snapshot-examples 4
        r del fx:budget
        r gcreate fx:budget ALPHA 0.2 DELTA 0.2 CLASSES 2 a b
        r config set gate.snapshot-examples 1000
        for {set i 0} {$i < 4} {incr i} {r gadd fx:budget TEXT "alpha one" LABEL a}
        assert_equal 1 [gcmd_info fx:budget candidate]
        set dropped_at 0
        for {set n 1} {$n <= $budget + 10} {incr n} {
            lassign [r gpredict fx:budget TEXT "alpha one"] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn fx:budget $id LABEL b]
            if {[gcmd_info fx:budget candidate] != 1} {
                set dropped_at $n
                break
            }
        }
        set info [r ginfo fx:budget]
        r del fx:budget
        # the next snapshot is taken at once: the gate has learned since
        assert_equal 2 [dict get $info candidate]
        assert_equal 0 [dict get $info generation]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal $dropped_at $budget "pieces of proof before the snapshot was dropped\
            (a flawless threshold needs $need right answers; 10 times that for each of 2 labels)"
    }

    gcmd_test {SPEC 5: a snapshot that leaves everything to the teacher is dropped too, by the requests it could have answered} {
        # "requests it left to the teacher although their label is one the
        # gate may answer count like proof". CHANGES 9c: before, a snapshot
        # that only ever guessed an ALWAYSDEFER label was never dropped.
        # The snapshot here has seen nothing but "b", which is always
        # deferred, and guesses "b" every time. One label may be answered:
        # a flawless threshold needs 35 right answers, and the snapshot
        # gets 10 times that.
        set need [gcmd_proof_needed 0.2 0.2 1 1]
        assert_equal 35 $need
        set budget [expr {10 * $need}]
        r config set gate.snapshot-examples 4
        r del fx:defer
        r gcreate fx:defer ALPHA 0.2 DELTA 0.2 CLASSES 2 a b ALWAYSDEFER 1 b
        r config set gate.snapshot-examples 1000
        for {set i 0} {$i < 4} {incr i} {r gadd fx:defer TEXT "beta two" LABEL b}
        assert_equal 1 [gcmd_info fx:defer candidate]
        # (30 requests that get their answer much later: below)
        set later {}
        for {set i 0} {$i < 30} {incr i} {lappend later [lindex [r gpredict fx:defer TEXT "beta two"] 1]}
        # While the teacher says "b" as well, the gate could not have
        # answered whatever the snapshot guessed: nothing counts, however
        # many such requests there are.
        for {set n 1} {$n <= $budget + 20} {incr n} {
            lassign [r gpredict fx:defer TEXT "beta two"] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn fx:defer $id LABEL b]
        }
        set info [r ginfo fx:defer]
        assert_equal 1 [dict get $info candidate] "after [expr {$budget + 20}] requests whose label is never answered"
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 0 [dict get $info calibration-samples]
        # Now the teacher says "a".
        set dropped_at 0
        for {set n 1} {$n <= $budget + 10} {incr n} {
            lassign [r gpredict fx:defer TEXT "beta two"] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn fx:defer $id LABEL a]
            set info [r ginfo fx:defer]
            if {[dict get $info candidate] != 1} {
                set dropped_at $n
                break
            }
            # (a guess of the label that is never answered is not proof)
            assert_equal 0 [dict get $info calibration-samples]
            # The count is part of the gate: the value holds, one after
            # the other, the proof of the snapshot, its wrong guesses, the
            # requests it left to the teacher, its spot checks and those
            # that disagreed, and what it may use up (gate_codec.c).
            if {$n == 123} {
                assert {[string first [binary format w6 [list 0 0 123 0 0 $budget]] [gcmd_dump_plain fx:defer]] > 0}
            }
        }
        assert_equal $dropped_at $budget "requests the snapshot left to the teacher before it was dropped\
            (a flawless threshold needs $need right answers; 10 times that for the one label)"
        # the next snapshot is taken at once: the gate has learned since
        assert_equal 2 [dict get $info candidate]
        assert_equal 0 [dict get $info generation]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 0 [dict get $info missing]
        assert_equal 30 [dict get $info pending]

        # The second snapshot has learned "a" from those answers and
        # guesses "a". The 30 requests from before get their answer, "a":
        # they were asked while the first snapshot was the one being
        # tested, so they are nothing the second could have been tested
        # on. Then the teacher says "b" again: every guess of the second
        # snapshot is proof, all of it wrong, and it is dropped after 10
        # times what a flawless threshold of snapshot 2 needs, 41: not 30
        # requests sooner.
        foreach id $later {assert_equal 1 [r glearn fx:defer $id LABEL a]}
        set info [r ginfo fx:defer]
        assert_equal 2 [dict get $info candidate]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal 0 [dict get $info pending]
        set need [gcmd_proof_needed 0.2 0.2 2 1]
        assert_equal 41 $need
        set budget [expr {10 * $need}]
        set dropped_at 0
        for {set n 1} {$n <= $budget + 10} {incr n} {
            lassign [r gpredict fx:defer TEXT "beta two"] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn fx:defer $id LABEL b]
            set info [r ginfo fx:defer]
            if {[dict get $info candidate] != 2} {
                set dropped_at $n
                break
            }
            assert_equal $n [dict get $info calibration-samples] "proof of the second snapshot (it guesses 'a')"
        }
        r del fx:defer
        assert_equal $dropped_at $budget "pieces of proof before the second snapshot was dropped"
        assert_equal 3 [dict get $info candidate]
        assert_equal 0 [dict get $info generation]
    }

    gcmd_test {SPEC 5: once a snapshot answers, only spot checks count against the one being tested} {
        # CHANGES 9c: a request counts against a snapshot when it "could
        # have been tested on" it: "asked while it was the candidate, and
        # nothing serving or audit". That is the sample its proof comes
        # from: every request while nothing answers, the spot-checked
        # share afterwards.
        # A classifier of "a" and "b", "b" always deferred. X is "a" to
        # every snapshot and Y is "b" to every snapshot (a named feature
        # decides). The first snapshot proves "a" on X and answers; the
        # second is being tested. Then come requests Y to which the
        # teacher says "a": both snapshots leave them to the teacher.
        # With AUDIT 1 every one of them is a request the second snapshot
        # would have been tested on: it is dropped after 10 times the 41
        # right answers a flawless threshold of snapshot 2 needs. With
        # AUDIT one in a billion none of them is, and nothing counts.
        set x {TEXT {} FEATURES 1 f 2}
        set y {TEXT {} FEATURES 1 f -2}
        assert_equal 35 [gcmd_proof_needed 0.2 0.2 1 1]
        set budget [expr {10 * [gcmd_proof_needed 0.2 0.2 2 1]}]
        assert_equal 410 $budget
        foreach {audit dropped} {1 1  0.000000001 0} {
            set shown "AUDIT $audit"
            r config set gate.snapshot-examples 4
            r del fx:share
            r gcreate fx:share ALPHA 0.2 DELTA 0.2 AUDIT $audit CLASSES 2 a b ALWAYSDEFER 1 b
            r config set gate.snapshot-examples 1000
            foreach form [list $x $y $x $y] label {a b a b} {r gadd fx:share {*}$form LABEL $label}
            assert_equal 1 [gcmd_info fx:share candidate] $shown
            for {set i 0} {$i < 35} {incr i} {
                lassign [r gpredict fx:share {*}$x] action id
                assert_equal ABSTAIN $action "$shown: request X [expr {$i + 1}]"
                assert_equal 1 [r glearn fx:share $id LABEL a]
            }
            set info [r ginfo fx:share]
            assert_equal ACTIVE [dict get $info state] $shown
            assert_equal 1 [dict get $info generation] $shown
            assert_equal 2 [dict get $info candidate] $shown
            set dropped_at 0
            for {set n 1} {$n <= $budget + 40} {incr n} {
                lassign [r gpredict fx:share {*}$y] action id
                # (no snapshot guesses "a" here: nothing to answer, nothing to spot-check)
                assert_equal ABSTAIN $action "$shown: request Y $n"
                assert_equal 1 [r glearn fx:share $id LABEL a]
                set info [r ginfo fx:share]
                if {[dict get $info candidate] != 2} {
                    set dropped_at $n
                    break
                }
                assert_equal 0 [dict get $info calibration-samples] "$shown: request Y $n"
            }
            if {$dropped} {
                assert_equal $budget $dropped_at "$shown: requests the second snapshot left to the teacher before it was dropped"
                assert_equal 3 [dict get $info candidate] $shown
            } else {
                assert_equal 0 $dropped_at "$shown: the second snapshot was dropped by requests it was not to be tested on"
                assert_equal 2 [dict get $info candidate] $shown
            }
            # the first snapshot goes on answering
            assert_equal 1 [dict get $info generation] $shown
            assert_equal ACTIVE [dict get $info state] $shown
            assert_equal 0 [dict get $info revocations] $shown
            gcmd_check_saveable fx:share $shown
        }
        r del fx:share
    }

    gcmd_test {SPEC 3: a newer snapshot takes over only if it answers more than the current one answers now} {
        # "A newer snapshot takes over only when it has proven thresholds
        # of its own and answers more of the same spot-checked requests
        # than the current one." CHANGES 9a: the current snapshot is judged
        # by the thresholds it has NOW. Before, one whose thresholds had
        # moved down since the requests were asked was undercounted and
        # could be replaced by a snapshot that answers no more than it.
        #
        # Two requests, X and Y: the same (empty) text and a named feature
        # the model has learned to lean on. Every snapshot guesses "b" for
        # both and is surer of X. While the first snapshot is tested the
        # teacher says "a" to Y 'errors' times and "b" to X: the thresholds
        # above Y are proven by 39 right answers, those that take Y in
        # need more, so the snapshot answers X only. From then on the
        # teacher says "b" to both, every request is proof for both
        # snapshots (AUDIT 1), and all of it is right:
        #   - the lower thresholds of the first snapshot are proven by
        #     request number need(errors) - 39; from then on it answers Y;
        #   - the second snapshot has its first proven thresholds at
        #     request 44 (snapshot 2, two labels), and they take all 44.
        # After 4 errors the first of these is request 43: at request 44
        # the second snapshot answers no more than the first does now, and
        # does not take over. After 5 errors it is request 53: at request
        # 44 the second snapshot answers X and Y, the first only X, and
        # the second takes over.
        # A third gate: the teacher goes on saying "a" to Y. Neither
        # snapshot ever gets to answer Y; the second answers what the
        # first answers, X, and does not take over however much proof it
        # has (44 right answers to X at request 88).
        set x {TEXT {} FEATURES 1 f 2}
        set y {TEXT {} FEATURES 1 f -0.5}
        assert_equal 39 [gcmd_proof_needed 0.2 0.2 1 2]
        assert_equal 82 [gcmd_proof_needed 0.2 0.2 1 2 4]
        assert_equal 92 [gcmd_proof_needed 0.2 0.2 1 2 5]
        assert_equal 44 [gcmd_proof_needed 0.2 0.2 2 2]
        foreach {errors teacher_y takes_over} {4 b 0  5 b 1  1 a 0} {
            set shown "$errors errors"
            set lower_at [expr {[gcmd_proof_needed 0.2 0.2 1 2 $errors] - 39}]
            r del fx:take
            gcmd_create_fast fx:take ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 a b
            for {set i 0} {$i < 40} {incr i} {r gadd fx:take {*}$x LABEL b}
            assert_equal 1 [gcmd_info fx:take candidate]
            for {set i 0} {$i < $errors} {incr i} {
                lassign [r gpredict fx:take {*}$y] action id
                assert_equal ABSTAIN $action
                assert_equal 1 [r glearn fx:take $id LABEL a]
            }
            for {set i 1} {$i <= 39} {incr i} {
                assert_equal 0 [gcmd_info fx:take generation] "$shown: before right answer $i"
                lassign [r gpredict fx:take {*}$x] action id
                assert_equal ABSTAIN $action
                assert_equal 1 [r glearn fx:take $id LABEL b]
            }
            # The first snapshot answers "b", from a threshold that is not
            # the lowest; the second is being tested.
            set info [r ginfo fx:take]
            assert_equal 1 [dict get $info generation] $shown
            assert_equal 2 [dict get $info candidate] $shown
            lassign [dict get $info thresholds] a high
            assert_equal {} $a
            if {$high eq {} || [gcmd_grid_index 0.5 0.999 $high] < 1} {
                fail "$shown: this test needs a first snapshot that is less sure of Y than of X, with one\
                    of the 32 thresholds between them; its threshold for 'b' is '$high'"
            }
            set steps [gcmd_grid_index 0.5 0.999 $high]
            for {set k 1} {$k <= ($teacher_y eq "b" ? 60 : 100)} {incr k} {
                set shown "$errors errors, then '$teacher_y' for Y, request $k"
                set took_over [expr {$takes_over && $k > 44}]
                set info [r ginfo fx:take]
                assert_equal [expr {$took_over ? 2 : 1}] [dict get $info generation] "$shown: before it"
                if {$k % 2} {
                    # Y: left to the teacher until a snapshot that answers
                    # takes it in (request 43 or 44 made one do so)
                    lassign [r gpredict fx:take {*}$y] action id
                    assert_equal [expr {$teacher_y eq "b" && $k >= 44 ? "AUDIT" : "ABSTAIN"}] $action $shown
                    assert_equal 1 [r glearn fx:take $id LABEL $teacher_y]
                } else {
                    lassign [r gpredict fx:take {*}$x] action id
                    assert_equal AUDIT $action $shown
                    assert_equal 1 [r glearn fx:take $id LABEL b]
                }
                set info [r ginfo fx:take]
                set threshold [lindex [dict get $info thresholds] 1]
                if {$takes_over && $k >= 44} {
                    assert_equal 2 [dict get $info generation] "$shown: the second snapshot answers more"
                    assert_equal 3 [dict get $info candidate] $shown
                    assert {$threshold == 0.5}
                } else {
                    assert_equal 1 [dict get $info generation] "$shown: the second snapshot answers no more than the first"
                    assert_equal 2 [dict get $info candidate] $shown
                    # the proof of the snapshot being tested: every request so far
                    assert_equal $k [dict get $info calibration-samples] $shown
                    if {$teacher_y eq "a" || $k < $lower_at} {
                        assert_equal $high $threshold "$shown: the first snapshot answers X only"
                    } else {
                        assert {$threshold == 0.5}
                    }
                }
                assert_equal 0 [dict get $info revocations] $shown
                assert_equal 0 [dict get $info audit-errors] $shown
                # What the comparison is made with is part of the gate
                # (gate_codec.c, "paired"): for each threshold of "b", how
                # many of the requests so far the first snapshot's own
                # guess put at or above it. Both X and Y are above the
                # thresholds below the one it answered from at first, X
                # alone is above that one and the next. The table is
                # the second snapshot's while it is being tested, and is
                # cleared when it takes over (CHANGES 9a).
                if {$teacher_y eq "b" && $k in {43 44}} {
                    set payload [gcmd_dump_plain fx:take]
                    set table [binary format w* [concat [lrepeat $steps $k] [list [expr {$k / 2}] [expr {$k / 2}]]]]
                    if {$takes_over && $k == 44} {
                        assert_equal -1 [string first $table $payload] "$shown: the table of the snapshot that took over"
                    } else {
                        assert {[string first $table $payload] > 0}
                    }
                }
                # A table that cannot be is refused (CHANGES 9g: "paired
                # is nested and at most samples"): more requests above a
                # threshold than the snapshot has proof, or more above a
                # threshold than above the one below it.
                if {$teacher_y eq "b" && !$takes_over && $k == 43} {
                    set at [string first $table $payload]
                    set body [string range $payload 0 end-8]
                    foreach {what entry value accepted} [list \
                        "one request more above the lowest threshold than the snapshot has proof" 0 [expr {$k + 1}] 0 \
                        "one request more above a threshold than above the one below it" \
                            [expr {$steps + 1}] [expr {$k / 2 + 1}] 0 \
                        "one request less above the second threshold than above the lowest" 1 [expr {$k - 1}] 1 \
                    ] {
                        r del fx:take:copy
                        set offset [expr {$at + 8 * $entry}]
                        set forged [string replace $body $offset [expr {$offset + 7}] [binary format w $value]]
                        append forged [binary format w [gcmd_crc64 $forged]]
                        set refused [catch {r restore fx:take:copy 0 $forged} err]
                        assert_equal $refused [expr {!$accepted}] "RESTORE of a payload with $what in the table: $err"
                    }
                    r del fx:take:copy
                }
            }
            gcmd_check_saveable fx:take $shown
        }
        r del fx:take
    }

    gcmd_test {SPEC 5: the drift detector stops a gate at the wrong spot check the table says} {
        # Spot checks asked and answered one after the other; the first is
        # the first of a block (the gate answers since this moment). 'at'
        # is worked out by hand in the helpers test above: the 7th, 6th or
        # 18th of nothing but wrong ones; later when right ones came before
        # in the same block (a block does not start over, CHANGES 9f); in
        # the next block of 64 when too many did; and in the block of 512
        # when three in eight are wrong.
        foreach {alpha outcomes at} [list \
            0.2  {1 1 1 1 1 1 1 1 1 1}                   7 \
            0.1  {1 1 1 1 1 1 1 1 1 1}                   6 \
            0.05 {1 1 1 1 1 1 1 1 1 1}                   6 \
            0.5  [lrepeat 20 1]                          18 \
            0.2  {1 1 1 0 1 1 1 1 1 1 1 1}               9 \
            0.2  {1 1 1 1 1 1 0 1 1 1 1 1}               9 \
            0.2  [concat [lrepeat 5 0] [lrepeat 12 1]]   15 \
            0.2  [concat [lrepeat 60 0] [lrepeat 20 1]]  71 \
            0.2  [concat [lrepeat 57 0] [lrepeat 20 1]]  71 \
            0.2  [concat {*}[lrepeat 12 {1 1 1 0 0 0 0 0}]] 75 \
        ] {
            set shown "ALPHA $alpha, spot checks [string map {{ } {}} $outcomes]"
            set alarm [gcmd_drift_alarm_at $alpha $outcomes]
            assert_equal $alarm $at "$shown: the spot check that raises the alarm"
            r del fx:drift
            r gcreate fx:drift ALPHA $alpha DELTA 0.2 AUDIT 1 SCORE
            gcmd_prove_all fx:drift
            assert_equal ACTIVE [gcmd_info fx:drift state]
            set n 0
            set errors 0
            foreach wrong [lrange $outcomes 0 [expr {$alarm - 1}]] {
                incr n
                set info [r ginfo fx:drift]
                assert_equal [dict get $info state] ACTIVE "$shown: before spot check $n"
                assert_equal [dict get $info revocations] 0 "$shown: before spot check $n"
                assert_equal [dict get $info audit-samples] [expr {$n - 1}] $shown
                assert_equal [dict get $info audit-errors] $errors $shown
                lassign [r gpredict fx:drift SCORE 0.9] action id
                assert_equal $action AUDIT "$shown: spot check $n"
                assert_equal 1 [r glearn fx:drift $id CORRECT [expr {!$wrong}]]
                incr errors $wrong
            }
            # "the gate stops answering at once ... and starts over by itself"
            set info [r ginfo fx:drift]
            assert_equal [dict get $info revocations] 1 "$shown: after spot check $alarm"
            assert_equal [dict get $info state] CALIBRATING $shown
            assert_equal [dict get $info generation] 0 $shown
            assert_equal [dict get $info candidate] 2 $shown
            assert_equal [dict get $info thresholds] {{}} $shown
            assert_equal [dict get $info audit-samples] 0 $shown
            assert_equal [dict get $info calibration-samples] 0 $shown
            lassign [r gpredict fx:drift SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn fx:drift $id CORRECT 1]
            gcmd_check_saveable fx:drift $shown
        }
        # GRESET "restarts the counters": this one too
        assert_equal 1 [gcmd_info fx:drift revocations]
        assert_equal OK [r greset fx:drift]
        set info [r ginfo fx:drift]
        assert_equal 0 [dict get $info revocations]
        assert_equal 0 [dict get $info requests]
        assert_equal 3 [dict get $info candidate]
        r del fx:drift
    }

    gcmd_test {SPEC 3: a spot check counts the moment its answer arrives; one that still waits counts as right} {
        # gate_core.h, DRIFT: "A spot check gets its number when it is
        # asked and counts as right until a disagreeing answer arrives,
        # which is reported the moment it arrives." CHANGES 6: one
        # unanswered spot check used to hide every later one from the
        # detector. Here the first spot check of the block never gets its
        # answer: it is in the block, as a right one, so the alarm is at
        # the eighth wrong answer and not at the seventh (helpers test:
        # 594.75 and 1736.25).
        r del fx:drift
        r gcreate fx:drift ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_all fx:drift
        lassign [r gpredict fx:drift SCORE 0.9] action waiting
        assert_equal AUDIT $action
        set alarm [expr {[gcmd_drift_alarm_at 0.2 {0 1 1 1 1 1 1 1 1 1 1}] - 1}]
        assert_equal 8 $alarm
        for {set n 1} {$n <= $alarm} {incr n} {
            set info [r ginfo fx:drift]
            assert_equal [dict get $info state] ACTIVE "before spot check $n"
            # counted when answered, not when the older request lets them through
            assert_equal [dict get $info audit-samples] [expr {$n - 1}] "before spot check $n"
            assert_equal [dict get $info audit-errors] [expr {$n - 1}] "before spot check $n"
            assert_equal [dict get $info pending] 1 "before spot check $n"
            lassign [r gpredict fx:drift SCORE 0.9] action id
            assert_equal $action AUDIT "spot check $n"
            assert_equal 1 [r glearn fx:drift $id CORRECT 0]
        }
        set info [r ginfo fx:drift]
        assert_equal 1 [dict get $info revocations]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 2 [dict get $info candidate]
        # The request that still waited was proof for what the gate has
        # dropped: nothing waits any more, its answer is not used.
        assert_equal 0 [dict get $info pending]
        assert_equal 0 [dict get $info missing]
        assert_equal 0 [r glearn fx:drift $waiting CORRECT 1]
        gcmd_check_saveable fx:drift
        r del fx:drift
    }

    gcmd_test {SPEC 3: disagreements that arrive in a batch do not stop a gate that is as good as it says} {
        # "Its verdict does not depend on the order or the timing of the
        # answers." 40 spot checks are asked; 7 of them disagree (17.5%,
        # the gate promised 20%), and those 7 answers arrive first, in one
        # transaction. A detector that takes the answers in the order they
        # arrive saw seven disagreements in a row there, which is the
        # alarm (the rule before CHANGES 9f). In a block of 40 spot checks
        # 7 disagreements are (2^7 x 0.75^33 + 3^7 x 0.5^33) / 2 = 0.005.
        r del fx:batch
        r gcreate fx:batch ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_all fx:batch
        set ids {}
        for {set i 0} {$i < 40} {incr i} {
            lassign [r gpredict fx:batch SCORE 0.9] action id
            assert_equal AUDIT $action "spot check [expr {$i + 1}]"
            lappend ids $id
        }
        set d [gcmd_drift_new 0.2]
        for {set i 0} {$i < 40} {incr i} {gcmd_drift_ask d}
        foreach number {1 6 11 16 21 26 31} {assert_equal {} [gcmd_drift_tell d $number wrong]}
        assert {exp([gcmd_drift_log_stat 0.2 40 7]) < 0.01}

        r multi
        foreach i {0 5 10 15 20 25 30} {r glearn fx:batch [lindex $ids $i] CORRECT 0}
        assert_equal {1 1 1 1 1 1 1} [r exec]
        set info [r ginfo fx:batch]
        assert_equal ACTIVE [dict get $info state] "after seven disagreements in a row, of 40 spot checks"
        assert_equal 0 [dict get $info revocations]
        assert_equal 1 [dict get $info generation]
        assert_equal 7 [dict get $info audit-samples]
        assert_equal 7 [dict get $info audit-errors]
        assert_equal 33 [dict get $info pending]
        # the other 33 agree, the newest first
        foreach i {39 38 37 36 35 34 33 32 31 29 28 27 26 24 23 22 21 19 18 17 16 14 13 12 11 9 8 7 6 4 3 2 1} {
            assert_equal 1 [r glearn fx:batch [lindex $ids $i] CORRECT 1]
        }
        set info [r ginfo fx:batch]
        assert_equal ACTIVE [dict get $info state]
        assert_equal 0 [dict get $info revocations]
        assert_equal 40 [dict get $info audit-samples]
        assert_equal 7 [dict get $info audit-errors]
        assert_equal 0 [dict get $info pending]
        assert_equal 0 [dict get $info missing]
        # it goes on answering (here: asking for the next spot check)
        lassign [r gpredict fx:batch SCORE 0.9] action id
        assert_equal AUDIT $action
        assert_equal 1 [r glearn fx:batch $id CORRECT 1]
        gcmd_check_saveable fx:batch
        r del fx:batch
    }

    gcmd_test {SPEC 3: the answers of a block stop a gate at the same disagreement in whatever order they arrive} {
        # Nine spot checks are asked before any answer. Seven
        # disagreements among nine are (2^7 x 0.75^2 + 3^7 x 0.5^2) / 2 =
        # 309.4, eight are (2^8 x 0.75 + 3^8 x 0.5) / 2 = 1736.25: the
        # eighth disagreement is the alarm, whichever of the nine spot
        # checks it belongs to. (Asked and answered one at a time, the
        # seventh is: above.)
        assert {abs(exp([gcmd_drift_log_stat 0.2 9 7]) - 309.375) < 1e-6}
        foreach order {oldest-first newest-first} {
            r del fx:order
            r gcreate fx:order ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
            gcmd_prove_all fx:order
            set ids {}
            for {set i 0} {$i < 9} {incr i} {
                lassign [r gpredict fx:order SCORE 0.9] action id
                assert_equal AUDIT $action
                lappend ids $id
            }
            if {$order eq "newest-first"} {set ids [lreverse $ids]}
            set n 0
            foreach id [lrange $ids 0 7] {
                incr n
                set info [r ginfo fx:order]
                assert_equal ACTIVE [dict get $info state] "$order: before disagreement $n"
                assert_equal 0 [dict get $info revocations] "$order: before disagreement $n"
                assert_equal [expr {$n - 1}] [dict get $info audit-errors] "$order: before disagreement $n"
                assert_equal [expr {10 - $n}] [dict get $info pending] "$order: before disagreement $n"
                assert_equal 1 [r glearn fx:order $id CORRECT 0]
            }
            set info [r ginfo fx:order]
            assert_equal 1 [dict get $info revocations] "$order: after the eighth disagreement"
            assert_equal CALIBRATING [dict get $info state] $order
            assert_equal 2 [dict get $info candidate] $order
            # the ninth was proof for what the gate dropped
            assert_equal 0 [dict get $info pending] $order
            assert_equal 0 [dict get $info missing] $order
            assert_equal 0 [r glearn fx:order [lindex $ids 8] CORRECT 0] $order
            gcmd_check_saveable fx:order $order
        }
        r del fx:order
    }

    gcmd_test {SPEC 5: a disagreement that arrives after its block of 64 has ended counts in the longer blocks only} {
        # gate_stats.h: "A disagreeing answer that arrives after its block
        # has ended counts only in the longer blocks that are still open."
        # Spot check 1 waits; 2 to 64 agree; 65 and 66, the first two of
        # the next block of 64, agree; then the answer of spot check 1
        # arrives, and disagrees. The block that began with 65 still has
        # no disagreement: with the two right spot checks in it the alarm
        # is at its ninth wrong one ((2^8 x 0.75^2 + 3^8 x 0.5^2) / 2 =
        # 892.1 after eight, 2604.4 after nine), which is spot check 75 and
        # the tenth disagreement the gate is told. Had the late answer
        # been counted in the open block, spot check 73 would have been
        # the alarm. (In the blocks of 512 and of 4,096, ten disagreements
        # among 75 spot checks are nothing.)
        assert {abs(exp([gcmd_drift_log_stat 0.2 10 8]) - 892.125) < 1e-6}
        set d [gcmd_drift_new 0.2]
        set first [gcmd_drift_ask d]
        for {set i 0} {$i < 65} {incr i} {gcmd_drift_ask d}
        assert_equal {} [gcmd_drift_tell d $first wrong]
        for {set i 0} {$i < 8} {incr i} {assert_equal {} [gcmd_drift_tell d [gcmd_drift_ask d] wrong]}
        assert_equal worse [gcmd_drift_tell d [gcmd_drift_ask d] wrong]
        assert_equal 75 [dict get $d asked]

        r del fx:late
        r gcreate fx:late ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_all fx:late
        lassign [r gpredict fx:late SCORE 0.9] action waiting
        assert_equal AUDIT $action
        assert_equal AUDIT [lsort -unique [gcmd_ask_score fx:late 65 0.9 1]]
        assert_equal 1 [r glearn fx:late $waiting CORRECT 0]
        for {set told 1} {$told < 10} {incr told} {
            set info [r ginfo fx:late]
            assert_equal ACTIVE [dict get $info state] "after $told disagreements"
            assert_equal 0 [dict get $info revocations] "after $told disagreements"
            assert_equal $told [dict get $info audit-errors]
            assert_equal [expr {65 + $told}] [dict get $info audit-samples]
            assert_equal AUDIT [gcmd_ask_score fx:late 1 0.9 0]
        }
        set info [r ginfo fx:late]
        assert_equal 1 [dict get $info revocations] "after the tenth disagreement, spot check 75"
        assert_equal CALIBRATING [dict get $info state]
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2] + 75}] [dict get $info requests]
        gcmd_check_saveable fx:late
        r del fx:late
    }

    gcmd_test {SPEC 3: a classifier that got worse forgets its model and learns again} {
        r del fx:drift
        gcmd_create_fast fx:drift ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham
        gcmd_teach fx:drift 40
        gcmd_count_proof fx:drift
        set before [r ginfo fx:drift]
        assert_equal ACTIVE [dict get $before state]
        # Each label has a detector of its own. The two labels were proven
        # by requests that came in turns, "ham" right after "spam", so no
        # spot check was asked yet: the blocks of "spam" begin here.
        assert_equal 0 [dict get $before audit-samples]
        # The teacher changes its mind about one message: the seventh
        # wrong spot check is the alarm (above).
        for {set n 0} {$n < 40 && [gcmd_info fx:drift revocations] == 0} {incr n} {
            assert_equal $n [gcmd_info fx:drift audit-errors]
            lassign [r gpredict fx:drift TEXT "buy cheap pills now"] action id
            assert_equal $action AUDIT "spot check [expr {$n + 1}]"
            assert_equal 1 [r glearn fx:drift $id LABEL ham]
        }
        set alarm $n
        assert_equal $alarm [gcmd_drift_alarm_at 0.2 {1 1 1 1 1 1 1 1 1 1}] "the wrong spot check that stopped the gate"
        assert_equal 7 $alarm
        set info [r ginfo fx:drift]
        assert_equal 1 [dict get $info revocations]
        assert_equal COLD [dict get $info state]
        assert_equal 0 [dict get $info learned]
        assert_equal 0 [dict get $info generation]
        assert_equal 0 [dict get $info candidate]
        assert_equal {{} {}} [dict get $info thresholds]
        # the counters of requests go on, the configuration stays
        assert_equal [expr {[dict get $before requests] + $alarm}] [dict get $info requests]
        assert_equal [dict get $before classes] [dict get $info classes]
        # it starts over by itself: 40 examples, a snapshot with a new number
        lassign [r gpredict fx:drift TEXT "buy cheap pills now"] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn fx:drift $id LABEL ham]
        gcmd_teach fx:drift 39
        set info [r ginfo fx:drift]
        assert_equal CALIBRATING [dict get $info state]
        assert {[dict get $info candidate] > [dict get $before candidate]}
        assert {[dict get $info candidate] > [dict get $before generation]}
        gcmd_check_saveable fx:drift
        r del fx:drift
    }

    gcmd_test {SPEC 3: each label has a drift detector of its own} {
        # gate_core.h, DRIFT: "Each serving row runs a drift detector over
        # its spot checks of accepted traffic." CHANGES 9f: spot checks
        # are numbered per label. Six spot checks of "spam" agree. Six of
        # "ham" disagree: (2^6 + 3^6) / 2 = 396.5 in the block of "ham",
        # and nothing in the block of "spam", whose spot checks have the
        # same numbers, 1 to 6. Then one of "spam" disagrees: one wrong
        # among seven in its block, and the gate goes on. The seventh
        # disagreement of "ham" is the alarm.
        r del fx:rows
        gcmd_create_fast fx:rows ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham
        gcmd_teach fx:rows 40
        gcmd_count_proof fx:rows
        set before [r ginfo fx:rows]
        assert_equal ACTIVE [dict get $before state]
        assert_equal 0 [dict get $before audit-samples]
        set asked 0
        set wrong 0
        foreach {text teacher} [concat \
            [lrepeat 6 "buy cheap pills now" spam] \
            [lrepeat 6 "meeting at noon tomorrow" spam] \
            [list "buy cheap pills now" ham] \
        ] {
            lassign [r gpredict fx:rows TEXT $text] action id
            assert_equal AUDIT $action "spot check [incr asked]"
            assert_equal 1 [r glearn fx:rows $id LABEL $teacher]
            if {$asked > 6} {incr wrong}
            set info [r ginfo fx:rows]
            assert_equal ACTIVE [dict get $info state] "after $asked spot checks, $wrong of them wrong"
            assert_equal 0 [dict get $info revocations] "after $asked spot checks, $wrong of them wrong"
            assert_equal $wrong [dict get $info audit-errors]
            assert_equal $asked [dict get $info audit-samples]
            assert_equal 1 [dict get $info generation]
        }
        assert_equal {13 7} [list $asked $wrong]
        lassign [r gpredict fx:rows TEXT "meeting at noon tomorrow"] action id
        assert_equal AUDIT $action
        assert_equal 1 [r glearn fx:rows $id LABEL spam]
        set info [r ginfo fx:rows]
        assert_equal 1 [dict get $info revocations] "after the seventh disagreement for 'ham'"
        assert_equal COLD [dict get $info state]
        assert_equal 0 [dict get $info learned]
        gcmd_check_saveable fx:rows
        r del fx:rows
    }

    gcmd_test {SPEC 3: a model copy is one weight per bucket and label, 8,192 buckets} {
        # Four bytes a weight: 32 KiB for each label and copy of the model
        # (two labels share one set of weights).
        unset -nocomplain cold
        r config set gate.snapshot-examples 1
        foreach n {2 3 4 5} {
            set labels {}
            for {set i 0} {$i < $n} {incr i} {lappend labels label$i}
            r del fx:mem:$n
            r gcreate fx:mem:$n ALPHA 0.2 CLASSES $n {*}$labels
            set cold($n) [gcmd_info fx:mem:$n memory]
        }
        r config set gate.snapshot-examples 1000
        r del fx:mem:score
        r gcreate fx:mem:score ALPHA 0.2 SCORE
        # a score gate has no model
        assert {[gcmd_info fx:mem:score memory] < 32768 + 8192}
        assert {$cold(2) > [gcmd_info fx:mem:score memory] + 32768 - 8192}
        assert {$cold(2) < [gcmd_info fx:mem:score memory] + 32768 + 8192}
        # one more label, one more set of weights
        foreach n {4 5} {
            set more [expr {$cold($n) - $cold([expr {$n - 1}])}]
            assert {$more >= 32768 && $more < 32768 + 1024}
        }
        assert {$cold(3) - $cold(2) >= 2 * 32768 && $cold(3) - $cold(2) < 2 * 32768 + 1024}
        # The first snapshot is a second copy.
        foreach n {2 3 4 5} {
            r gadd fx:mem:$n TEXT hello LABEL label0
            assert_equal 1 [gcmd_info fx:mem:$n candidate]
            set copies [expr {$n == 2 ? 1 : $n}]
            set more [expr {[gcmd_info fx:mem:$n memory] - $cold($n)}]
            assert {$more >= $copies * 32768 && $more < $copies * 32768 + $n * 4096}
            r del fx:mem:$n
        }
        r del fx:mem:score
        # "up to three copies" (README): the model that learns, the
        # snapshot that answers and the snapshot being tested.
        r del fx:mem:three
        gcmd_create_fast fx:mem:three ALPHA 0.2 DELTA 0.2 CLASSES 3 spam ham other
        set none [gcmd_info fx:mem:three memory]
        gcmd_teach fx:mem:three 40
        gcmd_count_proof fx:mem:three
        set info [r ginfo fx:mem:three]
        assert_equal 1 [dict get $info generation]
        assert_equal 2 [dict get $info candidate]
        assert_equal 0 [dict get $info pending]
        set more [expr {[dict get $info memory] - $none}]
        assert {$more >= 2 * 3 * 32768 && $more < 2 * 3 * 32768 + 32768}
        r del fx:mem:three
    }

    gcmd_test {the server is still up (fixed choices)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 9. Late and lost answers (real time)
# ============================================================================
#
# SPEC 3, "Late and lost answers", "No answers at all" and the lost spot
# checks of "Getting worse"; SPEC 5, "feedback timeout", "answers missing in
# a row" and "drift detector"; CHANGES 2, 3 and 9 (b, e, f). A gate does its
# bookkeeping when it is written to, so each moment below is a write at a
# known time after the requests were made:
#
#   early   less than a quarter of the feedback timeout
#   late    more than a quarter of it, less than the whole
#   lost    more than the feedback timeout
#
# All gates of these tests are set up and visited in ONE run that takes a
# little longer than their feedback timeout; the tests then look at what the
# run saw. The run measures itself. If the machine was too slow for "early"
# to be early or for "late" to be before the timeout, the run is void and is
# done again with new gates, at most twice.

# Right answers "a" to the text "alpha one" until the classifier has a
# threshold for "a"; returns how many it took.
proc gcmd_prove_label {key {max 2000}} {
    for {set n 0} {$n < $max} {incr n} {
        if {[lindex [gcmd_info $key thresholds] 0] ne {}} {return $n}
        lassign [r gpredict $key TEXT "alpha one"] action id
        if {$id ne {}} {r glearn $key $id LABEL a}
    }
    fail "classifier $key proved nothing in $max right answers: [r ginfo $key]"
}

proc gcmd_late_run {timeout k} {
    set grace [expr {$timeout / 4}]
    set seen [dict create valid 1 why {}]

    r config set gate.feedback-timeout $timeout
    r config set gate.snapshot-examples 1
    foreach name {early late wrong flip gone lost idle} {
        r gcreate $k:$name ALPHA 0.2 DELTA 0.2 SCORE
    }
    foreach name {blind b31 b33 burst older u20 u19 u19w ur22 ur23} {
        r gcreate $k:$name ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
    }
    foreach name {s16 s15} {
        r gcreate $k:$name ALPHA 0.05 DELTA 0.2 AUDIT 1 SCORE
    }
    r gcreate $k:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 a b
    r gcreate $k:bcls ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 a b
    r config set gate.feedback-timeout 60000
    r config set gate.snapshot-examples 1000
    # Gates that answer, with AUDIT 1: every request is proof. Five score
    # gates answer from a threshold between 0.1 and 0.9, so that a request
    # at 0.1 is proof they want and no spot check; seven score gates (two
    # of them with an error rate of 0.05) and a classifier answer
    # everything they are asked, so that every request is a spot check.
    # Two of those have had five spot checks that agreed. And a classifier
    # that tests its first snapshot.
    foreach name {blind b31 b33 burst older} {gcmd_prove_score $k:$name}
    foreach name {u20 u19 u19w ur22 ur23 s16 s15} {gcmd_prove_all $k:$name}
    foreach name {ur22 ur23} {dict set seen $name agreed [gcmd_ask_score $k:$name 5 0.9 1]}
    r gadd $k:bcls TEXT "alpha one" LABEL a
    gcmd_prove_label $k:bcls
    r gadd $k:cls TEXT "alpha one" LABEL a

    # ---- the requests. In each gate the first gets no answer for now and
    # the second gets its answer at once.
    set t0 [clock milliseconds]
    foreach name {early late wrong gone lost} {
        dict set seen $name first [lindex [r gpredict $k:$name SCORE 0.7] 1]
        set id [lindex [r gpredict $k:$name SCORE 0.7] 1]
        dict set seen $name second [r glearn $k:$name $id CORRECT 1]
        dict set seen $name start [r ginfo $k:$name]
    }
    dict set seen cls first [lindex [r gpredict $k:cls TEXT "alpha one"] 1]
    set id [lindex [r gpredict $k:cls TEXT "alpha one"] 1]
    dict set seen cls second [r glearn $k:cls $id LABEL a]
    dict set seen cls start [r ginfo $k:cls]
    dict set seen idle first [lindex [r gpredict $k:idle SCORE 0.7] 1]
    dict set seen idle start [gcmd_state $k:idle]
    # one request without an answer and 39 right answers behind it: four
    # more than a gate that is never wrong needs
    dict set seen flip first [lindex [r gpredict $k:flip SCORE 0.7] 1]
    gcmd_ask_score $k:flip 39 0.7 1
    dict set seen flip start [r ginfo $k:flip]
    # requests nobody answers: proof below the threshold of the gate (32,
    # 31, 33, 40 of which the newest gets its answer at once, and 33 of
    # which the oldest gets its answer when all have been asked), and spot
    # checks
    foreach {name n ask} {blind 32 {SCORE 0.1} b31 31 {SCORE 0.1} b33 33 {SCORE 0.1} burst 40 {SCORE 0.1}
                          older 33 {SCORE 0.1}
                          u20 20 {SCORE 0.9} u19 19 {SCORE 0.9} u19w 19 {SCORE 0.9}
                          ur22 22 {SCORE 0.9} ur23 23 {SCORE 0.9} s16 16 {SCORE 0.9} s15 15 {SCORE 0.9}
                          bcls 32 {TEXT {alpha one}}} {
        set actions {}
        set oldest {}
        for {set i 0} {$i < $n} {incr i} {
            lassign [r gpredict $k:$name {*}$ask] action id
            lappend actions $action
            if {$id eq {}} {lappend actions "no id"}
            if {$i == 0} {set oldest $id}
        }
        dict set seen $name asked [lsort -unique $actions]
        if {$name eq "burst"} {dict set seen burst newest [r glearn $k:burst $id CORRECT 0]}
        if {$name eq "older"} {dict set seen older oldest [r glearn $k:older $oldest CORRECT 0]}
        dict set seen $name start [r ginfo $k:$name]
    }
    set t1 [clock milliseconds]

    # ---- early: a request, answered at once
    gcmd_sleep_until [expr {$t1 + $grace / 3}]
    set id [lindex [r gpredict $k:early SCORE 0.7] 1]
    r glearn $k:early $id CORRECT 1
    dict set seen early waiting [r ginfo $k:early]
    set took [expr {[clock milliseconds] - $t0}]
    if {$took >= $grace} {
        dict set seen valid 0
        dict set seen why "the early write came after $took ms, a quarter of the timeout is $grace ms"
    }

    # ---- late: a request, answered at once; then the answers that were late
    gcmd_sleep_until [expr {$t1 + $grace + 20}]
    foreach name {early late wrong gone} {
        set id [lindex [r gpredict $k:$name SCORE 0.7] 1]
        dict set seen $name third [r glearn $k:$name $id CORRECT 1]
        dict set seen $name moved [r ginfo $k:$name]
    }
    set id [lindex [r gpredict $k:cls TEXT "alpha one"] 1]
    dict set seen cls third [r glearn $k:cls $id LABEL a]
    dict set seen cls moved [r ginfo $k:cls]
    dict set seen late answer [r glearn $k:late [dict get $seen late first] CORRECT 1]
    dict set seen late again [r glearn $k:late [dict get $seen late first] CORRECT 1]
    dict set seen late after [r ginfo $k:late]
    dict set seen wrong answer [r glearn $k:wrong [dict get $seen wrong first] CORRECT 0]
    dict set seen wrong after [r ginfo $k:wrong]
    dict set seen cls answer [r glearn $k:cls [dict get $seen cls first] LABEL a]
    dict set seen cls after [r ginfo $k:cls]
    r gresume $k:flip
    dict set seen flip moved [r ginfo $k:flip]
    dict set seen flip answer [r glearn $k:flip [dict get $seen flip first] CORRECT 1]
    dict set seen flip after [r ginfo $k:flip]
    set took [expr {[clock milliseconds] - $t0}]
    if {$took >= $timeout} {
        dict set seen valid 0
        dict set seen why "the late answers came after $took ms, the timeout is $timeout ms"
    }

    # ---- lost
    gcmd_sleep_until [expr {$t1 + $timeout + 20}]
    # the gate nobody wrote to since its request
    dict set seen idle waited [gcmd_state $k:idle]
    r gresume $k:idle
    dict set seen idle after [r ginfo $k:idle]
    dict set seen idle answer [r glearn $k:idle [dict get $seen idle first] CORRECT 1]
    # the request proof had moved on without
    r gresume $k:gone
    dict set seen gone after [r ginfo $k:gone]
    dict set seen gone answer [r glearn $k:gone [dict get $seen gone first] CORRECT 1]
    # the request proof still waited for
    set id [lindex [r gpredict $k:lost SCORE 0.7] 1]
    dict set seen lost third [r glearn $k:lost $id CORRECT 1]
    dict set seen lost after [r ginfo $k:lost]
    dict set seen lost answer [r glearn $k:lost [dict get $seen lost first] CORRECT 1]
    # the gates whose requests nobody answered: one request more, which
    # gets its answer (a wrong guess for one of them)
    foreach {name ask answer} {blind {SCORE 0.9} {CORRECT 1} b31 {SCORE 0.9} {CORRECT 1}
                               b33 {SCORE 0.9} {CORRECT 1} burst {SCORE 0.9} {CORRECT 1}
                               older {SCORE 0.9} {CORRECT 1}
                               u20 {SCORE 0.9} {CORRECT 1} u19 {SCORE 0.9} {CORRECT 1}
                               u19w {SCORE 0.9} {CORRECT 0}
                               ur22 {SCORE 0.9} {CORRECT 1} ur23 {SCORE 0.9} {CORRECT 1}
                               s16 {SCORE 0.9} {CORRECT 1} s15 {SCORE 0.9} {CORRECT 1}
                               bcls {TEXT {alpha one}} {LABEL a}} {
        set reply [r gpredict $k:$name {*}$ask]
        dict set seen $name reply $reply
        dict set seen $name after [r ginfo $k:$name]
        dict set seen $name answer [r glearn $k:$name [lindex $reply 1] {*}$answer]
        dict set seen $name again [r ginfo $k:$name]
    }

    # ---- how many right answers each gate still needs (time no longer matters)
    foreach name {late wrong gone lost} {
        dict set seen $name more [gcmd_prove_all $k:$name 0.7]
    }
    dict set seen cls more [gcmd_prove_label $k:cls]
    r greset $k:lost
    dict set seen lost reset [r ginfo $k:lost]
    foreach name {early late wrong flip gone lost idle blind b31 b33 burst older u20 u19 u19w ur22 ur23
                  s16 s15 cls bcls} {
        gcmd_check_saveable $k:$name
        r del $k:$name
    }
    return $seen
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77010

    # The run. (Between the tests, so that each of them finds it when it is
    # run alone.)
    set late [dict create valid 0 why "the run did not start"]
    for {set attempt 1} {$attempt <= 3} {incr attempt} {
        if {[catch {gcmd_late_run 2000 lt$attempt} late]} {
            set late [dict create valid 0 why "the run failed: $late"]
            break
        }
        if {[dict get $late valid]} break
    }

    # What the tests below read: a field of what the run saw at a moment.
    proc gcmd_late {late gate moment {field {}}} {
        if {![dict get $late valid]} {
            fail "no valid run of the gates with a short feedback timeout: [dict get $late why]"
        }
        set value [dict get $late $gate $moment]
        if {$field eq {}} {return $value}
        dict get $value $field
    }

    gcmd_test {late answers: proof waits for an answer for a quarter of the feedback timeout} {
        # "Proof waits for a late answer for a quarter of the feedback
        # timeout ..., then moves on and counts the request as a wrong
        # guess until its answer arrives."
        assert {[gcmd_late $late early first] ne {}}
        assert_equal 1 [gcmd_late $late early second]
        # at once: one request waits for GLEARN, the answer of the other
        # waits for its turn
        assert_equal 1 [gcmd_late $late early start pending]
        assert_equal 0 [gcmd_late $late early start calibration-samples]
        # less than a quarter of the timeout later, with one answer more
        assert_equal 1 [gcmd_late $late early waiting pending]
        assert_equal 0 [gcmd_late $late early waiting calibration-samples]
        assert_equal 0 [gcmd_late $late early waiting missing]
        assert_equal 3 [gcmd_late $late early waiting requests]
        # more than a quarter later: the request without an answer counts
        # (as a wrong guess), and so do the three answers behind it
        assert_equal 1 [gcmd_late $late early third]
        assert_equal 4 [gcmd_late $late early moved calibration-samples]
        assert_equal 1 [gcmd_late $late early moved pending]
        assert_equal 0 [gcmd_late $late early moved missing]
        assert_equal CALIBRATING [gcmd_late $late early moved state]
    }

    gcmd_test {late answers: the answer of a request proof moved on without is still taken and sets the count right} {
        assert_equal 1 [gcmd_late $late late start pending]
        assert_equal 0 [gcmd_late $late late start calibration-samples]
        assert_equal 3 [gcmd_late $late late moved calibration-samples]
        # the request keeps waiting for its GLEARN, with its id
        assert_equal 1 [gcmd_late $late late moved pending]
        assert_equal 0 [gcmd_late $late late moved missing]
        assert_equal 1 [gcmd_late $late late answer]
        assert_equal 0 [gcmd_late $late late again]
        assert_equal 0 [gcmd_late $late late after pending]
        assert_equal 0 [gcmd_late $late late after missing]
        # it had been counted already: the count does not move ...
        assert_equal 3 [gcmd_late $late late after calibration-samples]
        # ... the wrong guess became a right one: with the three right
        # answers so far the gate needs what a gate that was never wrong
        # needs, 35 in all (SPEC 5).
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2] - 3}] [gcmd_late $late late more]
    }

    gcmd_test {late answers: a late answer that says the guess was wrong leaves it counted as wrong} {
        assert_equal 3 [gcmd_late $late wrong moved calibration-samples]
        assert_equal 1 [gcmd_late $late wrong answer]
        assert_equal 0 [gcmd_late $late wrong after pending]
        assert_equal 0 [gcmd_late $late wrong after missing]
        assert_equal 3 [gcmd_late $late wrong after calibration-samples]
        # one wrong guess and two right ones so far
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2 1 1 1] - 2}] [gcmd_late $late wrong more]
    }

    gcmd_test {late answers: a late answer that was right can be the one that proves a threshold} {
        # CHANGES 2: when the answer arrives the wrong guess is taken back,
        # and what that proves is proven at once.
        assert_equal 1 [gcmd_late $late flip start pending]
        assert_equal 0 [gcmd_late $late flip start calibration-samples]
        # proof moved on: one wrong guess (for now) and 39 right ones, where
        # 47 right ones are needed after a wrong one
        assert_equal 40 [gcmd_late $late flip moved calibration-samples]
        assert_equal CALIBRATING [gcmd_late $late flip moved state]
        assert_equal {{}} [gcmd_late $late flip moved thresholds]
        assert_equal 1 [gcmd_late $late flip moved pending]
        assert {[gcmd_proof_needed 0.2 0.2 1 1 1] > 39 + 1}
        # the answer: 40 right guesses, 35 were needed
        assert_equal 1 [gcmd_late $late flip answer]
        assert_equal ACTIVE [gcmd_late $late flip after state]
        assert_equal 1 [gcmd_late $late flip after generation]
        assert {[lindex [gcmd_late $late flip after thresholds] 0] == 0}
        assert_equal 40 [gcmd_late $late flip after calibration-samples]
        assert_equal 0 [gcmd_late $late flip after pending]
        assert_equal 0 [gcmd_late $late flip after missing]
    }

    gcmd_test {late answers: after the feedback timeout the answer is lost, GLEARN replies 0} {
        # "An answer that has not arrived after the feedback timeout ... is
        # lost: the request stays a wrong guess and its id is forgotten."
        # The request proof had moved on without:
        assert_equal 3 [gcmd_late $late gone moved calibration-samples]
        assert_equal 1 [gcmd_late $late gone moved pending]
        assert_equal 0 [gcmd_late $late gone after pending]
        assert_equal 1 [gcmd_late $late gone after missing]
        assert_equal 3 [gcmd_late $late gone after calibration-samples]
        assert_equal 0 [gcmd_late $late gone answer]
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2 1 1 1] - 2}] [gcmd_late $late gone more]
        # The request proof still waited for (nothing was written to the
        # gate in between): it counts now, and so do the answers behind it.
        assert_equal 1 [gcmd_late $late lost start pending]
        assert_equal 1 [gcmd_late $late lost third]
        assert_equal 0 [gcmd_late $late lost after pending]
        assert_equal 1 [gcmd_late $late lost after missing]
        assert_equal 3 [gcmd_late $late lost after calibration-samples]
        assert_equal 0 [gcmd_late $late lost answer]
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2 1 1 1] - 2}] [gcmd_late $late lost more]
        # GRESET "restarts the counters": the lost answers too
        assert_equal 0 [gcmd_late $late lost reset missing]
        assert_equal 0 [gcmd_late $late lost reset requests]
        assert_equal 2 [gcmd_late $late lost reset candidate]
    }

    gcmd_test {late answers: GINFO shows an idle gate as of its last write} {
        # "A gate does its bookkeeping when it is written to, so GINFO
        # shows an idle gate as of its last write."
        set start [gcmd_late $late idle start]
        assert_equal 1 [dict get [lindex $start 0] pending]
        assert_equal 0 [dict get [lindex $start 0] missing]
        # the feedback timeout has passed and nothing was written: GINFO
        # and the value are what they were
        assert_equal $start [gcmd_late $late idle waited]
        # the first write does the bookkeeping
        assert_equal 0 [gcmd_late $late idle after pending]
        assert_equal 1 [gcmd_late $late idle after missing]
        assert_equal 1 [gcmd_late $late idle after calibration-samples]
        assert_equal CALIBRATING [gcmd_late $late idle after state]
        assert_equal 0 [gcmd_late $late idle answer]
    }

    gcmd_test {late answers: 32 answers missing in a row stop the gate, 31 do not} {
        # "When 32 requests in a row end without an answer and no answer
        # of any request has arrived since the first of them was asked,
        # the feedback path is broken and the gate cannot check itself.
        # It stops answering, keeps its model, and proves itself again as
        # soon as answers arrive."
        # The gates answer from the second of their thresholds; the
        # requests nobody answers are below it: proof the gates asked for
        # (AUDIT 1), and not spot checks, which have a rule of their own
        # (below).
        foreach gate {blind b31 b33} n {32 31 33} {
            assert_equal ABSTAIN [gcmd_late $late $gate asked]
            assert_equal ACTIVE [gcmd_late $late $gate start state]
            assert_equal $n [gcmd_late $late $gate start pending]
            assert_equal 0 [gcmd_late $late $gate start missing]
            assert_equal 1 [gcmd_late $late $gate start generation]
            assert_equal 1 [gcmd_grid_index 0 0.998 [lindex [gcmd_late $late $gate start thresholds] 0]]
        }
        # 31: the gate still answers (here: asks for a spot check)
        assert_equal AUDIT [lindex [gcmd_late $late b31 reply] 0]
        assert_equal ACTIVE [gcmd_late $late b31 after state]
        assert_equal 31 [gcmd_late $late b31 after missing]
        assert_equal 0 [gcmd_late $late b31 after revocations]
        assert_equal 0 [gcmd_late $late b31 after audit-samples]
        assert_equal 0 [gcmd_late $late b31 after audit-errors]
        assert_equal 1 [gcmd_late $late b31 after generation]
        assert_equal [gcmd_late $late b31 start thresholds] [gcmd_late $late b31 after thresholds]
        assert_equal 1 [gcmd_late $late b31 after pending]
        assert_equal 1 [gcmd_late $late b31 answer]
        assert_equal ACTIVE [gcmd_late $late b31 again state]
        assert_equal 1 [gcmd_late $late b31 again audit-samples]
        # 32: it stopped by itself. Nothing is tested (GINFO revocations
        # counts the stop); the request that found it so gets an id.
        foreach gate {blind b33} {
            lassign [gcmd_late $late $gate reply] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            assert_equal COLD [gcmd_late $late $gate after state]
            assert_equal 1 [gcmd_late $late $gate after revocations]
            assert_equal 0 [gcmd_late $late $gate after generation]
            assert_equal 0 [gcmd_late $late $gate after candidate]
            assert_equal {{}} [gcmd_late $late $gate after thresholds]
            assert_equal 0 [gcmd_late $late $gate after calibration-samples]
            assert_equal 1 [gcmd_late $late $gate after pending]
            # An answer arrives: a new snapshot, with a new number, is tested.
            assert_equal 1 [gcmd_late $late $gate answer]
            assert_equal CALIBRATING [gcmd_late $late $gate again state]
            assert_equal 2 [gcmd_late $late $gate again candidate]
            assert_equal 0 [gcmd_late $late $gate again pending]
            assert_equal 1 [gcmd_late $late $gate again revocations]
        }
        assert_equal 32 [gcmd_late $late blind after missing]
        assert_equal 32 [gcmd_late $late blind again missing]
        # 33: the last one was proof for what the gate dropped at the
        # 32nd. It is forgotten, not missing (CHANGES 9e).
        assert_equal 32 [gcmd_late $late b33 after missing]
        assert_equal 32 [gcmd_late $late b33 again missing]
    }

    gcmd_test {late answers: answers that are lost while another arrives are not an outage} {
        # "... and no answer of any request has arrived since the first of
        # them was asked." CHANGES 9b: 40 requests in one burst; the
        # answer of the newest arrives at once, the other 39 never do and
        # are given up together when their time is up. Each of them saw an
        # answer arrive after it was asked: 39 lost answers, no outage.
        # (Before, requests given up together were a run, whatever had
        # been answered in between.)
        assert_equal ABSTAIN [gcmd_late $late burst asked]
        assert_equal 1 [gcmd_late $late burst newest]
        assert_equal ACTIVE [gcmd_late $late burst start state]
        assert_equal 39 [gcmd_late $late burst start pending]
        assert_equal AUDIT [lindex [gcmd_late $late burst reply] 0]
        assert_equal ACTIVE [gcmd_late $late burst after state]
        assert_equal 39 [gcmd_late $late burst after missing]
        assert_equal 0 [gcmd_late $late burst after revocations]
        assert_equal 1 [gcmd_late $late burst after generation]
        assert_equal [gcmd_late $late burst start thresholds] [gcmd_late $late burst after thresholds]
        assert_equal 1 [gcmd_late $late burst after pending]
        assert_equal 1 [gcmd_late $late burst answer]
        assert_equal ACTIVE [gcmd_late $late burst again state]
        assert_equal 0 [gcmd_late $late burst again pending]
        # The same when the answer that arrives is the one of the oldest
        # request, after 32 more were asked: none of those 32 gets an
        # answer, but an answer has arrived since the first of them was
        # asked.
        assert_equal ABSTAIN [gcmd_late $late older asked]
        assert_equal 1 [gcmd_late $late older oldest]
        assert_equal ACTIVE [gcmd_late $late older start state]
        assert_equal 32 [gcmd_late $late older start pending]
        assert_equal AUDIT [lindex [gcmd_late $late older reply] 0]
        assert_equal ACTIVE [gcmd_late $late older after state]
        assert_equal 32 [gcmd_late $late older after missing]
        assert_equal 0 [gcmd_late $late older after revocations]
        assert_equal 1 [gcmd_late $late older after generation]
        assert_equal 1 [gcmd_late $late older after pending]
    }

    gcmd_test {late answers: lost spot checks stop a gate that answers, at the lost answer the table says} {
        # SPEC 3 "Getting worse": "A spot check whose answer is lost may
        # hide a disagreement: when so many are lost that the gate can no
        # longer rule that out, it stops answering, keeps its model, and
        # proves itself again." SPEC 5: at 10^9, with lost answers counted
        # as disagreements (worked out in the helpers test of section 8).
        # The gates answer everything (AUDIT 1): every request is a spot
        # check, the first one the first of a block.
        foreach {gate n} {u20 20 u19 19 u19w 19 ur22 22 ur23 23 s16 16 s15 15} {
            assert_equal AUDIT [gcmd_late $late $gate asked]
            assert_equal ACTIVE [gcmd_late $late $gate start state]
            assert_equal $n [gcmd_late $late $gate start pending]
            assert_equal 0 [gcmd_late $late $gate start missing]
            assert {[lindex [gcmd_late $late $gate start thresholds] 0] == 0}
        }
        # 20 spot checks, all lost: the 20th is the one.
        assert_equal 20 [gcmd_drift_lost_at 0.2 20]
        lassign [gcmd_late $late u20 reply] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        assert_equal COLD [gcmd_late $late u20 after state]
        assert_equal 20 [gcmd_late $late u20 after missing]
        assert_equal 1 [gcmd_late $late u20 after revocations]
        assert_equal 0 [gcmd_late $late u20 after generation]
        assert_equal 0 [gcmd_late $late u20 after candidate]
        assert_equal {{}} [gcmd_late $late u20 after thresholds]
        assert_equal 0 [gcmd_late $late u20 after audit-samples]
        assert_equal 1 [gcmd_late $late u20 after pending]
        # it proves itself again as soon as an answer arrives
        assert_equal 1 [gcmd_late $late u20 answer]
        assert_equal CALIBRATING [gcmd_late $late u20 again state]
        assert_equal 2 [gcmd_late $late u20 again candidate]
        assert_equal 20 [gcmd_late $late u20 again missing]
        assert_equal 1 [gcmd_late $late u20 again revocations]
        # 19 spot checks, all lost: the gate goes on answering.
        assert_equal 0 [gcmd_drift_lost_at 0.2 19]
        foreach gate {u19 u19w} {
            assert_equal AUDIT [lindex [gcmd_late $late $gate reply] 0]
            assert_equal ACTIVE [gcmd_late $late $gate after state]
            assert_equal 19 [gcmd_late $late $gate after missing]
            assert_equal 0 [gcmd_late $late $gate after revocations]
            assert_equal 1 [gcmd_late $late $gate after generation]
            # a lost answer is not a spot check that disagreed
            assert_equal 0 [gcmd_late $late $gate after audit-samples]
            assert_equal 0 [gcmd_late $late $gate after audit-errors]
            assert_equal 1 [gcmd_late $late $gate after pending]
            assert_equal 1 [gcmd_late $late $gate answer]
        }
        # ... the 20th spot check agrees: nothing happens
        assert_equal ACTIVE [gcmd_late $late u19 again state]
        assert_equal 1 [gcmd_late $late u19 again audit-samples]
        assert_equal 0 [gcmd_late $late u19 again audit-errors]
        assert_equal 0 [gcmd_late $late u19 again revocations]
        # ... the 20th spot check disagrees: 19 lost and 1 wrong of 20 is
        # the same number. The gate stops because it cannot be checked.
        set d [gcmd_drift_new 0.2]
        for {set i 0} {$i < 19} {incr i} {assert_equal {} [gcmd_drift_tell d [gcmd_drift_ask d] lost]}
        assert_equal unchecked [gcmd_drift_tell d [gcmd_drift_ask d] wrong]
        assert_equal COLD [gcmd_late $late u19w again state]
        assert_equal 1 [gcmd_late $late u19w again revocations]
        assert_equal 19 [gcmd_late $late u19w again missing]
        assert_equal 0 [gcmd_late $late u19w again generation]
        assert_equal 0 [gcmd_late $late u19w again candidate]
        assert_equal 0 [gcmd_late $late u19w again pending]
        # After five spot checks that agreed it takes 23, not 22 ("more
        # after right ones", CHANGES 9).
        assert_equal 0 [gcmd_drift_lost_at 0.2 22 5]
        assert_equal 23 [gcmd_drift_lost_at 0.2 23 5]
        foreach gate {ur22 ur23} {
            assert_equal {AUDIT AUDIT AUDIT AUDIT AUDIT} [gcmd_late $late $gate agreed]
            assert_equal 5 [gcmd_late $late $gate start audit-samples]
        }
        assert_equal AUDIT [lindex [gcmd_late $late ur22 reply] 0]
        assert_equal ACTIVE [gcmd_late $late ur22 after state]
        assert_equal 22 [gcmd_late $late ur22 after missing]
        assert_equal 0 [gcmd_late $late ur22 after revocations]
        assert_equal 5 [gcmd_late $late ur22 after audit-samples]
        assert_equal ABSTAIN [lindex [gcmd_late $late ur23 reply] 0]
        assert_equal COLD [gcmd_late $late ur23 after state]
        assert_equal 23 [gcmd_late $late ur23 after missing]
        assert_equal 1 [gcmd_late $late ur23 after revocations]
        assert_equal 0 [gcmd_late $late ur23 after generation]
        # With an error rate of 0.05 a lost answer counts 2 and 4: the
        # 16th is the one, 15 are not enough ("about 15 to 16 lost in a
        # row in a fresh block at small ALPHA", CHANGES 9).
        assert_equal 0 [gcmd_drift_lost_at 0.05 15]
        assert_equal 16 [gcmd_drift_lost_at 0.05 16]
        assert_equal AUDIT [lindex [gcmd_late $late s15 reply] 0]
        assert_equal ACTIVE [gcmd_late $late s15 after state]
        assert_equal 15 [gcmd_late $late s15 after missing]
        assert_equal 0 [gcmd_late $late s15 after revocations]
        assert_equal ABSTAIN [lindex [gcmd_late $late s16 reply] 0]
        assert_equal COLD [gcmd_late $late s16 after state]
        assert_equal 16 [gcmd_late $late s16 after missing]
        assert_equal 1 [gcmd_late $late s16 after revocations]
        assert_equal 0 [gcmd_late $late s16 after generation]
        assert_equal 0 [gcmd_late $late s16 after candidate]
    }

    gcmd_test {late answers: a classifier whose spot checks are lost stops answering and keeps its model} {
        # CHANGES 3 and 9f: lost answers say nothing about the model. (A
        # gate that got worse forgets it: section 8.) 32 spot checks of
        # the label "a" are asked before any of them is given up: the 25th
        # lost answer is the one (helpers test of section 8), and the
        # seven behind it were proof for what the gate dropped there.
        assert_equal 25 [gcmd_drift_lost_at 0.2 32]
        assert_equal AUDIT [gcmd_late $late bcls asked]
        assert_equal ACTIVE [gcmd_late $late bcls start state]
        assert_equal 32 [gcmd_late $late bcls start pending]
        assert_equal 0 [gcmd_late $late bcls start audit-samples]
        set learned [gcmd_late $late bcls start learned]
        assert {$learned >= 40}
        set numbers [expr {max([gcmd_late $late bcls start generation], [gcmd_late $late bcls start candidate])}]
        lassign [gcmd_late $late bcls reply] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        assert_equal COLD [gcmd_late $late bcls after state]
        assert_equal $learned [gcmd_late $late bcls after learned]
        assert_equal 25 [gcmd_late $late bcls after missing]
        assert_equal 1 [gcmd_late $late bcls after revocations]
        assert_equal 0 [gcmd_late $late bcls after generation]
        assert_equal 0 [gcmd_late $late bcls after candidate]
        assert_equal {{} {}} [gcmd_late $late bcls after thresholds]
        assert_equal 1 [gcmd_late $late bcls after pending]
        # an answer arrives: the model it kept is tested again, as a new snapshot
        assert_equal 1 [gcmd_late $late bcls answer]
        assert_equal CALIBRATING [gcmd_late $late bcls again state]
        assert_equal [expr {$learned + 1}] [gcmd_late $late bcls again learned]
        assert_equal [expr {$numbers + 1}] [gcmd_late $late bcls again candidate]
        assert_equal 25 [gcmd_late $late bcls again missing]
        assert_equal 1 [gcmd_late $late bcls again revocations]
    }

    gcmd_test {late answers: a classifier learns from a late answer and counts it for its snapshot} {
        assert_equal 1 [gcmd_late $late cls start candidate]
        assert_equal 1 [gcmd_late $late cls start pending]
        assert_equal 0 [gcmd_late $late cls start calibration-samples]
        assert_equal 2 [gcmd_late $late cls start learned]
        assert_equal 3 [gcmd_late $late cls moved calibration-samples]
        assert_equal 1 [gcmd_late $late cls moved pending]
        assert_equal 3 [gcmd_late $late cls moved learned]
        assert_equal 1 [gcmd_late $late cls answer]
        assert_equal 0 [gcmd_late $late cls after pending]
        assert_equal 0 [gcmd_late $late cls after missing]
        assert_equal 3 [gcmd_late $late cls after calibration-samples]
        # the example was learned when its label came
        assert_equal 4 [gcmd_late $late cls after learned]
        # three right guesses of "a" so far, of the 39 a label of two needs
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2 1 2] - 3}] [gcmd_late $late cls more]
    }

    gcmd_test {the server is still up (late and lost answers)} {
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 10. Random arguments and random sessions
# ============================================================================

# A random session of valid commands on the gate 'key' ('kind' is score or
# cls, a classifier of "spam" and "ham"): requests, answers in any order and
# mostly right, examples, pauses, now and then a reset. With 'timed' the
# session also sleeps a millisecond now and then: for gates created with a
# feedback timeout of a few milliseconds. After every command the GINFO of
# the gate must make sense, by itself and against the GINFO before; every
# 50 commands the gate must survive DUMP and RESTORE. From step 'flaky' on
# (if it is not negative) the teacher of a score gate disagrees with half
# of the confident results: a gate that got worse (such a session has no
# resets). Returns how many requests
# the session made, how many of them the gate answered, how often the gate
# stopped answering by itself, and the last GINFO.
proc gcmd_session {key kind steps timed {flaky -1}} {
    set asked 0
    set accepts 0
    set stops 0
    set ids {}                ;# requests the session has not answered: {id what}
    set last [r ginfo $key]
    set top [expr {max([dict get $last generation], [dict get $last candidate])}]
    set classes [llength [dict get $last classes]]
    set did {}
    for {set step 0} {$step < $steps} {incr step} {
        set roll [gcmd_rnd 1000]
        set reset 0
        if {$roll < 450} {
            if {$kind eq "score"} {
                set what [gcmd_pick {0.9 0.9 0.95 0.999 1 0.5 0.1 0}]
                set cmd [list gpredict $key SCORE $what]
            } else {
                set what [gcmd_rnd 10]
                set cmd [list gpredict $key TEXT [lindex [gcmd_example $what] 0]]
            }
            lassign [r {*}$cmd] action arg
            incr asked
            if {$action eq "ACCEPT"} {incr accepts}
            if {$action ni {ACCEPT AUDIT ABSTAIN}} {fail "$key: '$cmd' replied '$action $arg'"}
            if {$action ne "ACCEPT" && $arg ne {}} {lappend ids [list $arg $what]}
            if {$action eq "AUDIT" && $arg eq {}} {fail "$key: '$cmd' replied AUDIT without an id"}
        } elseif {$roll < 850} {
            if {[llength $ids] == 0} continue
            set at [expr {[gcmd_rnd 3] ? 0 : [gcmd_rnd [llength $ids]]}]
            lassign [lindex $ids $at] id what
            set ids [lreplace $ids $at $at]
            if {$kind eq "score"} {
                set right [expr {$what >= 0.5 ? [gcmd_rnd 40] > 0 : [gcmd_rnd 3] == 0}]
                if {$flaky >= 0 && $step >= $flaky} {set right [gcmd_rnd 2]}
                set cmd [list glearn $key $id CORRECT $right]
            } else {
                set label [lindex [gcmd_example $what] 1]
                if {[gcmd_rnd 40] == 0} {set label [gcmd_pick {spam ham}]}
                set cmd [list glearn $key $id LABEL $label]
            }
            set used [r {*}$cmd]
            # Without timeouts every id the session holds is one the gate
            # still waits for.
            if {$used ni {0 1} || (!$timed && $used != 1)} {
                fail "$key: '$cmd' replied '$used' (step $step, after: [join [lrange $did end-8 end] {; }])"
            }
        } elseif {$roll < 920} {
            if {$kind ne "cls"} continue
            lassign [gcmd_example [gcmd_rnd 10]] text label
            set cmd [list gadd $key TEXT $text LABEL $label]
            r {*}$cmd
        } elseif {$roll < 950} {
            set cmd [list [gcmd_pick {gpause gresume gresume}] $key]
            r {*}$cmd
        } elseif {$roll < 953} {
            # (a gate that is to get worse is left to get there)
            if {$flaky >= 0} continue
            set cmd [list greset $key]
            r {*}$cmd
            set ids {}
            set reset 1
        } else {
            if {!$timed} continue
            after 1
            set cmd {after 1}
        }
        lappend did $cmd
        set info [r ginfo $key]
        set shown "$key, step $step, after: [join [lrange $did end-8 end] {; }]\n$info\n"

        # ---- GINFO by itself
        if {[dict get $info requests] != [dict get $info accepted] + [dict get $info audited] +
                [dict get $info abstained]} {
            fail "the request counters do not add up ($shown)"
        }
        set generation [dict get $info generation]
        set candidate [dict get $info candidate]
        switch [dict get $info state] {
            ACTIVE {set fits [expr {$generation > 0}]}
            CALIBRATING {set fits [expr {$generation == 0 && $candidate > 0}]}
            COLD {set fits [expr {$generation == 0 && $candidate == 0}]}
            PAUSED {set fits 1}
            default {set fits 0}
        }
        if {!$fits} {fail "the state does not fit generation and candidate ($shown)"}
        if {$generation > 0 && $candidate > 0 && $candidate <= $generation} {
            fail "the snapshot being tested is not newer than the one answering ($shown)"
        }
        set proven 0
        foreach alpha [dict get $info alphas] threshold [dict get $info thresholds] {
            if {$threshold eq {}} continue
            incr proven
            if {$alpha == 0} {fail "a threshold for a label that is always deferred ($shown)"}
        }
        if {($generation > 0) != ($proven > 0)} {
            fail "a gate answers exactly when it has a threshold ($shown)"
        }
        if {$generation == 0 && ([dict get $info coverage] != 0 || [dict get $info audit-samples] != 0)} {
            fail "coverage or spot checks without a snapshot that answers ($shown)"
        }
        if {[dict get $info coverage] < 0 || [dict get $info coverage] > 1 ||
            [dict get $info audit-errors] > [dict get $info audit-samples] ||
            [dict get $info pending] > 4096 || [dict get $info memory] <= 0} {
            fail "a value out of its range ($shown)"
        }

        # ---- against the GINFO before
        # Snapshot numbers are never reused; a snapshot answers only after
        # it was the one being tested.
        if {$candidate != 0 && $candidate != [dict get $last candidate] && $candidate <= $top} {
            fail "the snapshot number $candidate was used before ($shown)"
        }
        if {$generation != 0 && $generation != [dict get $last generation] &&
            $generation != [dict get $last candidate]} {
            fail "the snapshot $generation answers without having been tested ($shown)"
        }
        set top [expr {max($top, $generation, $candidate)}]
        if {$generation != 0 && $generation == [dict get $last generation]} {
            # the same snapshot answers: its thresholds only move down, on its grid
            set lowest [expr {$kind eq "score" ? 0.0 : max(1.0 / $classes, 0.25)}]
            set highest [expr {$kind eq "score" ? 0.998 : 0.999}]
            foreach old [dict get $last thresholds] threshold [dict get $info thresholds] {
                if {$old eq $threshold} continue
                if {$threshold eq {} || ($old ne {} && $threshold > $old)} {
                    fail "a threshold moved up ($shown)"
                }
                if {[gcmd_grid_index $lowest $highest $threshold] < 0} {
                    fail "the threshold $threshold is not one of the 32 ($shown)"
                }
            }
        }
        if {!$reset} {
            foreach field {requests accepted audited abstained missing revocations} {
                if {[dict get $info $field] < [dict get $last $field]} {
                    fail "the counter $field went down ($shown)"
                }
            }
            if {[dict get $info revocations] > [dict get $last revocations]} {
                incr stops
                # a gate that stops answering answers nothing
                if {$generation != 0} {fail "the gate stopped and still answers ($shown)"}
            }
            if {[dict get $info learned] < [dict get $last learned] &&
                [dict get $info revocations] == [dict get $last revocations]} {
                fail "the gate forgot examples without stopping ($shown)"
            }
        }
        # GINFO pending is what the session knows to be waiting. (A score
        # gate that stops by itself drops the requests that were proof for
        # what it dropped.)
        if {!$timed} {
            if {$kind eq "score" && [dict get $info revocations] > [dict get $last revocations] && !$reset} {
                set ids {}
            }
            if {[dict get $info pending] != [llength $ids]} {
                fail "pending is [dict get $info pending], the session waits for [llength $ids] answers ($shown)"
            }
        } elseif {[dict get $info pending] > [llength $ids]} {
            fail "pending is [dict get $info pending], the session holds only [llength $ids] ids ($shown)"
        }
        set last $info
        if {$step % 50 == 49} {
            gcmd_check_saveable $key "($shown)"
            gcmd_check_recovery $key "($shown)"
        }
    }
    gcmd_check_saveable $key
    list $asked $accepts $stops $last
}

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
            # Whatever the commands so far made of the gates, each of them
            # can be saved and loaded.
            if {$it % 400 == 399} {
                foreach key {rnd:1 rnd:2 rnd:3 rnd:4} {
                    if {[r type $key] eq "gate-type"} {gcmd_check_saveable $key "(after $it random commands)"}
                }
            }
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

    gcmd_test {random sessions: GINFO stays consistent and every gate can be saved and loaded} {
        # (the same draws whether or not the test above ran)
        expr {srand(20261129)}
        gcmd_seed 77108
        set n 0
        set accepted 0
        set stopped 0
        set snapshots 0
        foreach {kind flaky options} {
            score -1  {ALPHA 0.2 DELTA 0.2 SCORE}
            score -1  {ALPHA 0.2 DELTA 0.2 SCORE AUDIT 1}
            score 600 {ALPHA 0.3 DELTA 0.9 SCORE AUDIT 0.3}
            score 300 {ALPHA 0.2 DELTA 0.5 SCORE AUDIT 1}
            cls   -1  {ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham}
            cls   -1  {ALPHA 0.2 DELTA 0.5 CLASSES 3 spam ham other ALWAYSDEFER 1 other AUDIT 0.5}
            cls   -1  {ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham CLASSALPHA 1 spam 0.1 AUDIT 1}
        } {
            incr n
            r del rs:$n
            assert_equal OK [r gcreate rs:$n {*}$options]
            lassign [gcmd_session rs:$n $kind 1200 0 $flaky] asked accepts stops end
            assert {$asked > 300}
            incr accepted $accepts
            incr stopped $stops
            incr snapshots [expr {max([dict get $end generation], [dict get $end candidate])}]
        }
        # The sessions got somewhere: gates answered by themselves, gates
        # stopped by themselves, snapshots were taken one after the other.
        assert {$accepted > 100}
        assert {$stopped > 0}
        assert {$snapshots > 14}
    }

    gcmd_test {random sessions with feedback timeouts of a few milliseconds} {
        # The same sessions with real time in them: requests time out,
        # proof moves on without late answers, answers stop arriving. What
        # happens when depends on the clock; what must hold does not.
        expr {srand(20261130)}
        gcmd_seed 77109
        set n 0
        foreach {kind timeout options} {
            score 4  {ALPHA 0.2 DELTA 0.2 SCORE AUDIT 0.5}
            score 12 {ALPHA 0.2 DELTA 0.2 SCORE AUDIT 1}
            score 40 {ALPHA 0.3 DELTA 0.9 SCORE AUDIT 0.3}
            cls   4  {ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham AUDIT 0.5}
            cls   12 {ALPHA 0.2 DELTA 0.5 CLASSES 3 spam ham other ALWAYSDEFER 1 other AUDIT 1}
            cls   40 {ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham}
        } {
            incr n
            r del rt:$n
            r config set gate.feedback-timeout $timeout
            assert_equal OK [r gcreate rt:$n {*}$options]
            r config set gate.feedback-timeout 60000
            lassign [gcmd_session rt:$n $kind 800 1] asked accepts stops end
            assert {$asked > 200}
        }
        assert_equal PONG [r ping]
    }
}

# ============================================================================
# 11. The internal command, called the way a replication link calls it
# ============================================================================
#
# Clients cannot call _GATE.APPLY (section 3); replicas and the AOF loader
# get it from a primary that realized every value it carries. DEBUG
# MARK-INTERNAL-CLIENT turns a test connection into such a caller, so that
# these tests can also send what no primary sends. The contract (gate.c,
# CHANGES 5, 8 and 9): the values are checked before they are used; an
# operation that does not fit its gate, or has too few words to be one, is
# skipped and counted (gate_apply_mismatches in INFO) and the reply is OK
# all the same; a gate that was changed this way is not trusted by this
# server any more. The last test of the section goes the other way: it
# applies what the server itself sends on.
#
#   _GATE.APPLY key CREATE now lineage epoch snapshot-examples timeout <GCREATE arguments>
#   _GATE.APPLY key PREDICT now draw has-serving label score has-candidate label score
#                           request-number [TEXT text [FEATURES ...]]
#   _GATE.APPLY key LEARN now id answer
#   _GATE.APPLY key ADD now TEXT text [FEATURES ...] LABEL label
#   _GATE.APPLY key PAUSE | RESUME | TICK now
#   _GATE.APPLY key RESET | RECOVER now epoch
#
# (draw and scores are the 16 hex digits of a double, epochs 16 hex digits)

# The 16 hex digits a double travels as.
proc gcmd_bits {value} {
    binary scan [binary format Q $value] H16 hex
    return $hex
}

# Operations that did not fit their gate, as INFO counts them.
proc gcmd_mismatches {} {
    if {![regexp {gate_apply_mismatches:(\d+)} [r info gate] -> n]} {
        fail "INFO gate has no gate_apply_mismatches: [r info gate]"
    }
    return $n
}

# Send one operation over the marked connection: the reply must be OK.
# Returns 1 if it was applied, 0 if it was counted as a mismatch.
proc gcmd_apply {key args} {
    set before [gcmd_mismatches]
    set reply [$::gcmd_internal _gate.apply $key {*}$args]
    if {$reply ne "OK"} {fail "_GATE.APPLY $key [string range $args 0 200] replied '$reply'"}
    set counted [expr {[gcmd_mismatches] - $before}]
    if {$counted ni {0 1}} {fail "one operation, $counted mismatches"}
    expr {!$counted}
}

# What the server has sent on since 'repl' was attached to it
# (attach_to_replication_stream), up to a marker written here: the
# _GATE.APPLY operations, each as the list {key operation value ...}.
set ::gcmd_marker 0
proc gcmd_stream_ops {repl} {
    r set gcmd:marker [incr ::gcmd_marker]
    set ops {}
    while 1 {
        set cmd [read_from_replication_stream $repl]
        if {$cmd eq {}} {fail "what the server sends on ended before the marker"}
        if {[lrange $cmd 0 2] eq [list set gcmd:marker $::gcmd_marker]} break
        if {[lindex $cmd 0] eq "_gate.apply"} {lappend ops [lrange $cmd 1 end]}
    }
    return $ops
}

# One command as an append only file holds it.
proc gcmd_aof_format {args} {
    set out "*[llength $args]\r\n"
    foreach arg $args {append out "\$[string length $arg]\r\n$arg\r\n"}
    return $out
}

start_server [list tags {"modules external:skip needs:debug"} overrides [list loadmodule $gatemodule]] {
    r config set gate.test-seed 77011

    set ::gcmd_internal {}
    catch {
        set ::gcmd_internal [redis_client]
        $::gcmd_internal debug mark-internal-client
    }

    gcmd_test {_GATE.APPLY: no mismatch is counted for what clients do} {
        assert_equal 0 [gcmd_mismatches]
        r del ia:score ia:cls
        r gcreate ia:score ALPHA 0.2 DELTA 0.2 SCORE
        gcmd_prove_score ia:score
        gcmd_create_fast ia:cls ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham
        gcmd_prove_classifier ia:cls
        foreach {key answer} {ia:score {CORRECT 1} ia:cls {LABEL spam}} {
            r gpause $key
            r gresume $key
            assert_error "ERR *" {r gpredict $key BOGUS x}
            assert_equal 0 [r glearn $key not-an-id {*}$answer]
            assert_equal 0 [r glearn $key 0123456789abcdef-1 {*}$answer]
            r greset $key
        }
        assert_equal 0 [gcmd_mismatches]
        # the counter is one line of INFO, in a section of the module
        assert_match "*# gate*gate_apply_mismatches:0*" [r info gate]
        assert_match "*gate_apply_mismatches:0*" [r info everything]
        r del ia:score ia:cls
    }

    gcmd_test {_GATE.APPLY: what fits is applied, and the gate is no longer trusted by this server} {
        set now [expr {[clock milliseconds] + 1000}]
        set half [gcmd_bits 0.5]
        r del ia:score ia:cls ia:new
        # CREATE: the gate is there; GINFO shows it as its first write will
        # leave it.
        assert_equal 1 [gcmd_apply ia:new CREATE $now 00000000000000aa 00000000000000bb 1000 60000 \
                            ALPHA 0.3 DELTA 0.2 CLASSES 2 x y]
        set info [r ginfo ia:new]
        assert_equal classifier [dict get $info kind]
        assert_equal {x y} [dict get $info classes]
        assert {[dict get $info alpha] == 0.3}
        assert_equal COLD [dict get $info state]
        gcmd_check_saveable ia:new
        r del ia:new

        # A gate of this server that answers; then one operation from
        # outside (here: the clock, which changes nothing).
        r gcreate ia:score ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        gcmd_prove_all ia:score
        lassign [r gpredict ia:score SCORE 0.9] action waiting
        assert_equal AUDIT $action
        set before [r ginfo ia:score]
        assert_equal ACTIVE [dict get $before state]
        set digest [debug_digest_value ia:score]
        assert_equal 1 [gcmd_apply ia:score TICK $now]
        assert_equal $digest [debug_digest_value ia:score]
        # GINFO shows that the gate will start over (CHANGES 8: a node must
        # not answer on proof it does not hold alone) ...
        set shown [r ginfo ia:score]
        assert_equal CALIBRATING [dict get $shown state]
        assert_equal 0 [dict get $shown generation]
        assert_equal 2 [dict get $shown candidate]
        assert_equal {{}} [dict get $shown thresholds]
        assert_equal 0 [dict get $shown pending]
        assert_equal [dict get $before requests] [dict get $shown requests]
        # ... and its first write makes it so
        lassign [r gpredict ia:score SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 0 [r glearn ia:score $waiting CORRECT 1]
        assert_equal 1 [r glearn ia:score $id CORRECT 1]
        set after [r ginfo ia:score]
        assert_equal CALIBRATING [dict get $after state]
        assert_equal 2 [dict get $after candidate]
        assert_equal 1 [dict get $after calibration-samples]

        # PAUSE and RESUME; a request and its answer (the request counters
        # and what the gate holds show them; proof is not shown for a gate
        # this server does not trust)
        assert_equal 1 [gcmd_apply ia:score PAUSE $now]
        assert_equal PAUSED [gcmd_info ia:score state]
        assert_equal 1 [gcmd_apply ia:score RESUME $now]
        assert_equal CALIBRATING [gcmd_info ia:score state]
        set info [r ginfo ia:score]
        set epoch [string range $id 0 15]
        set seq [expr {[lindex [split $id -] 1] + 1}]
        assert_equal 1 [gcmd_apply ia:score PREDICT $now $half 0 -1 $half 1 -1 [gcmd_bits 0.9] $seq]
        assert_equal [expr {[dict get $info requests] + 1}] [gcmd_info ia:score requests]
        assert {[gcmd_info ia:score memory] > [dict get $info memory]}
        set digest [debug_digest_value ia:score]
        assert_equal 1 [gcmd_apply ia:score LEARN $now $epoch-$seq 1]
        assert {[gcmd_info ia:score memory] == [dict get $info memory]}
        if {[gcmd_has_digest]} {assert {$digest ne [debug_digest_value ia:score]}}
        gcmd_check_saveable ia:score

        # the same for a classifier: its request carries the text
        r gcreate ia:cls ALPHA 0.2 CLASSES 2 a b
        assert_equal 1 [gcmd_apply ia:cls ADD $now TEXT hello LABEL b]
        assert_equal 1 [gcmd_info ia:cls learned]
        assert_equal 1 [gcmd_apply ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 1 TEXT hello]
        assert_equal 1 [gcmd_info ia:cls requests]
        assert_equal 1 [gcmd_apply ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 5 TEXT hello FEATURES 1 x 1]
        assert_equal 2 [gcmd_info ia:cls requests]
        # RESET and RECOVER set the number ids start with
        assert_equal 1 [gcmd_apply ia:cls RECOVER $now 00000000000000ee]
        assert_equal 1 [gcmd_info ia:cls learned]
        assert_equal 1 [gcmd_apply ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 6 TEXT hello]
        assert_equal 1 [gcmd_apply ia:cls LEARN $now 00000000000000ee-6 0]
        assert_equal 2 [gcmd_info ia:cls learned]
        assert_equal 1 [gcmd_apply ia:cls RESET $now 00000000000000ff]
        assert_equal 0 [gcmd_info ia:cls learned]
        assert_equal 0 [gcmd_info ia:cls requests]
        gcmd_check_saveable ia:cls
        r del ia:score ia:cls
    }

    gcmd_test {_GATE.APPLY: what does not fit is skipped, counted and answered OK} {
        set now [expr {[clock milliseconds] + 1000}]
        set half [gcmd_bits 0.5]
        r del ia:score ia:cls ia:none ia:string
        r set ia:string value
        r gcreate ia:score ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate ia:cls ALPHA 0.2 CLASSES 2 a b
        set id [lindex [r gpredict ia:score SCORE 0.9] 1]
        set epoch [string range $id 0 15]
        r gpredict ia:cls TEXT hello
        set before [list [debug_digest_value ia:score] [debug_digest_value ia:cls]]
        # (one millisecond past the last value of the clock, which is 7
        # days before the end of 64 bits)
        set past [expr {9223372036854775807 - 7 * 86400 * 1000 + 1}]
        foreach op [list \
            [list ia:score PREDICT $now [gcmd_bits 1.5] 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT $now [gcmd_bits 1.0] 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT $now [gcmd_bits -0.5] 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 [gcmd_bits 1.5] 2] \
            [list ia:score PREDICT $now $half 0 -1 [gcmd_bits 1.5] 1 -1 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 7ff8000000000000 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 7ff0000000000000 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 0.5 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 64 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -2 $half 2] \
            [list ia:score PREDICT $now $half 0 64 $half 1 -1 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 5 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 0 $half 2] \
            [list ia:score PREDICT $now $half 0 0 $half 1 -1 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 2 TEXT hello] \
            [list ia:score PREDICT $now $half 0 -1 $half 0 -1 $half 2] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 1] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 0] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half -1] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half two] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 4611686018427387904] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 9223372036854775807] \
            [list ia:score PREDICT $now $half 0 -1 $half 1 -1 $half 9223372036854775808] \
            [list ia:score PREDICT -1 $half 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT 9223372036854775807 $half 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT $past $half 0 -1 $half 1 -1 $half 2] \
            [list ia:score PREDICT soon $half 0 -1 $half 1 -1 $half 2] \
            [list ia:score TICK $past] \
            [list ia:score LEARN $now $id 2] \
            [list ia:score LEARN $now $id -1] \
            [list ia:score LEARN $now $id yes] \
            [list ia:score LEARN $now garbage 1] \
            [list ia:score LEARN $now $id] \
            [list ia:score LEARN $now $id 1 extra] \
            [list ia:score ADD $now TEXT hello LABEL a] \
            [list ia:score BOGUS $now] \
            [list ia:score PAUSE] \
            [list ia:score PAUSE now] \
            [list ia:score PAUSE $now extra] \
            [list ia:score TICK -5] \
            [list ia:score RESET $now nothex] \
            [list ia:score RESET $now] \
            [list ia:score RECOVER $now 123] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 2] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 2 TEXT] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 2 TEXT hello extra] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 2 TEXT hello FEATURES 2 x 1 x 2] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 2 TEXT hello FEATURES 1 x nan] \
            [list ia:cls PREDICT $now $half 0 -1 $half 0 -1 $half 1 TEXT hello] \
            [list ia:cls LEARN $now $epoch-1 2] \
            [list ia:cls ADD $now TEXT hello LABEL nosuch] \
            [list ia:cls ADD $now TEXT hello] \
            [list ia:cls ADD $now TEXT hello FEATURES 2 x 1 x 2 LABEL a] \
            [list ia:cls ADD $now TEXT [string repeat x 1048577] LABEL a] \
            [list ia:none PAUSE $now] \
            [list ia:none PREDICT $now $half 0 -1 $half 1 -1 $half 1] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 1000 60000 ALPHA 7 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 0 60000 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 1000000001 60000 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 4294967297 60000 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 1000 0 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 1000 604800001 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa nothex 1000 60000 ALPHA 0.2 SCORE] \
            [list ia:none CREATE $now 00000000000000aa 00000000000000bb 1000 60000] \
            [list ia:none RESTORE garbage] \
            [list ia:none RESTORE] \
            [list ia:string PAUSE $now] \
            [list ia:string LEARN $now $id 1] \
            [list ia:string TICK $now] \
        ] {
            assert_equal [gcmd_apply {*}$op] 0 "_GATE.APPLY [string range $op 0 160] was applied"
        }
        # nothing of it changed a gate or made a key
        assert_equal $before [list [debug_digest_value ia:score] [debug_digest_value ia:cls]]
        assert_equal 0 [r exists ia:none]
        assert_equal value [r get ia:string]
        gcmd_check_saveable ia:score
        gcmd_check_saveable ia:cls
        # the gates go on working for their clients
        assert_equal 1 [r glearn ia:cls [lindex [r gpredict ia:cls TEXT hello] 1] LABEL a]
        assert_equal 1 [r glearn ia:score [lindex [r gpredict ia:score SCORE 0.5] 1] CORRECT 1]
        r del ia:score ia:cls ia:string
    }

    gcmd_test {_GATE.APPLY: an operation of fewer than three words is counted like any other that does not fit} {
        # CHANGES 9: "_GATE.APPLY with fewer than three words is counted as
        # a mismatch and replies OK (an error reply while loading an AOF
        # can stop the server)". So can an error reply to a primary, on a
        # replica that is set to stop on one. (The file itself: below.)
        r del ia:short
        r gcreate ia:short ALPHA 0.2 DELTA 0.2 SCORE
        set digest [debug_digest_value ia:short]
        set failures {}
        foreach op {{ia:short} {}} {
            set before [gcmd_mismatches]
            set rc [catch {$::gcmd_internal _gate.apply {*}$op} reply]
            set counted [expr {[gcmd_mismatches] - $before}]
            if {$rc || $reply ne "OK" || $counted != 1} {
                lappend failures "_GATE.APPLY [expr {$op eq {} ? "alone" : "with a key and nothing else"}]:\
                    [expr {$rc ? "the error" : "the reply"}] '$reply', counted $counted times as an\
                    operation that did not fit (expected: the reply OK, counted once)"
            }
        }
        assert_equal $digest [debug_digest_value ia:short]
        r del ia:short
        if {[llength $failures]} {fail [join $failures "\n"]}
    }

    gcmd_test {_GATE.APPLY: a request number of 2^62 or more is refused; the last one below it leaves a gate that can be loaded} {
        # The request number of a PREDICT operation is taken over by the
        # gate (CHANGES 8). No counter of a gate goes above 2^62, and the
        # module refuses to load one that did (CHANGES 9d): an operation
        # with such a number does not fit (CHANGES 9, gate.c). It is
        # counted and changes nothing, the numbers of the gate go on from
        # where they were.
        set now [expr {[clock milliseconds] + 1000}]
        set half [gcmd_bits 0.5]
        foreach seq {4611686018427387904 4611686018427387905 9223372036854775807} {
            set shown "the request number $seq"
            r del ia:odd
            r gcreate ia:odd ALPHA 0.2 DELTA 0.2 SCORE
            set digest [debug_digest_value ia:odd]
            assert_equal 0 [gcmd_apply ia:odd PREDICT $now $half 0 -1 $half 1 -1 $half $seq] $shown
            assert_equal $digest [debug_digest_value ia:odd] $shown
            gcmd_check_saveable ia:odd "(after a PREDICT with $shown)"
            assert_equal 1 [gcmd_apply ia:odd PREDICT $now $half 0 -1 $half 1 -1 $half 1] $shown
            gcmd_check_saveable ia:odd "(after a PREDICT with $shown and one with the number 1)"
        }
        # The largest number that fits is 2^62 - 1. After it the gate has
        # no number left for another request: it answers ABSTAIN without
        # an id, like a gate that is full (CHANGES 9d), and it is still a
        # gate that can be saved and loaded. (A gate that its own DUMP
        # cannot bring back also stops the next restart of the server at
        # its RDB file.)
        r del ia:odd
        r gcreate ia:odd ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal 1 [gcmd_apply ia:odd PREDICT $now $half 0 -1 $half 1 -1 $half 4611686018427387903]
        set failures {}
        if {[catch {gcmd_check_saveable ia:odd} err]} {
            regsub {^assertion:} $err {} err
            lappend failures "after _GATE.APPLY key PREDICT $now $half 0 -1 $half 1 -1 $half 4611686018427387903\
                (2^62 - 1, applied) on a new score gate: $err"
        }
        assert_equal {ABSTAIN {}} [r gpredict ia:odd SCORE 0.5]
        assert_equal {ABSTAIN {}} [r gpredict ia:odd SCORE 0.9]
        set info [r ginfo ia:odd]
        assert_equal 0 [dict get $info pending]
        assert_equal 3 [dict get $info requests]
        assert_equal 3 [dict get $info abstained]
        if {[catch {gcmd_check_saveable ia:odd} err]} {
            regsub {^assertion:} $err {} err
            lappend failures "the same gate after two requests of a client: $err"
        }
        r del ia:odd
        if {[llength $failures]} {fail [join $failures "\n"]}
    }

    gcmd_test {_GATE.APPLY: a PREDICT that records nothing gives its request number back} {
        # CHANGES 9: "... and gives the number back if nothing was
        # recorded under it."
        set now [expr {[clock milliseconds] + 1000}]
        set half [gcmd_bits 0.5]
        # A new score gate tests its first snapshot: an operation without
        # the guess of that snapshot does not fit, whatever its number.
        r del ia:back
        r gcreate ia:back ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal 0 [gcmd_apply ia:back PREDICT $now $half 0 -1 $half 0 -1 $half 1000]
        assert_equal 1 [gcmd_apply ia:back PREDICT $now $half 0 -1 $half 1 -1 $half 1]
        # (the number 1 is taken now)
        assert_equal 0 [gcmd_apply ia:back PREDICT $now $half 0 -1 $half 1 -1 $half 1]
        # a client goes on with the number 2
        lassign [r gpredict ia:back SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert_equal 2 [lindex [split $id -] 1]
        gcmd_check_saveable ia:back
        # A gate that answers. A request it answers by itself (the draw
        # 0.5 is no spot check with AUDIT 0.05, the score 0.9 is above
        # its threshold 0) records nothing, so no primary sends it: it
        # does not fit, and the spot check a primary does send, under the
        # next number of the gate, still fits.
        r del ia:back
        r gcreate ia:back ALPHA 0.2 DELTA 0.2 SCORE
        set next [expr {[gcmd_prove_all ia:back] + 1}]
        assert_equal [expr {[gcmd_proof_needed 0.2 0.2] + 1}] $next
        assert_equal ACTIVE [gcmd_info ia:back state]
        set digest [debug_digest_value ia:back]
        assert_equal 0 [gcmd_apply ia:back PREDICT $now $half 1 -1 [gcmd_bits 0.9] 0 -1 [gcmd_bits 0.0] [expr {$next + 7}]]
        assert_equal $digest [debug_digest_value ia:back]
        assert_equal 1 [gcmd_apply ia:back PREDICT $now [gcmd_bits 0.01] 1 -1 [gcmd_bits 0.9] 0 -1 [gcmd_bits 0.0] $next]
        if {[gcmd_has_digest]} {assert {$digest ne [debug_digest_value ia:back]}}
        gcmd_check_saveable ia:back
        r del ia:back
    }

    gcmd_test {_GATE.APPLY: what the commands of clients send on fits a gate that follows them, to the last value of the clock} {
        # SPEC 3 "Redis": replicas "receive what actually happened". Here
        # the test is the replica: it reads what the server sends on and
        # applies it, operation by operation, to a twin of each gate.
        # Every operation must fit and each twin must end as the same
        # gate. The first gate starts three milliseconds before the end of
        # the range of the clock, with a feedback timeout of two (CHANGES
        # 9d: "the clock a command realizes is clamped at
        # GATE_MAX_CLOCK_MS"; an operation with a later clock is refused
        # by whoever gets it). The others are random sessions with
        # feedback timeouts of a few milliseconds: late and lost answers,
        # spot checks, alarms.
        set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
        r del tw:clock tw:clock:twin tw:score tw:score:twin tw:cls tw:cls:twin
        foreach key {tw:clock tw:clock:twin} {
            assert_equal 1 [gcmd_apply $key CREATE [expr {$last_clock - 200}] 00000000000000aa 00000000000000bb \
                                1000 100 ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        }
        assert_equal 0 [gcmd_apply tw:clock TICK [expr {$last_clock + 1}]]
        set repl [attach_to_replication_stream]
        set rc [catch {
            # ---- the gate that reaches the end of the clock. (What is
            # lost when depends on the machine; what is asserted does not.)
            expr {srand(20261202)}
            set ids {}
            for {set i 0} {$i < 3} {incr i} {
                lassign [r gpredict tw:clock SCORE 0.9] action id
                assert_equal ABSTAIN $action
                lappend ids $id
            }
            r glearn tw:clock [lindex $ids 2] CORRECT 1
            after 130
            # 70 ms before the end: the two requests without an answer
            # are older than the feedback timeout, 100 ms
            lassign [r gpredict tw:clock SCORE 0.9] action id
            assert_equal ABSTAIN $action
            r glearn tw:clock $id CORRECT 1
            assert_equal 0 [r glearn tw:clock [lindex $ids 0] CORRECT 1]
            assert_equal 0 [r glearn tw:clock [lindex $ids 1] CORRECT 1]
            set info [r ginfo tw:clock]
            assert {[dict get $info missing] >= 2}
            assert_equal 0 [dict get $info pending]
            after 80
            # the clock has reached its end: it stays there, nothing that
            # is asked from here on gets older
            assert_equal OK [r gpause tw:clock]
            assert_equal OK [r gresume tw:clock]
            gcmd_session tw:clock score 250 0

            # ---- two sessions with feedback timeouts of 12 ms
            r config set gate.feedback-timeout 12
            r config set gate.snapshot-examples 5
            assert_equal OK [r gcreate tw:score ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
            assert_equal OK [r gcreate tw:cls ALPHA 0.2 DELTA 0.5 AUDIT 0.5 CLASSES 3 spam ham other ALWAYSDEFER 1 other]
            r config set gate.feedback-timeout 60000
            r config set gate.snapshot-examples 1000
            gcmd_session tw:score score 500 1
            gcmd_session tw:cls cls 500 1
            set ops [gcmd_stream_ops $repl]
        } err]
        close_replication_stream $repl
        if {$rc} {error $err}

        # ---- the twins
        set applied {}
        foreach op $ops {
            set key [lindex $op 0]
            if {$key ni {tw:clock tw:score tw:cls}} continue
            dict incr applied $key
            set shown "_GATE.APPLY [string range $op 0 160] (operation [dict get $applied $key] of $key)"
            # (RESTORE has a blob where the others have the clock)
            if {[lindex $op 1] ne "RESTORE" && [lindex $op 2] > $last_clock} {
                fail "$shown: the clock [lindex $op 2] is past its last value, $last_clock"
            }
            if {![gcmd_apply $key:twin {*}[lrange $op 1 end]]} {
                fail "$shown did not fit the twin of the gate"
            }
        }
        foreach key {tw:clock tw:score tw:cls} {
            assert {[dict get $applied $key] > 20}
            assert_equal [debug_digest_value $key] [debug_digest_value $key:twin] "the twin of $key"
            gcmd_check_saveable $key:twin
        }
        assert {[dict get $applied tw:score] > 300 && [dict get $applied tw:cls] > 300}
        r del tw:clock tw:clock:twin tw:score tw:score:twin tw:cls tw:cls:twin
    }

    gcmd_test {_GATE.APPLY: random operations are answered OK, the gates stay gates, the server stays up} {
        expr {srand(20261201)}
        set now [clock milliseconds]
        set keys {ir:score ir:cls ir:string ir:none}
        r del {*}$keys
        r set ir:string value
        set texts {hello "buy cheap pills now" {} "a b c d e f"}
        set bits [list [gcmd_bits 0.0] [gcmd_bits 0.5] [gcmd_bits 0.9] [gcmd_bits 0.999] [gcmd_bits 1.0] \
                      [gcmd_bits 0.04] [gcmd_bits 0.3]]
        set wild [list [gcmd_bits 1.5] [gcmd_bits -0.5] 7ff0000000000000 7ff8000000000000 fff0000000000000 \
                      0000000000000001 8000000000000000 zz {} 0.5 3fe 3FE0000000000000]
        set junk [list {} x -1 0 1 2 63 64 65 4294967296 9223372036854775807 -9223372036854775808 \
                      18446744073709551616 TEXT LABEL FEATURES ALPHA SCORE CLASSES a b nosuch 0.5 nan \
                      00000000000000bb 00000000000000bb-1 "a\x00b" [binary format H* ff] [string repeat z 300]]
        set epochs {00000000000000bb 00000000000000dd 00000000000000ee}
        set seen {}
        set applied 0
        for {set it 0} {$it < 5000} {incr it} {
            # (re)create the two gates now and then, the way a primary does
            if {$it % 1250 == 0} {
                gcmd_apply ir:score CREATE $now 00000000000000aa 00000000000000bb 1000 [gcmd_pick {5 60000}] \
                    ALPHA 0.2 DELTA 0.2 AUDIT [gcmd_pick {1 0.5 0.05}] SCORE
                gcmd_apply ir:cls CREATE $now 00000000000000cc 00000000000000dd [gcmd_pick {1 3 1000}] \
                    [gcmd_pick {5 60000}] ALPHA 0.2 DELTA 0.2 CLASSES 3 a b c ALWAYSDEFER 1 c
                # the number the next request of each gate gets
                unset -nocomplain ir_next
                array set ir_next {ir:score 1 ir:cls 1 ir:string 1 ir:none 1}
            }
            set key [gcmd_pick {ir:score ir:score ir:cls ir:cls ir:string ir:none}]
            set classifier [expr {$key eq "ir:cls"}]
            incr now [gcmd_pick {0 0 0 1 1 2 7 30 -5}]
            set t [expr {[gcmd_rnd 20] ? $now : [gcmd_pick {0 -1 x 9223372036854775807 9223372036854775000}]}]
            set slabel [gcmd_pick {-1 -1 -1 0 1 2 3 63 64 -2 x}]
            set clabel [gcmd_pick {-1 -1 -1 0 1 2 3 63 64 -2 x}]
            # Request numbers stay small: the test above is about the large ones.
            set seq [expr {$ir_next($key) + [gcmd_pick {0 0 0 0 0 0 1 3 -1 -100}]}]
            set recorded 0
            switch [gcmd_rnd 12] {
                0 - 1 - 2 {
                    set op [list PREDICT $t [gcmd_pick $bits] [gcmd_rnd 2] $slabel [gcmd_pick $bits] \
                                [gcmd_rnd 2] $clabel [gcmd_pick $bits] $seq]
                    if {[gcmd_rnd 5] ? $classifier : !$classifier} {lappend op TEXT [gcmd_pick $texts]}
                    if {$classifier && [gcmd_rnd 5] == 0} {lappend op FEATURES 1 x [gcmd_pick {1 -1 nan}]}
                    set recorded 1
                }
                3 {
                    set op [list PREDICT $t [gcmd_pick [concat $bits $wild]] [gcmd_pick {0 1 2 -1 x}] $slabel \
                                [gcmd_pick [concat $bits $wild]] [gcmd_pick {0 1 2 -1 x}] $clabel \
                                [gcmd_pick [concat $bits $wild]] $seq]
                    if {$classifier && [gcmd_rnd 2]} {lappend op TEXT [gcmd_pick $texts]}
                    set recorded 1
                }
                4 - 5 - 6 {
                    set op [list LEARN $t [gcmd_pick $epochs]-[expr {1 + [gcmd_rnd $ir_next($key)]}] \
                                [gcmd_pick {0 1 0 1 0 1 2 3 -1 x}]]
                }
                7 {
                    set op [list ADD $t TEXT [gcmd_pick $texts] {*}[gcmd_pick {{} {FEATURES 1 x 1} {FEATURES 1 x nan}}] \
                                LABEL [gcmd_pick {a b c a b nosuch}]]
                }
                8 {set op [list [gcmd_pick {PAUSE RESUME TICK TICK TICK}] $t]}
                9 {
                    set op [list [gcmd_pick {RESET RECOVER RECOVER}] $t [gcmd_pick [concat $epochs {zz {}}]]]
                }
                10 {set op [gcmd_picks $junk 1 8]}
                11 {
                    set op [list [gcmd_pick {PREDICT LEARN ADD PAUSE RESUME TICK RESET RECOVER CREATE RESTORE}] \
                                {*}[gcmd_picks $junk 0 10]]
                }
            }
            set shown "_GATE.APPLY $key [string range $op 0 200]"
            if {[catch {gcmd_apply $key {*}$op} fits]} {
                fail "$shown: $fits"
            }
            dict incr seen [lindex $op 0]
            incr applied $fits
            # a request that was applied took its number
            if {$fits && $recorded} {set ir_next($key) [expr {$seq + 1}]}
            if {$it % 50 == 49} {
                foreach gate {ir:score ir:cls} {
                    set info [r ginfo $gate]
                    assert_equal [dict get $info requests] [expr {[dict get $info accepted] +
                        [dict get $info audited] + [dict get $info abstained]}] $shown
                    assert {[dict get $info pending] <= 4096}
                    assert {[dict get $info state] in {COLD CALIBRATING ACTIVE PAUSED}}
                    gcmd_check_saveable $gate "(after $shown)"
                }
                assert_equal value [r get ir:string]
                assert_equal 0 [r exists ir:none]
            }
        }
        # the loop did what it was meant to: operations of every kind, and
        # a good share of them applied
        foreach name {PREDICT LEARN ADD PAUSE RESET RECOVER TICK} {assert {[dict exists $seen $name]}}
        assert {$applied > 1000}
        # public commands find working gates
        foreach gate {ir:score ir:cls} {
            assert_equal OK [r greset $gate]
            gcmd_check_saveable $gate
        }
        lassign [r gpredict ir:score SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn ir:score $id CORRECT 1]
        assert_equal 1 [r gadd ir:cls TEXT hello LABEL a]
        assert_equal PONG [r ping]
    }

    gcmd_test {_GATE.APPLY: random operations at the end of the clock and of the request numbers leave gates that can be loaded} {
        # CHANGES 9d: "A gate that decodes can therefore always be encoded
        # and decoded again, whatever the core did with it in between".
        # The same must hold for a gate that operations take to the last
        # value of its clock (7 days before the end of 64 bits) and to its
        # last request numbers (below 2^62): after every operation each
        # gate is saved and loaded. Nothing past either end is applied.
        expr {srand(20261203)}
        set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
        set last_count [expr {1 << 62}]
        set bits [list [gcmd_bits 0.0] [gcmd_bits 0.5] [gcmd_bits 0.9] [gcmd_bits 0.04]]
        set failures {}
        set applied 0
        set refused 0
        unset -nocomplain ib_next ib_epoch ib_asked
        for {set round 0} {$round < 12 && [llength $failures] < 3} {incr round} {
            r del ib:score ib:cls
            set now [expr {$last_clock - [gcmd_pick {0 1 3 50 100000}]}]
            assert_equal 1 [gcmd_apply ib:score CREATE $now 00000000000000aa 00000000000000bb 1000 [gcmd_pick {2 60000}] \
                                ALPHA 0.2 DELTA 0.2 AUDIT [gcmd_pick {1 0.5}] SCORE]
            assert_equal 1 [gcmd_apply ib:cls CREATE $now 00000000000000cc 00000000000000dd 1000 [gcmd_pick {2 60000}] \
                                ALPHA 0.2 DELTA 0.2 CLASSES 3 a b c ALWAYSDEFER 1 c]
            # the number the next request of each gate gets, the first
            # number of its ids, and the numbers that may be waiting
            foreach key {ib:score ib:cls} epoch {00000000000000bb 00000000000000dd} {
                set ib_next($key) [expr {$last_count - [gcmd_pick {40 5 3 2 1}]}]
                set ib_epoch($key) $epoch
                set ib_asked($key) {}
            }
            set did {}
            for {set it 0} {$it < 40} {incr it} {
                set key [gcmd_pick {ib:score ib:cls}]
                # (time passes; at the end of the clock most operations
                # stay at its last value, some try to go past it)
                set t [expr {$now + [gcmd_pick {0 0 0 1 1 2 30}]}]
                if {$t > $last_clock && [gcmd_rnd 4]} {set t $last_clock}
                set now [expr {min($t, $last_clock)}]
                set seq [expr {$ib_next($key) + [gcmd_pick {0 0 0 0 1 2}]}]
                set recorded 0
                switch [gcmd_rnd 8] {
                    0 - 1 - 2 - 3 {
                        # a request: the score gate tests a snapshot, the
                        # classifier has learned nothing yet
                        if {$key eq "ib:score"} {
                            set score [gcmd_pick $bits]
                            set op [list PREDICT $t [gcmd_pick $bits] 0 -1 $score 1 -1 $score $seq]
                        } else {
                            set op [list PREDICT $t [gcmd_pick $bits] 0 -1 [gcmd_bits 0.0] 0 -1 [gcmd_bits 0.0] $seq \
                                        TEXT [gcmd_pick {hello "buy cheap pills now"}]]
                        }
                        set recorded 1
                    }
                    4 - 5 {
                        set number [gcmd_pick [concat $ib_asked($key) [list $ib_next($key) 1]]]
                        set op [list LEARN $t $ib_epoch($key)-$number [gcmd_rnd 2]]
                    }
                    6 {set op [list [gcmd_pick {TICK TICK PAUSE RESUME}] $t]}
                    7 {
                        set ib_epoch($key) [gcmd_pick {00000000000000ee 00000000000000ff}]
                        set op [list [gcmd_pick {RECOVER RECOVER RESET}] $t $ib_epoch($key)]
                        set ib_asked($key) {}
                    }
                }
                lappend did "$key $op"
                set shown "round $round, after: [join [lrange $did end-5 end] {; }]"
                set fits [gcmd_apply $key {*}$op]
                incr applied $fits
                # nothing is applied past either end
                if {$t > $last_clock || ($recorded && $seq >= $last_count)} {
                    incr refused
                    if {$fits} {fail "an operation past the end of the clock or of the request numbers was applied ($shown)"}
                } elseif {$recorded && !$fits} {
                    fail "a request below the last number did not fit ($shown)"
                }
                if {$fits && $recorded} {
                    set ib_next($key) [expr {$seq + 1}]
                    lappend ib_asked($key) $seq
                }
                set broken 0
                foreach gate {ib:score ib:cls} {
                    if {[catch {gcmd_check_saveable $gate} err]} {
                        regsub {^assertion:} $err {} err
                        lappend failures "$err ($shown)"
                        set broken 1
                    }
                }
                if {$broken} break
            }
        }
        r del ib:score ib:cls
        if {[llength $failures]} {fail [join $failures "\n"]}
        # the loop did what it was meant to
        assert {$applied > 200 && $refused > 20}
    }

    catch {$::gcmd_internal close}
}

# The append only file is the other caller of the internal command, and one
# that runs a command without looking at how many words it has.
start_server [list tags {"modules external:skip needs:debug"} overrides [list loadmodule $gatemodule appendonly yes]] {
    r config set gate.test-seed 77012

    gcmd_test {_GATE.APPLY: an append only file with operations of fewer than three words is loaded to its end} {
        # CHANGES 9: "_GATE.APPLY with fewer than three words is counted
        # as a mismatch and replies OK (an error reply while loading an
        # AOF can stop the server)." The file here is the server's own,
        # with three lines more behind its back: the command with a key
        # and nothing else, the command alone, and an ordinary write that
        # shows the file was read to its end.
        r del ia:aof ia:after
        r gcreate ia:aof ALPHA 0.2 DELTA 0.2 SCORE
        set waiting [lindex [r gpredict ia:aof SCORE 0.5] 1]
        # (everything written so far is in the file)
        r config set appendfsync always
        r set ia:flushed 1
        set digest [debug_digest_value ia:aof]
        set before [gcmd_mismatches]
        set fp [open [get_last_incr_aof_path r] a]
        fconfigure $fp -translation binary
        puts -nonewline $fp [gcmd_aof_format _GATE.APPLY ia:aof]
        puts -nonewline $fp [gcmd_aof_format _GATE.APPLY]
        puts -nonewline $fp [gcmd_aof_format SET ia:after 1]
        close $fp
        assert_equal OK [r debug loadaof]
        assert_equal 1 [r get ia:after]
        assert_equal [expr {$before + 2}] [gcmd_mismatches]
        # the gate is what the file made of it before those lines
        assert_equal gate-type [r type ia:aof]
        assert_equal $digest [debug_digest_value ia:aof]
        # ... a loaded gate: it starts over at its first write
        assert_equal 0 [r glearn ia:aof $waiting CORRECT 1]
        lassign [r gpredict ia:aof SCORE 0.5] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [r glearn ia:aof $id CORRECT 1]
        assert_equal [expr {$before + 2}] [gcmd_mismatches]
        r config set appendfsync everysec
        r del ia:aof ia:after ia:flushed
        assert_equal PONG [r ping]
    }
}
