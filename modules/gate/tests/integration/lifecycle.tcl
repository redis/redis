# Gate data type: what a gate does over its life, through its commands only.
#
# Every test states the contract of modules/gate/SPEC.md. Where a number is
# asserted exactly, the comment next to it says which sentence of the
# specification it follows from. Three rules are stated more exactly in the
# file comments of gate_core.h and gate_stats.h than in SPEC.md, and the
# tests that lean on them say so: how many answered requests may wait
# behind a late one, when a block of spot checks is judged by the drift
# detector, and which of the candidate's requests the answering snapshot is
# compared on.
#
# Run from the repository root:
#
#   ./runtest --single ../modules/gate/tests/integration/lifecycle
#
# DETERMINISM. Every test that depends on a random draw starts with lc_seed:
# with gate.test-seed set, the draws and ids of the module come from a seeded
# sequence. The requests, labels and noise of the tests come from lc_rand, a
# generator of their own. A test therefore sees the same requests, the same
# draws and the same replies on every run. No assertion names the outcome of
# one particular draw: they hold for any draw, or (shares over thousands of
# requests) for nearly every sequence of draws.
#
# TIME. A gate reads the clock, and it does its bookkeeping only when it is
# written to. The tests about late and lost answers therefore create their
# gates with a short gate.feedback-timeout (lc_gcreate_timeout) and sleep.
# A sleep lasts at least as long as it says, so "this request is too old
# now" is always true. "This happened in time" is not, on a machine that is
# busy with something else: where an assertion needs the test to have been
# fast, the timeout is some hundred times what the commands take, or the
# test measures its own time first and skips the assertion if it was slow.
# (The test and the gate do not read the same clock, and both count whole
# milliseconds: such a measurement is good to a few milliseconds, and is
# compared with that much to spare.)

set gatemodule [file normalize modules/gate/gate.so]

# ------------------------------- Helpers ----------------------------------

# Restart the seeded sequence of the module. Redis does not call a module's
# setter when CONFIG SET repeats the value a setting already has, so
# "CONFIG SET gate.test-seed 7" alone restarts nothing if the seed was 7
# already: the seed is cleared first.
proc lc_seed {seed} {
    r config set gate.test-seed 0
    r config set gate.test-seed $seed
}

# Randomness of the test itself: a small generator with a state of its own,
# so that the stream of requests does not depend on the Tcl version.
proc lc_srand {seed} {
    set ::lc_rng $seed
    set ::lc_seen [dict create]
}

proc lc_rand {n} {
    set ::lc_rng [expr {($::lc_rng * 1103515245 + 12345) & 0x7fffffff}]
    return [expr {($::lc_rng >> 8) % $n}]
}

proc lc_pick {list} {
    return [lindex $list [lc_rand [llength $list]]]
}

# One field of GINFO. The reply is a map in RESP3 and a flat field/value
# array in RESP2: Tcl reads both as a dictionary.
proc lc_get {key field} {
    return [dict get [r ginfo $key] $field]
}

# The threshold of a score gate ({} while it does not answer).
proc lc_threshold {key} {
    return [lindex [lc_get $key thresholds] 0]
}

# SPEC section 2, GINFO: the request counters.
proc lc_assert_counters {key} {
    set i [r ginfo $key]
    set sum [expr {[dict get $i accepted] + [dict get $i audited] + [dict get $i abstained]}]
    assert_equal $sum [dict get $i requests] "requests of $key = accepted + audited + abstained"
}

# 1 if every label the gate may answer has a threshold.
proc lc_all_proven {key} {
    set i [r ginfo $key]
    foreach alpha [dict get $i alphas] threshold [dict get $i thresholds] {
        if {$alpha != 0 && $threshold eq {}} {return 0}
    }
    return 1
}

# GCREATE with gate.feedback-timeout set to 'timeout' milliseconds. The
# setting is read when a gate is created, so it holds for this gate only:
# the default (SPEC section 5: one minute) is back for the next one.
proc lc_gcreate_timeout {timeout key args} {
    r config set gate.feedback-timeout $timeout
    set reply [r gcreate $key {*}$args]
    r config set gate.feedback-timeout 60000
    return $reply
}

# ------------------------------ Score gates -------------------------------

# One request to a score gate, answered at once if the gate wants feedback.
# Returns the action.
proc lc_score {key score correct} {
    lassign [r gpredict $key SCORE $score] action id
    if {$id ne {}} {r glearn $key $id CORRECT $correct}
    return $action
}

# Requests at 'score' on which the cheap result is right, until the gate is
# ACTIVE. Returns how many it took.
proc lc_calibrate {key {score 0.9} {max 600}} {
    set n 0
    while {[lc_get $key state] ne "ACTIVE"} {
        if {$n >= $max} {fail "gate $key is not ACTIVE after $max correct answers: [r ginfo $key]"}
        incr n
        lassign [r gpredict $key SCORE $score] action id
        if {$id ne {}} {r glearn $key $id CORRECT 1}
    }
    return $n
}

# A score gate proven on a stream where the cheap result is right at 0.9
# and wrong at 0.3. It ends ACTIVE with its threshold between the two.
proc lc_gate_between {key args} {
    r gcreate $key ALPHA 0.2 DELTA 0.2 {*}$args SCORE
    for {set n 0} {$n < 300} {incr n} {
        lc_score $key 0.9 1
        if {[lc_get $key state] eq "ACTIVE"} return
        lc_score $key 0.3 0
    }
    fail "gate $key is not ACTIVE: [r ginfo $key]"
}

# A score gate that answers every score and spot-checks every request:
# with AUDIT 1 every request a gate could answer is a spot check, so the
# test decides which spot checks there are and no dice are involved. It is
# proven by correct answers at 0.9 and has had no spot check yet.
proc lc_gate_spot {key {timeout 60000} {alpha 0.2}} {
    lc_gcreate_timeout $timeout $key ALPHA $alpha DELTA 0.2 AUDIT 1 SCORE
    lc_calibrate $key
    if {[lc_threshold $key] != 0} {fail "gate $key does not answer every score: [r ginfo $key]"}
}

# One spot check of such a gate. Returns its id.
proc lc_spot {key} {
    lassign [r gpredict $key SCORE 0.9] action id
    if {$action ne "AUDIT"} {fail "gate $key replied $action where it answers: [r ginfo $key]"}
    return $id
}

# A score gate that has stopped answering because the answers of its spot
# checks stopped coming (SPEC section 3, "Getting worse": a spot check
# whose answer is lost may hide a disagreement, and when so many are lost
# that the gate can no longer rule that out, it stops answering and keeps
# its model). Its first 20 spot checks were left without an answer until
# they were too old: the 20th lost answer of a block in which nothing else
# was asked is the one that does it at ALPHA 0.2 (see the drift detector
# below). Returns the id given to the request that found the gate stopped;
# ::lc_outage_found is the time just before that request.
proc lc_outage {key} {
    lc_gate_spot $key 300
    for {set k 0} {$k < 20} {incr k} {lc_spot $key}
    after 350
    set ::lc_outage_found [clock milliseconds]
    lassign [r gpredict $key SCORE 0.9] action id
    if {$action ne "ABSTAIN" || $id eq {}} {
        fail "gate $key replied '$action' '$id' after 20 lost spot checks: [r ginfo $key]"
    }
    return $id
}

# SPEC section 3 (Proof, Budget) and section 5 (betting test, proven): the
# number of correct answers in a row, after 'errors' wrong ones, that prove
# a threshold for snapshot number 'generation'.
#
# A threshold is tested by betting. The wealth starts at 1; a bet stakes a
# fraction c of the largest stake that cannot go broke, 1 / (1 - alpha), so
# a correct answer multiplies its wealth by 1 + c * alpha / (1 - alpha) and
# a wrong one by 1 - c; the four fractions carry equal weight. The threshold
# is proven once the wealth reaches 1 / share, where the share is what the
# snapshot may spend, DELTA / (g (g + 1)), split evenly over the thresholds
# it tests. Every threshold is a test of its own: 32 for each of the
# 'labels' the gate may answer, and one label for a score gate.
proc lc_proof_needed {alpha delta generation {errors 0} {labels 1}} {
    set bar [expr {32.0 * $labels * $generation * ($generation + 1) / $delta}]
    for {set n 1} {$n < 100000} {incr n} {
        set wealth 0.0
        foreach c {0.1 0.25 0.5 0.9} {
            set wealth [expr {$wealth + 0.25 * pow(1 - $c, $errors) *
                              pow(1 + $c * $alpha / (1 - $alpha), $n)}]
        }
        if {$wealth >= $bar} {return $n}
    }
    error "lc_proof_needed: no proof within 100000 answers"
}

# SPEC section 5, snapshot dropped: a classifier snapshot that has not
# taken over is dropped after 10 times the proof a flawless threshold
# needs, per label the gate may answer. The proof a flawless threshold
# needs is lc_proof_needed without a wrong answer. (780 pieces of proof for
# the first snapshot of two labels at ALPHA 0.2 and DELTA 0.2, 880 for the
# second.)
proc lc_snapshot_budget {alpha delta generation labels} {
    return [expr {10 * $labels * [lc_proof_needed $alpha $delta $generation 0 $labels]}]
}

# ---------------- The drift detector, written down a second time ----------
#
# SPEC section 5, drift detector: likelihood ratios for 2x and 4x the error
# rate (at most halfway to 1), averaged, over blocks of 64, 512 and 4,096
# spot checks in the order they were asked; a spot check counts as right
# until its answer says otherwise; alarm at 1,000, and at 10^9 with lost
# answers counted as disagreements.
#
# gate_stats.h says what that leaves open. A spot check gets its number
# when it is asked, and the first one of a block opens it. A block is judged
# whenever one of its spot checks turns out wrong or lost, over the spot
# checks asked in it so far: the right ones (and the ones not answered yet)
# multiply a ratio by (1 - p) / (1 - alpha), the wrong ones by p / alpha,
# for p = 2 alpha and p = 4 alpha. A spot check whose block has ended
# counts only in the longer blocks that are still open. Of the two alarms
# the first ("got worse") wins over the second ("can no longer be
# checked").
#
# A detector is a dictionary. A gate has one for every label, and starts a
# new one with every snapshot that takes over.

proc lc_drift_new {alpha} {
    set right {}
    set wrong {}
    foreach times {2 4} {
        set p [expr {min($times * $alpha, (1 + $alpha) / 2.0)}]
        lappend right [expr {log((1 - $p) / (1 - $alpha))}]
        lappend wrong [expr {log($p / $alpha)}]
    }
    return [dict create asked 0 wrong {0 0 0} lost {0 0 0} right_step $right wrong_step $wrong]
}

# A spot check is asked. Returns its number.
proc lc_drift_ask {name} {
    upvar 1 $name d
    set asked [dict get $d asked]
    set wrong {}
    set lost {}
    foreach length {64 512 4096} w [dict get $d wrong] l [dict get $d lost] {
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

# The logarithm of what a block is worth: 'against' of the 'open' spot
# checks asked in it so far count against the gate.
proc lc_drift_level {d open against} {
    set logs {}
    foreach right [dict get $d right_step] wrong [dict get $d wrong_step] {
        lappend logs [expr {$against * $wrong + ($open - $against) * $right}]
    }
    set top [expr {max([lindex $logs 0], [lindex $logs 1])}]
    set sum 0.0
    foreach l $logs {set sum [expr {$sum + 0.5 * exp($l - $top)}]}
    return [expr {$top + log($sum)}]
}

# The answer of spot check 'number' disagrees ('lost' 0), or is given up
# ('lost' 1). Returns "worse", "unchecked", or nothing.
proc lc_drift_report {name number lost} {
    upvar 1 $name d
    set asked [dict get $d asked]
    set verdict ""
    set wrongs {}
    set losts {}
    foreach length {64 512 4096} w [dict get $d wrong] l [dict get $d lost] {
        if {($number - 1) / $length == ($asked - 1) / $length} {
            if {$lost} {incr l} else {incr w}
            set open [expr {($asked - 1) % $length + 1}]
            if {!$lost && [lc_drift_level $d $open $w] >= log(1000)} {
                set verdict worse
            }
            if {$verdict ne "worse" && [lc_drift_level $d $open [expr {$w + $l}]] >= log(1e9)} {
                set verdict unchecked
            }
        }
        lappend wrongs $w
        lappend losts $l
    }
    dict set d wrong $wrongs
    dict set d lost $losts
    return $verdict
}

proc lc_drift_wrong {name number} {
    upvar 1 $name d
    return [lc_drift_report d $number 0]
}

proc lc_drift_lost {name number} {
    upvar 1 $name d
    return [lc_drift_report d $number 1]
}

# How many disagreeing spot checks in a row, each answered before the next
# is asked, raise the alarm of a detector that has seen 'right' agreeing
# ones and nothing else.
proc lc_drift_wrong_needed {alpha right} {
    set d [lc_drift_new $alpha]
    for {set n 0} {$n < $right} {incr n} {lc_drift_ask d}
    for {set n 1} {$n < 100000} {incr n} {
        if {[lc_drift_wrong d [lc_drift_ask d]] eq "worse"} {return $n}
    }
    error "lc_drift_wrong_needed: no alarm"
}

# ----------------- A score gate, written down a second time ---------------
#
# What SPEC sections 3 and 5 say about the first snapshot of a score gate
# created with AUDIT 1, in a few lines of Tcl. With AUDIT 1 every request
# the gate could answer is a spot check, so every request is proof and the
# model needs no dice. Time is counted in rounds: the test that uses the
# model (in the feedback block) leaves more than a quarter of the feedback
# timeout between two rounds and less than that inside one, so a request of
# an earlier round is late and one of this round is not.
#
#   thresholds   32, evenly spaced in log(1 - threshold) from 0 to 0.998
#   proof        counted in request order; an answer waits for the answers
#                of older requests, but not for one that is late: that
#                request counts as a wrong guess until its answer arrives
#   betting test as in lc_proof_needed, one for every threshold
#   proven       wealth 32 x 1 x 2 / DELTA; the gate answers at the lowest
#                proven threshold
#   spot checks  a request the gate could have answered gets its number in
#                the drift detector when it is asked, and its answer reaches
#                the detector the moment it arrives (lc_drift_*): the gate
#                starts over when the detector says "worse"
#   given up     a request whose answer has not come after the feedback
#                timeout stays a wrong guess and is missing; a spot check
#                among them is a lost answer for the drift detector, and 32
#                of them in a row, none of which has seen an answer of any
#                request arrive since it was asked, stop the gate as well

proc lc_model_init {alpha delta} {
    upvar #0 ::lc_m m
    array unset m
    array set m {policy -1 samples 0 audits 0 audit_errors 0 waiting 0 unanswered 0 next 1
                 missing 0 run 0 mark 1}
    set m(queue) {}
    set m(open) [dict create]
    for {set t 0} {$t < 32} {incr t} {
        set m(grid,$t) [expr {$t == 31 ? 0.998 : 1 - pow(0.002, $t / 31.0)}]
        set m(proven,$t) 0
        set m(wealth,$t) {0.0 0.0 0.0 0.0}
    }
    set m(right) {}
    set m(wrong) {}
    foreach c {0.1 0.25 0.5 0.9} {
        lappend m(right) [expr {log(1 + $c * $alpha / (1 - $alpha))}]
        lappend m(wrong) [expr {log(1 - $c)}]
    }
    set m(bar) [expr {log(32.0 * 1 * 2 / $delta)}]
    set m(drift) [lc_drift_new $alpha]
}

# Add 'steps' to the logarithms of the four bets of one threshold.
proc lc_model_bet {t steps {sign 1}} {
    upvar #0 ::lc_m m
    set wealth {}
    foreach w $m(wealth,$t) step $steps {lappend wealth [expr {$w + $sign * $step}]}
    set m(wealth,$t) $wealth
}

# The thresholds a request is proof for have changed for the better: see
# which of them are proven now. The gate answers at the lowest proven one.
proc lc_model_prove {seq} {
    upvar #0 ::lc_m m
    for {set t 0} {$t < 32 && $m(grid,$t) <= $m(score,$seq)} {incr t} {
        set sum 0.0
        foreach w $m(wealth,$t) {set sum [expr {$sum + 0.25 * exp($w)}]}
        if {log($sum) >= $m(bar)} {set m(proven,$t) 1}
    }
    for {set t 31} {$t >= 0} {incr t -1} {
        if {$m(proven,$t)} {set m(policy) $t}
    }
}

# Count one request as proof, as a right or as a wrong guess.
proc lc_model_count {seq wrong} {
    upvar #0 ::lc_m m
    incr m(samples)
    for {set t 0} {$t < 32 && $m(grid,$t) <= $m(score,$seq)} {incr t} {
        lc_model_bet $t [expr {$wrong ? $m(wrong) : $m(right)}]
    }
    if {!$wrong} {lc_model_prove $seq}
}

# What every write command does first: count the answered requests in
# request order, and move on without an answer that is late and in the way
# of answered ones.
proc lc_model_settle {round} {
    upvar #0 ::lc_m m
    while {[llength $m(queue)]} {
        set head [lindex $m(queue) 0]
        if {$m(answered,$head)} {
            incr m(waiting) -1
            lc_model_count $head [expr {!$m(correct,$head)}]
        } elseif {$m(waiting) > 0 && $m(round,$head) < $round} {
            set m(moved_on,$head) 1
            lc_model_count $head 1
        } else {
            break
        }
        set m(queue) [lrange $m(queue) 1 end]
    }
}

# A request. Returns the action of the reply and the model's number for the
# request.
proc lc_model_predict {score round} {
    upvar #0 ::lc_m m
    lc_model_settle $round
    set seq $m(next)
    incr m(next)
    array set m [list score,$seq $score round,$seq $round answered,$seq 0 moved_on,$seq 0 spot,$seq 0]
    if {$m(policy) >= 0 && $score >= $m(grid,$m(policy))} {
        set d $m(drift)
        set m(spot,$seq) [lc_drift_ask d]
        set m(drift) $d
    }
    lappend m(queue) $seq
    dict set m(open) $seq 1
    incr m(unanswered)
    return [list [expr {$m(spot,$seq) ? "AUDIT" : "ABSTAIN"}] $seq]
}

# The answer of a request. Returns what the drift detector makes of it:
# "worse", "unchecked", or nothing.
proc lc_model_learn {seq correct round} {
    upvar #0 ::lc_m m
    lc_model_settle $round
    set m(answered,$seq) 1
    set m(correct,$seq) $correct
    incr m(unanswered) -1
    dict unset m(open) $seq
    # An answer has arrived: the requests asked so far are no outage.
    set m(mark) $m(next)
    set m(run) 0
    if {$m(spot,$seq)} {
        incr m(audits)
        if {!$correct} {
            incr m(audit_errors)
            set d $m(drift)
            set verdict [lc_drift_wrong d $m(spot,$seq)]
            set m(drift) $d
            if {$verdict ne ""} {return $verdict}
        }
    }
    if {!$m(moved_on,$seq)} {
        incr m(waiting)
    } elseif {$correct} {
        # Proof had moved on without this answer: the wrong guess it was
        # counted as is taken back.
        for {set t 0} {$t < 32 && $m(grid,$t) <= $m(score,$seq)} {incr t} {
            lc_model_bet $t $m(right)
            lc_model_bet $t $m(wrong) -1
        }
        lc_model_prove $seq
    }
    lc_model_settle $round
    return ""
}

# The feedback timeout has passed for every request that is still without
# its answer, and the gate is written to: they are given up, oldest first.
# Each stays a wrong guess (if proof had not moved on without it, it is
# counted now, in its turn) and is missing. Returns what stops the gate, if
# anything does: "blind" (32 in a row and no answer since the first of them
# was asked) or "unchecked" (the drift detector, of lost spot checks).
proc lc_model_give_up {} {
    upvar #0 ::lc_m m
    foreach seq [dict keys $m(open)] {
        if {!$m(moved_on,$seq)} {
            while {[lindex $m(queue) 0] != $seq} {
                set head [lindex $m(queue) 0]
                incr m(waiting) -1
                lc_model_count $head [expr {!$m(correct,$head)}]
                set m(queue) [lrange $m(queue) 1 end]
            }
            lc_model_count $seq 1
            set m(queue) [lrange $m(queue) 1 end]
        }
        dict unset m(open) $seq
        incr m(missing)
        if {$seq >= $m(mark) && [incr m(run)] >= 32} {return blind}
        if {$m(spot,$seq)} {
            set d $m(drift)
            set verdict [lc_drift_lost d $m(spot,$seq)]
            set m(drift) $d
            if {$verdict ne ""} {return $verdict}
        }
    }
    # The answers that waited behind them.
    foreach head $m(queue) {
        incr m(waiting) -1
        lc_model_count $head [expr {!$m(correct,$head)}]
    }
    set m(queue) {}
    return ""
}

# Compare what GINFO shows with the model.
proc lc_model_check {key what} {
    upvar #0 ::lc_m m
    set i [r ginfo $key]
    set want [expr {$m(policy) < 0 ? {} : $m(grid,$m(policy))}]
    set got [lindex [dict get $i thresholds] 0]
    if {($got eq {}) != ($want eq {}) || ($got ne {} && abs($got - $want) > 1e-9)} {
        fail "$what: the gate answers from '$got', SPEC sections 3 and 5 say from '$want': $i"
    }
    assert_equal [dict get $i state] [expr {$m(policy) < 0 ? "CALIBRATING" : "ACTIVE"}] "$what: state"
    assert_equal [dict get $i calibration-samples] $m(samples) "$what: calibration-samples"
    assert_equal [dict get $i audit-samples] $m(audits) "$what: audit-samples"
    assert_equal [dict get $i audit-errors] $m(audit_errors) "$what: audit-errors"
    # A request without an answer is pending until it is too old, and
    # missing from then on.
    assert_equal [expr {[dict get $i pending] + [dict get $i missing]}] $m(unanswered) \
        "$what: pending + missing"
    assert_morethan_equal [dict get $i missing] $m(missing) "$what: missing"
    assert_equal [dict get $i revocations] 0 "$what: revocations"
}

# ---------------------------- Classifier gates ----------------------------

# A learnable problem: short messages in which one word decides the label.
set ::lc_signal(respond) {help question urgent broken please error}
set ::lc_signal(ignore) {thanks awesome lunch weekend cheers congrats}
set ::lc_signal(escalate) {outage breach lawsuit emergency critical offline}
set ::lc_fillers {the a team everyone today now here there again really just
                  so well and but maybe quite also still soon}

# A message with that label: a signal word between two filler words, in any
# rotation. Remembered in ::lc_seen.
proc lc_message {label} {
    set words [list [lc_pick $::lc_fillers] [lc_pick $::lc_signal($label)] [lc_pick $::lc_fillers]]
    set k [lc_rand 3]
    set text [join [concat [lrange $words $k end] [lrange $words 0 [expr {$k - 1}]]] " "]
    dict set ::lc_seen $text 1
    return $text
}

# A message about one of many topics: two words of its own between two
# filler words.
proc lc_topic_message {topic} {
    return "[lc_pick $::lc_fillers] alpha${topic}word beta${topic}term [lc_pick $::lc_fillers]"
}

# The teacher: the expensive call a gate stands in front of.
set ::lc_teacher_calls 0
proc lc_teacher {text} {
    incr ::lc_teacher_calls
    set words [split $text " "]
    foreach label {respond ignore escalate} {
        foreach word $::lc_signal($label) {
            if {[lsearch -exact $words $word] >= 0} {return $label}
        }
    }
    error "lc_teacher: no signal word in '$text'"
}

# A teacher that says the other label one time in twenty.
proc lc_noisy {label} {
    if {[lc_rand 100] < 5} {return [lc_other $label]}
    return $label
}

proc lc_other {label} {
    return [expr {$label eq "ignore" ? "respond" : "ignore"}]
}

# The client loop of SPEC section 2, line by line.
proc lc_decide {key text} {
    lassign [r gpredict $key TEXT $text] action arg
    if {$action eq "ACCEPT"} {
        return $arg
    }
    set label [lc_teacher $text]                 ;# ABSTAIN and AUDIT
    if {$arg ne {}} {
        r glearn $key $arg LABEL $label
    }
    return $label
}

# 'count' labelled examples from history.
proc lc_history {key labels count} {
    for {set k 0} {$k < $count} {incr k} {
        set label [lc_pick $labels]
        r gadd $key TEXT [lc_message $label] LABEL $label
    }
}

# A classifier gate taught 'history' examples, with its first snapshot taken
# right after them, and then used through the client loop until every label
# it may answer has a threshold. 'args' are further GCREATE options.
proc lc_trained_gate {key labels history max args} {
    r config set gate.snapshot-examples $history
    r gcreate $key ALPHA 0.2 DELTA 0.2 CLASSES [llength $labels] {*}$labels {*}$args
    lc_history $key $labels $history
    for {set n 1} {$n <= $max} {incr n} {
        lc_decide $key [lc_message [lc_pick $labels]]
        if {$n % 10 == 0 && [lc_all_proven $key]} {return $n}
    }
    fail "gate $key: a label is still without a threshold after $max requests: [r ginfo $key]"
}

# The simplest problem there is: every label has one message of its own.
set ::lc_one(ignore) thanks
set ::lc_one(respond) help
set ::lc_one(escalate) outage

# A classifier gate whose first snapshot is never wrong on that problem:
# each message is taught 20 times, in turns, and the snapshot is taken
# right after that. Where a test counts the correct answers a proof needs,
# this is the gate it counts on: every answer to "thanks" is one correct
# answer for "ignore", and nothing else. 'args' are further GCREATE options.
proc lc_exact_gate {key labels args} {
    r config set gate.snapshot-examples [expr {20 * [llength $labels]}]
    r gcreate $key ALPHA 0.2 DELTA 0.2 CLASSES [llength $labels] {*}$labels {*}$args
    for {set k 0} {$k < 20} {incr k} {
        foreach label $labels {
            r gadd $key TEXT $::lc_one($label) LABEL $label
        }
    }
}

# One request with the message of 'label', answered with 'answer' (the
# label itself unless given). The gate must have wanted the answer. Returns
# the action.
proc lc_exact_ask {key label {answer {}}} {
    if {$answer eq {}} {set answer $label}
    lassign [r gpredict $key TEXT $::lc_one($label)] action id
    if {$action eq "ACCEPT" || $id eq {}} {
        fail "gate $key replied '$action' '$id' to the message of $label: [r ginfo $key]"
    }
    assert_equal [r glearn $key $id LABEL $answer] 1
    return $action
}

# ============================== Score gates ===============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a score gate starts its first test at GCREATE} {
        lc_seed 1
        assert_equal [r gcreate s:new ALPHA 0.2 DELTA 0.2 SCORE] OK
        assert_equal [r type s:new] gate-type

        set i [r ginfo s:new]
        assert_equal [dict get $i kind] score
        assert_equal [dict get $i state] CALIBRATING
        assert {[dict get $i alpha] == 0.2}
        assert {[dict get $i delta] == 0.2}
        assert {[dict get $i audit] == 0.05}
        assert_equal [llength [dict get $i classes]] 0
        assert_equal [llength [dict get $i alphas]] 1
        assert {[lindex [dict get $i alphas] 0] == 0.2}
        assert_equal [llength [dict get $i thresholds]] 1
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 1
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal [dict get $i $field] 0 $field
        }
        assert {[dict get $i coverage] == 0}
        assert_morethan [dict get $i memory] 0
    }

    test {gate lifecycle: before it is ACTIVE a score gate abstains with an id on every request} {
        lc_seed 2
        r gcreate s:warm ALPHA 0.2 DELTA 0.2 SCORE

        # The cheap result is right at 0.9 and wrong at 0.3. Whatever the
        # second stream does, the thresholds above 0.3 see correct answers
        # only, and SPEC section 3 says how many they need.
        set need [lc_proof_needed 0.2 0.2 1]
        assert_equal $need 35
        set seen [dict create]
        for {set n 1} {$n <= $need} {incr n} {
            foreach {score correct} {0.9 1 0.3 0} {
                if {$n == $need && $score == 0.3} break
                assert_equal [lc_get s:warm state] CALIBRATING "before answer $n at $score"
                lassign [r gpredict s:warm SCORE $score] action id
                assert_equal $action ABSTAIN
                assert {$id ne {}}
                assert {![dict exists $seen $id]}
                dict set seen $id 1
                assert_equal [r glearn s:warm $id CORRECT $correct] 1
            }
        }

        # Nothing was wasted while warming up: every answer counted as proof.
        set i [r ginfo s:warm]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i calibration-samples] [expr {2 * $need - 1}]
        assert_equal [dict get $i requests] [expr {2 * $need - 1}]
        assert_equal [dict get $i abstained] [expr {2 * $need - 1}]
        assert_equal [dict get $i accepted] 0
        assert_equal [dict get $i audited] 0
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i missing] 0
    }

    test {gate lifecycle: the proof a score gate needs is the betting test of SPEC section 3} {
        # Test settings, the same after one and two wrong answers, another
        # pair of settings, and the defaults with a realistic error rate.
        foreach {options alpha delta errors} {
            {ALPHA 0.2 DELTA 0.2}  0.2  0.2  0
            {ALPHA 0.2 DELTA 0.2}  0.2  0.2  1
            {ALPHA 0.2 DELTA 0.2}  0.2  0.2  2
            {DELTA 0.5 ALPHA 0.3}  0.3  0.5  0
            {ALPHA 0.05}           0.05 0.01 0
        } {
            lc_seed 3
            r del s:bet
            r gcreate s:bet {*}$options SCORE
            for {set k 0} {$k < $errors} {incr k} {
                lassign [r gpredict s:bet SCORE 0.9] action id
                assert_equal [r glearn s:bet $id CORRECT 0] 1
            }
            set need [lc_proof_needed $alpha $delta 1 $errors]
            for {set n 1} {$n <= $need} {incr n} {
                assert_equal [lc_get s:bet state] CALIBRATING \
                    "$options, $errors wrong: after [expr {$n - 1}] of $need correct answers"
                lassign [r gpredict s:bet SCORE 0.9] action id
                assert_equal $action ABSTAIN
                assert_equal [r glearn s:bet $id CORRECT 1] 1
            }
            assert_equal [lc_get s:bet state] ACTIVE "$options, $errors wrong: after $need correct answers"
            assert_equal [lc_get s:bet calibration-samples] [expr {$errors + $need}]
        }
    }

    test {gate lifecycle: an ACTIVE score gate answers from its threshold up and abstains below it} {
        lc_seed 4
        lc_gate_between s:thr
        set i [r ginfo s:thr]
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 0

        # Half of the answers at or above any score up to 0.3 were wrong, and
        # nothing above 0.9 was ever seen: the lowest proven threshold lies
        # between the two. SPEC section 5 says where exactly: the 32
        # thresholds are evenly spaced in log(1 - threshold) from 0 to
        # 0.998, and the lowest one above 0.3 is the third.
        set thr [lindex [dict get $i thresholds] 0]
        assert {$thr > 0.3 && $thr <= 0.9}
        assert {abs($thr - (1 - pow(0.002, 2 / 31.0))) < 1e-9}
        set asked [dict get $i requests]

        # The threshold is the lowest score the gate answers at.
        foreach score [list $thr 0.34 0.5 0.9 1] {
            for {set k 0} {$k < 20} {incr k} {
                lassign [r gpredict s:thr SCORE $score] action id
                if {$action eq "ACCEPT"} {
                    assert_equal $id {}
                } else {
                    assert_equal $action AUDIT "score $score"
                    assert {$id ne {}}
                    assert_equal [r glearn s:thr $id CORRECT 1] 1
                }
            }
        }
        foreach score {0.33 0.3 0.1 0} {
            for {set k 0} {$k < 25} {incr k} {
                lassign [r gpredict s:thr SCORE $score] action id
                assert_equal $action ABSTAIN "score $score"
                if {$id ne {}} {assert_equal [r glearn s:thr $id CORRECT 0] 1}
            }
        }

        # 100 requests the gate could answer and 100 it could not.
        set i [r ginfo s:thr]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [lindex [dict get $i thresholds] 0] $thr
        assert_equal [dict get $i requests] [expr {$asked + 200}]
        assert_equal [expr {[dict get $i accepted] + [dict get $i audited]}] 100
        assert_equal [dict get $i abstained] [expr {$asked + 100}]
        assert_morethan [dict get $i accepted] 0
        assert_range [dict get $i coverage] 0.45 0.55
        assert_equal [dict get $i pending] 0
        lc_assert_counters s:thr
    }

    test {gate lifecycle: ACCEPT gives no id and needs no feedback} {
        lc_seed 5
        r gcreate s:acc ALPHA 0.2 DELTA 0.2 SCORE
        lc_calibrate s:acc
        set accepts 0
        for {set k 0} {$k < 400 && $accepts < 100} {incr k} {
            set before [r ginfo s:acc]
            lassign [r gpredict s:acc SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr accepts
                assert_equal $id {}
                # Nothing waits for an answer and nothing was added to the
                # proof: the gate simply answered.
                set after [r ginfo s:acc]
                assert_equal [dict get $after pending] [dict get $before pending]
                assert_equal [dict get $after calibration-samples] [dict get $before calibration-samples]
                assert_equal [dict get $after accepted] [expr {[dict get $before accepted] + 1}]
                assert_equal [dict get $after requests] [expr {[dict get $before requests] + 1}]
            } else {
                assert_equal $action AUDIT
                assert_equal [r glearn s:acc $id CORRECT 1] 1
            }
        }
        assert_equal $accepts 100
        assert_equal [lc_get s:acc state] ACTIVE
        assert_equal [lc_get s:acc pending] 0
        lc_assert_counters s:acc
    }

    test {gate lifecycle: with AUDIT 1 a gate never ACCEPTs, it replies AUDIT} {
        lc_seed 6
        lc_gate_between s:audit1 AUDIT 1
        assert {[lc_get s:audit1 audit] == 1}
        for {set k 0} {$k < 50} {incr k} {
            lassign [r gpredict s:audit1 SCORE 0.9] action id
            assert_equal $action AUDIT
            assert {$id ne {}}
            assert_equal [r glearn s:audit1 $id CORRECT 1] 1
            assert_equal [lc_score s:audit1 0.3 0] ABSTAIN
        }
        set i [r ginfo s:audit1]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i accepted] 0
        assert_equal [dict get $i audited] 50
        assert_equal [dict get $i audit-samples] 50
        assert_equal [dict get $i audit-errors] 0
        assert_equal [dict get $i pending] 0
        lc_assert_counters s:audit1
    }

    test {gate lifecycle: about AUDIT of the requests a gate could answer are spot checks} {
        lc_seed 7
        r gcreate s:share ALPHA 0.2 DELTA 0.2 AUDIT 0.1 SCORE
        lc_calibrate s:share
        set asked [lc_get s:share requests]
        set accepts 0
        set audits 0
        for {set k 0} {$k < 3000} {incr k} {
            lassign [r gpredict s:share SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr accepts
                assert_equal $id {}
            } elseif {$action eq "AUDIT"} {
                incr audits
                assert_equal [r glearn s:share $id CORRECT 1] 1
            } else {
                fail "request $k: $action from a gate that answers every score"
            }
        }
        # 3000 draws at 0.1: 300 expected, with a standard deviation of 16.
        assert_range $audits 235 365
        set i [r ginfo s:share]
        assert_equal [dict get $i accepted] $accepts
        assert_equal [dict get $i audited] $audits
        assert_equal [dict get $i audit-samples] $audits
        assert_equal [dict get $i audit-errors] 0
        assert_equal [dict get $i abstained] $asked
        assert {[dict get $i coverage] > 0.999}
        assert_equal [dict get $i pending] 0
        lc_assert_counters s:share
    }

    test {gate lifecycle: below the threshold an ABSTAIN has an id only when it was drawn as proof} {
        lc_seed 8
        lc_gate_between s:low AUDIT 0.25
        set thr [lc_threshold s:low]
        set with 0
        set without 0
        for {set k 0} {$k < 400} {incr k} {
            # The traffic keeps its shape: right at 0.9, wrong at 0.3.
            set action [lc_score s:low 0.9 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            lassign [r gpredict s:low SCORE 0.3] action id
            assert_equal $action ABSTAIN
            if {$id eq {}} {
                incr without
            } else {
                incr with
                assert_equal [r glearn s:low $id CORRECT 0] 1
            }
        }
        # 400 draws at 0.25: 100 expected, with a standard deviation of 9.
        assert_range $with 65 135
        assert_equal [expr {$with + $without}] 400
        # The lower thresholds are still wrong half of the time: none of
        # them was proven, so the gate answers where it answered before.
        assert_equal [lc_threshold s:low] $thr
        assert_equal [lc_get s:low state] ACTIVE
        assert_equal [lc_get s:low pending] 0
        lc_assert_counters s:low
    }

    test {gate lifecycle: a score gate keeps testing lower thresholds, its threshold only moves down} {
        lc_seed 9
        r gcreate s:down ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        # Two early requests at 0.5 on which the cheap result is wrong, then
        # a cheap result that is right at 0.9.
        for {set k 0} {$k < 2} {incr k} {
            lassign [r gpredict s:down SCORE 0.5] action id
            assert_equal [r glearn s:down $id CORRECT 0] 1
        }
        lc_calibrate s:down
        set first [lc_threshold s:down]
        assert {$first > 0.5 && $first <= 0.9}
        assert_equal [lc_score s:down 0.5 1] ABSTAIN

        # From now on the cheap result is right at 0.5. The thresholds at
        # and below 0.5 are tested on the requests drawn as proof, and
        # after enough of them the gate answers there too.
        set last $first
        for {set k 0} {$k < 600 && $last > 0.5} {incr k} {
            lc_score s:down 0.5 1
            set thr [lc_threshold s:down]
            assert {$thr <= $last}
            set last $thr
        }
        assert {$last <= 0.5}
        for {set k 0} {$k < 20} {incr k} {
            set action [lc_score s:down 0.5 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        }
        # The same snapshot all along.
        assert_equal [lc_get s:down state] ACTIVE
        assert_equal [lc_get s:down generation] 1
        assert_equal [lc_get s:down revocations] 0
        assert_morethan [lc_get s:down coverage] 0
        lc_assert_counters s:down
    }

    test {gate lifecycle: a score gate answers when the cheap result is right often enough, and only then} {
        # Two caches with the same traffic, scores spread over the whole
        # range. The cheap result of the first is wrong one time in twenty
        # whatever the score. The second is wrong three times in ten: more
        # than an ALPHA of 0.2 allows, at every threshold. With DELTA 0.01
        # a proof of the second would be a one in a hundred accident at the
        # very most.
        lc_seed 10
        lc_srand 10
        r gcreate s:good ALPHA 0.2 DELTA 0.01 SCORE
        r gcreate s:bad ALPHA 0.2 DELTA 0.01 SCORE
        set wrong 0
        for {set n 1} {$n <= 3000} {incr n} {
            set score [expr {[lc_rand 1000] / 1000.0}]
            set draw [lc_rand 100]
            foreach key {s:good s:bad} errors {5 30} {
                set correct [expr {$draw >= $errors}]
                lassign [r gpredict $key SCORE $score] action id
                if {$key eq "s:bad" && $action ne "ABSTAIN"} {
                    fail "request $n: $action from a gate whose cheap result is wrong 30 times in 100"
                }
                if {$action eq "ACCEPT" && !$correct} {incr wrong}
                if {$id ne {}} {r glearn $key $id CORRECT $correct}
            }
        }
        set i [r ginfo s:bad]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i generation] 0
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i accepted] 0
        assert_equal [dict get $i audited] 0
        assert_equal [dict get $i calibration-samples] 3000
        # A score gate has no other snapshot to try: it goes on testing.
        assert_equal [dict get $i candidate] 1
        lc_assert_counters s:bad

        set i [r ginfo s:good]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_morethan [dict get $i accepted] 2000
        assert_morethan $wrong 0
        assert {$wrong <= 0.2 * [dict get $i accepted]}
        assert_morethan [dict get $i audit-samples] 0
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i pending] 0
        lc_assert_counters s:good
    }
}

# =============================== Feedback =================================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: the order of GLEARN does not change what a score gate proves} {
        # The same 120 requests on three keys, right at 0.9 and wrong at
        # 0.3, answered oldest first, newest first and in a scattered order.
        foreach key {f:fwd f:rev f:mix} {
            lc_seed 11
            r gcreate $key ALPHA 0.2 DELTA 0.2 SCORE
            set ids {}
            for {set k 0} {$k < 120} {incr k} {
                lassign [r gpredict $key SCORE [expr {$k % 2 ? 0.3 : 0.9}]] action id
                assert_equal $action ABSTAIN
                lappend ids $id
            }
            assert_equal [lc_get $key pending] 120
            for {set j 0} {$j < 120} {incr j} {
                switch $key {
                    f:fwd {set k $j}
                    f:rev {set k [expr {119 - $j}]}
                    f:mix {set k [expr {($j * 37 + 5) % 120}]}
                }
                assert_equal [r glearn $key [lindex $ids $k] CORRECT [expr {$k % 2 ? 0 : 1}]] 1
            }
        }
        set fwd [r ginfo f:fwd]
        assert_equal [dict get $fwd state] ACTIVE
        assert_equal [dict get $fwd generation] 1
        assert_equal [dict get $fwd calibration-samples] 120
        assert_equal [dict get $fwd pending] 0
        assert {[lindex [dict get $fwd thresholds] 0] > 0.3}
        foreach key {f:rev f:mix} {
            set other [r ginfo $key]
            foreach field {state thresholds generation candidate calibration-samples audit-samples
                           pending missing revocations requests abstained} {
                assert_equal [dict get $other $field] [dict get $fwd $field] "$field of $key"
            }
        }
    }

    test {gate lifecycle: proof is applied in request order, an answer waits for the older ones} {
        lc_seed 12
        r gcreate f:wait ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        for {set k 0} {$k < 60} {incr k} {
            lassign [r gpredict f:wait SCORE 0.9] action id
            lappend ids $id
        }
        # Every answer but the oldest, newest first: far more than a proof
        # needs, but none of it may be used before the oldest request, as
        # long as that one is not late. (Proof waits a quarter of the
        # feedback timeout for it: 15 seconds with the default.)
        for {set k 59} {$k >= 1} {incr k -1} {
            assert_equal [r glearn f:wait [lindex $ids $k] CORRECT 1] 1
        }
        assert_equal [lc_get f:wait state] CALIBRATING
        assert_equal [lc_get f:wait calibration-samples] 0
        # They wait for their turn, not for GLEARN.
        assert_equal [lc_get f:wait pending] 1
        # An answer that waits for its turn is used: it cannot be given twice.
        assert_equal [r glearn f:wait [lindex $ids 59] CORRECT 0] 0

        assert_equal [r glearn f:wait [lindex $ids 0] CORRECT 1] 1
        set i [r ginfo f:wait]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i calibration-samples] 60
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i missing] 0
    }

    test {gate lifecycle: an id is good for one GLEARN, on the gate that issued it} {
        lc_seed 13
        r gcreate f:a ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate f:b ALPHA 0.2 DELTA 0.2 SCORE
        lassign [r gpredict f:a SCORE 0.9] action first
        # A retried GPREDICT is a new request.
        lassign [r gpredict f:a SCORE 0.9] action second
        lassign [r gpredict f:b SCORE 0.9] action other
        assert {$first ne $second}

        assert_equal [r glearn f:a $other CORRECT 1] 0
        assert_equal [r glearn f:a $first CORRECT 1] 1
        # Retrying is always safe, with the same answer or with another one.
        assert_equal [r glearn f:a $first CORRECT 1] 0
        assert_equal [r glearn f:a $first CORRECT 0] 0
        assert_equal [lc_get f:a calibration-samples] 1
        assert_equal [lc_get f:a pending] 1
        assert_equal [r glearn f:a $second CORRECT 1] 1
        assert_equal [lc_get f:a calibration-samples] 2
        assert_equal [lc_get f:a pending] 0

        # The other gate saw none of this.
        assert_equal [lc_get f:b pending] 1
        assert_equal [lc_get f:b calibration-samples] 0
        assert_equal [r glearn f:b $other CORRECT 1] 1
        assert_equal [lc_get f:b calibration-samples] 1
    }

    test {gate lifecycle: pending counts the requests waiting for GLEARN and returns to 0} {
        lc_seed 14
        r gcreate f:pend ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        for {set k 1} {$k <= 5} {incr k} {
            lassign [r gpredict f:pend SCORE 0.9] action id
            lappend ids $id
            assert_equal [lc_get f:pend pending] $k
        }
        set left 5
        foreach id $ids {
            assert_equal [r glearn f:pend $id CORRECT 1] 1
            incr left -1
            assert_equal [lc_get f:pend pending] $left
        }

        # A classifier that is still learning waits for its labels too.
        lc_srand 14
        r config set gate.snapshot-examples 1000
        r gcreate f:cpend ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        set ids {}
        foreach label {ignore respond ignore} {
            lassign [r gpredict f:cpend TEXT [lc_message $label]] action id
            assert_equal $action ABSTAIN
            lappend ids $id
        }
        assert_equal [lc_get f:cpend pending] 3
        set left 3
        foreach id [lreverse $ids] label {ignore respond ignore} {
            assert_equal [r glearn f:cpend $id LABEL $label] 1
            incr left -1
            assert_equal [lc_get f:cpend pending] $left
        }
        assert_equal [lc_get f:cpend learned] 3
    }

    test {gate lifecycle: a request never answered in time counts as missing and the gate keeps working} {
        lc_seed 15
        lc_gcreate_timeout 300 f:late ALPHA 0.2 DELTA 0.2 SCORE
        set lost {}
        for {set k 0} {$k < 3} {incr k} {
            lassign [r gpredict f:late SCORE 0.9] action id
            lappend lost $id
        }
        assert_equal [lc_get f:late pending] 3
        after 350

        # Too old: the answer is refused.
        assert_equal [r glearn f:late [lindex $lost 0] CORRECT 1] 0
        set i [r ginfo f:late]
        assert_equal [dict get $i missing] 3
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i calibration-samples] 3

        # The cautious reading: each of them stays a wrong guess. The proof
        # takes exactly as long as after three wrong answers, which is
        # longer than after a clean start, and it comes.
        assert_equal [lc_calibrate f:late] [lc_proof_needed 0.2 0.2 1 3]
        assert_morethan [lc_proof_needed 0.2 0.2 1 3] [lc_proof_needed 0.2 0.2 1]
        assert_equal [lc_get f:late missing] 3
        assert_equal [lc_get f:late revocations] 0
        lc_assert_counters f:late
    }

    test {gate lifecycle: an answer that comes after proof has moved on is still used, and the gate ends as if it had come at once} {
        # SPEC section 3, late and lost answers: proof waits for a late
        # answer for a quarter of the feedback timeout, then moves on and
        # counts the request as a wrong guess until its answer arrives.
        set timeout 2000
        set need [lc_proof_needed 0.2 0.2 1]
        # The requests of both gates: the cheap result is right at 0.9 and
        # wrong at 0.3, and the last request is the one at 0.9 whose answer
        # completes the proof of the thresholds above 0.3.
        set stream {}
        for {set n 1} {$n <= $need} {incr n} {
            lappend stream 0.9 1
            if {$n < $need} {lappend stream 0.3 0}
        }
        set total [expr {[llength $stream] / 2}]
        # What both gates see afterwards.
        set more {}
        for {set n 0} {$n < 50} {incr n} {lappend more 0.9 1 0.3 0}

        # The first gate gets every answer at once.
        lc_seed 61
        lc_gcreate_timeout $timeout l:prompt ALPHA 0.2 DELTA 0.2 SCORE
        foreach {score correct} $stream {
            assert_equal [lc_get l:prompt state] CALIBRATING
            lassign [r gpredict l:prompt SCORE $score] action id
            assert_equal [r glearn l:prompt $id CORRECT $correct] 1
        }
        set prompt [r ginfo l:prompt]
        assert_equal [dict get $prompt state] ACTIVE
        assert_equal [dict get $prompt generation] 1
        assert_equal [dict get $prompt calibration-samples] $total
        assert {[lindex [dict get $prompt thresholds] 0] > 0.3}
        foreach {score correct} $more {lc_score l:prompt $score $correct}
        set prompt_later [r ginfo l:prompt]

        # The second gate: the same requests, the same draws, but the
        # answer of the very first request is late.
        lc_seed 61
        lc_gcreate_timeout $timeout l:late ALPHA 0.2 DELTA 0.2 SCORE
        set start [clock milliseconds]
        set k 0
        foreach {score correct} [lrange $stream 0 end-2] {
            lassign [r gpredict l:late SCORE $score] action id
            assert_equal $action ABSTAIN
            if {[incr k] == 1} {
                set late $id
            } else {
                assert_equal [r glearn l:late $id CORRECT $correct] 1
            }
        }
        # Less than a quarter of the timeout has gone by: the newer answers
        # wait for their turn. One request is waiting for GLEARN.
        set waiting [expr {[clock milliseconds] - $start < $timeout / 4 - 10}]
        if {$waiting} {
            set i [r ginfo l:late]
            assert_equal [dict get $i calibration-samples] 0
            assert_equal [dict get $i pending] 1
            assert_equal [dict get $i state] CALIBRATING
        }
        after [expr {$timeout / 4 + 50}]
        # GINFO shows an idle gate as of its last write.
        if {$waiting} {assert_equal [lc_get l:late calibration-samples] 0}

        # The last request: the gate is written to, and proof moves on
        # without the late answer.
        lassign [lrange $stream end-1 end] score correct
        lassign [r gpredict l:late SCORE $score] action id
        assert_equal $action ABSTAIN
        set i [r ginfo l:late]
        assert_equal [dict get $i calibration-samples] [expr {$total - 1}]
        assert_equal [dict get $i pending] 2
        assert_equal [dict get $i missing] 0
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [r glearn l:late $id CORRECT $correct] 1
        # Every answer is in but the late one, which counts as a wrong
        # guess for now: the proof is one correct answer short.
        set i [r ginfo l:late]
        assert_equal [dict get $i calibration-samples] $total
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i pending] 1

        # The late answer arrives, well inside the feedback timeout. It is
        # used, once, and the request counts as what it was: a right guess.
        assert_equal [r glearn l:late $late CORRECT 1] 1
        assert_equal [r glearn l:late $late CORRECT 1] 0
        set i [r ginfo l:late]
        foreach field {state thresholds generation candidate calibration-samples pending missing
                       revocations audit-samples audit-errors requests abstained accepted audited} {
            assert_equal [dict get $i $field] [dict get $prompt $field] "$field, after the late answer"
        }
        # From here on the two gates are one and the same.
        foreach {score correct} $more {lc_score l:late $score $correct}
        assert_equal [r ginfo l:late] $prompt_later
        assert_morethan [dict get $prompt_later accepted] 0
        lc_assert_counters l:late
    }

    test {gate lifecycle: answers that come late, in batches or newest first leave the gate that answers at once would have left} {
        # SPEC section 3: proof is counted in request order, so a slow
        # teacher cannot bias it, and the verdict of the drift detector
        # does not depend on the order or the timing of the answers. Four
        # gates are asked the same 400 requests, with the same draws: the
        # cheap result is wrong at 0.3 and right at 0.9, save for every
        # sixth request at 0.9. Each gate answers from a threshold between
        # the two before the first of them, so the same requests are spot
        # checks for all four. What differs is when the answers come:
        #
        #   s:now      each at once
        #   s:batch    in batches of 25 requests, each batch newest first
        #   s:newest   after the last request, newest first
        #   s:late     after the last request and a quarter of the feedback
        #              timeout: the newest first, so that proof moves on
        #              without all the others, then the rest oldest first
        #
        # No answer is lost. The four gates must be the same gate at the
        # end, down to the last counter, and from then on: when the cheap
        # result goes wrong for good, they stop at the same request.
        set timeout 2000
        set stream {}
        for {set k 0} {$k < 400} {incr k} {
            if {$k % 2} {
                lappend stream 0.3 0
            } else {
                lappend stream 0.9 [expr {$k / 2 % 6 != 5}]
            }
        }
        set late_in_time 1
        unset -nocomplain final stopped_after by_itself
        foreach key {s:now s:batch s:newest s:late} {
            lc_seed 71
            r config set gate.feedback-timeout [expr {$key eq "s:late" ? $timeout : 60000}]
            lc_gate_between $key AUDIT 0.5
            r config set gate.feedback-timeout 60000
            set held {}
            set asked 0
            set start [clock milliseconds]
            if {$key eq "s:now"} {
                # What the detector of SPEC section 5 makes of the spot
                # checks of this stream (an AUDIT reply is one): nothing.
                set d [lc_drift_new 0.2]
            }
            foreach {score correct} $stream {
                lassign [r gpredict $key SCORE $score] action id
                incr asked
                if {$key eq "s:now" && $action eq "AUDIT"} {
                    set number [lc_drift_ask d]
                    if {!$correct && [lc_drift_wrong d $number] ne ""} {
                        fail "request $asked: the detector of SPEC section 5 raises an alarm on this stream"
                    }
                }
                if {$id eq {}} continue
                if {$key eq "s:now"} {
                    assert_equal [r glearn $key $id CORRECT $correct] 1
                    continue
                }
                lappend held [list $id $correct]
                if {$key eq "s:batch" && $asked % 25 == 0} {
                    foreach answer [lreverse $held] {
                        assert_equal [r glearn $key {*}[linsert $answer 1 CORRECT]] 1
                    }
                    set held {}
                }
            }
            if {$key eq "s:late"} {
                set before [r ginfo $key]
                assert_equal [dict get $before pending] [llength $held]
                assert_equal [r glearn $key {*}[linsert [lindex $held end] 1 CORRECT]] 1
                after [expr {$timeout / 4 + 50}]
                # Proof moves on without every answer that is late: each of
                # those requests counts as a wrong guess for now.
                assert_equal [r gresume $key] OK
                set i [r ginfo $key]
                if {[clock milliseconds] - $start < $timeout - 100} {
                    assert_equal [dict get $i calibration-samples] \
                        [expr {[dict get $before calibration-samples] + [llength $held]}]
                    assert_equal [dict get $i pending] [expr {[llength $held] - 1}]
                    assert_equal [dict get $i missing] 0
                }
                set held [lrange $held 0 end-1]
            } else {
                set held [lreverse $held]
            }
            foreach answer $held {
                set used [r glearn $key {*}[linsert $answer 1 CORRECT]]
                if {$key eq "s:late" && [clock milliseconds] - $start >= $timeout - 100} {
                    # This machine was too slow for this gate: answers were
                    # given up, and that is another story.
                    set late_in_time 0
                    break
                }
                assert_equal $used 1
            }
            set final($key) [r ginfo $key]
        }
        # The stream did what it was meant to do.
        set i $final(s:now)
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i missing] 0
        assert_morethan [dict get $i audit-samples] 64
        assert_morethan [dict get $i audit-errors] 5
        assert_morethan [dict get $i accepted] 0
        set keys {s:now s:batch s:newest}
        if {$late_in_time} {lappend keys s:late}
        foreach key [lrange $keys 1 end] {
            foreach field [dict keys $i] {
                assert_equal [dict get $final($key) $field] [dict get $i $field] "$field of $key (first) and of s:now (second)"
            }
        }

        # From now on the cheap result is wrong every time. The gates stop
        # with the same failed spot check: the one at which the detector
        # that followed s:now says "worse".
        foreach key $keys {
            lc_seed 72
            set stopped_after($key) 0
            set by_itself($key) 0
            for {set k 0} {$k < 1000} {incr k} {
                lassign [r gpredict $key SCORE 0.9] action id
                if {$action eq "ABSTAIN"} break
                if {$action eq "ACCEPT"} {
                    incr by_itself($key)
                    continue
                }
                incr stopped_after($key)
                assert_equal [r glearn $key $id CORRECT 0] 1
            }
            assert_equal $action ABSTAIN
            assert_equal [lc_get $key revocations] 1
            assert_equal [lc_get $key state] CALIBRATING
            assert_equal [lc_get $key candidate] 2
        }
        set want 0
        while {[lc_drift_wrong d [lc_drift_ask d]] ne "worse"} {incr want}
        incr want
        foreach key $keys {
            assert_equal $stopped_after($key) $want "failed spot checks that stopped $key"
            assert_equal $by_itself($key) $by_itself(s:now) "requests $key answered by itself on its way there"
        }
        assert_morethan_equal $want 7
    }

    test {gate lifecycle: proof does not move on while no answer waits, and an answer after the feedback timeout is refused} {
        lc_seed 64
        set timeout 800
        lc_gcreate_timeout $timeout l:idle ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        for {set k 0} {$k < 5} {incr k} {
            lassign [r gpredict l:idle SCORE 0.9] action id
            lappend ids $id
        }
        after [expr {$timeout / 4 + 50}]

        # A quarter of the timeout has gone by and the gate is written to,
        # but no answer waits behind the five late ones: there is nothing
        # to move on to, and nobody is counted as wrong.
        lassign [r gpredict l:idle SCORE 0.9] action id
        lappend ids $id
        set i [r ginfo l:idle]
        assert_equal [dict get $i calibration-samples] 0
        assert_equal [dict get $i pending] 6
        assert_equal [dict get $i missing] 0

        # The answer of the third request. The first two are in its way and
        # they are late: proof moves on without them, and no further.
        assert_equal [r glearn l:idle [lindex $ids 2] CORRECT 1] 1
        set i [r ginfo l:idle]
        assert_equal [dict get $i calibration-samples] 3
        assert_equal [dict get $i pending] 5
        assert_equal [dict get $i missing] 0
        # The answer of the sixth: the fourth and the fifth were in its way.
        assert_equal [r glearn l:idle [lindex $ids 5] CORRECT 1] 1
        assert_equal [lc_get l:idle calibration-samples] 6
        assert_equal [lc_get l:idle pending] 4

        # Late answers are used as long as the feedback timeout has not
        # passed: the first request was right, the fourth was wrong.
        assert_equal [r glearn l:idle [lindex $ids 0] CORRECT 1] 1
        assert_equal [r glearn l:idle [lindex $ids 3] CORRECT 0] 1
        set i [r ginfo l:idle]
        assert_equal [dict get $i calibration-samples] 6
        assert_equal [dict get $i pending] 2
        assert_equal [dict get $i missing] 0
        assert_equal [dict get $i state] CALIBRATING

        after [expr {$timeout - $timeout / 4}]
        # The feedback timeout has passed for the second and the fifth
        # request. Their answers are lost: the ids are forgotten and the
        # requests are missing.
        assert_equal [r glearn l:idle [lindex $ids 1] CORRECT 1] 0
        set i [r ginfo l:idle]
        assert_equal [dict get $i missing] 2
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i calibration-samples] 6
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [r glearn l:idle [lindex $ids 4] CORRECT 1] 0

        # Three right guesses so far (the first, the third and the sixth
        # request) and three wrong ones: the fourth, and the two that stay
        # wrong guesses because their answer never came. The proof needs
        # what it needs after three wrong answers, to the request.
        assert_equal [lc_calibrate l:idle] [expr {[lc_proof_needed 0.2 0.2 1 3] - 3}]
        assert_equal [lc_get l:idle missing] 2
        lc_assert_counters l:idle
    }

    test {gate lifecycle: with one answer in fifty lost a gate becomes ACTIVE, a quarter of the timeout later and not the whole of it} {
        lc_seed 63
        set timeout 800
        lc_gcreate_timeout $timeout l:lossy ALPHA 0.2 DELTA 0.2 SCORE
        set start [clock milliseconds]
        set lost {}
        for {set n 1} {$n <= 200} {incr n} {
            lassign [r gpredict l:lossy SCORE 0.9] action id
            assert_equal $action ABSTAIN
            if {$n % 50 == 1} {
                # The teacher failed: there is nothing to report.
                lappend lost $id
            } elseif {$n == 200} {
                # The teacher is still thinking about this one.
                set slow $id
            } else {
                assert_equal [r glearn l:lossy $id CORRECT 1] 1
            }
        }
        if {[clock milliseconds] - $start < $timeout / 4 - 10} {
            # All the answers wait behind the very first request.
            set i [r ginfo l:lossy]
            assert_equal [dict get $i state] CALIBRATING
            assert_equal [dict get $i calibration-samples] 0
            assert_equal [dict get $i pending] 5
        }
        after [expr {$timeout / 4 + 50}]

        # A quarter of the timeout later the next answer finds the lost
        # ones late: proof moves on without them, each counts as a wrong
        # guess, and the 196 right ones are proof enough.
        assert_equal [r glearn l:lossy $slow CORRECT 1] 1
        set i [r ginfo l:lossy]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i calibration-samples] 200
        assert {[lindex [dict get $i thresholds] 0] == 0}
        # Nothing was given up for that: the four requests still wait for
        # their answers.
        assert_equal [dict get $i pending] 4
        assert_equal [dict get $i missing] 0
        for {set k 0} {$k < 20} {incr k} {
            set action [lc_score l:lossy 0.9 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        }

        after [expr {$timeout - $timeout / 4}]
        # Now they are too old: missing counts them, their ids are
        # forgotten, and that is all a lost answer costs.
        set action [lc_score l:lossy 0.9 1]
        assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        set i [r ginfo l:lossy]
        assert_equal [dict get $i missing] 4
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        foreach id $lost {
            assert_equal [r glearn l:lossy $id CORRECT 1] 0
        }
        lc_assert_counters l:lossy
    }

    test {gate lifecycle: one late answer never holds up more than 16384 answered requests} {
        # gate_core.h (EVIDENCE) and CHANGES.md section 2: proof moves on
        # without a late answer after a quarter of the feedback timeout,
        # or as soon as more than 16,384 answered requests wait behind it.
        # The gate has the default timeout: a quarter of it is 15 seconds.
        lc_seed 62
        r gcreate l:many ALPHA 0.2 DELTA 0.2 SCORE
        set start [clock milliseconds]
        lassign [r gpredict l:many SCORE 0.9] action late
        for {set k 1} {$k <= 16384} {incr k} {
            lassign [r gpredict l:many SCORE 0.9] action id
            if {[r glearn l:many $id CORRECT 1] != 1} {fail "request $k: its answer was not used"}
        }
        set waiting [expr {[clock milliseconds] - $start < 14000}]
        set before [r ginfo l:many]
        if {$waiting} {
            assert_equal [dict get $before state] CALIBRATING
            assert_equal [dict get $before calibration-samples] 0
        }
        assert_equal [dict get $before pending] 1

        # One answer more than may wait.
        lassign [r gpredict l:many SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert_equal [r glearn l:many $id CORRECT 1] 1
        set i [r ginfo l:many]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i calibration-samples] 16386
        assert_equal [dict get $i pending] 1
        assert_equal [dict get $i missing] 0
        # The answers that waited are counted and gone.
        if {$waiting} {assert_lessthan [dict get $i memory] [expr {[dict get $before memory] / 10}]}

        # The late answer is still wanted, and still used.
        assert_equal [r glearn l:many $late CORRECT 1] 1
        assert_equal [lc_get l:many pending] 0
        assert_equal [lc_get l:many state] ACTIVE
        lc_assert_counters l:many
    }

    test {gate lifecycle: whatever the order and the delay of the answers, a gate proves what the betting test of SPEC section 5 proves, and stops when its drift detector says so} {
        # 8 rounds of 40 requests to a gate with AUDIT 1, checked after
        # every command against lc_model_*, the specification written down
        # a second time. Of the answers, about 7 in 10 come within the round
        # (in any order), 2 in 10 one or two rounds later, when proof has
        # moved on without them, and a few never. Between two rounds more
        # than a quarter of the feedback timeout goes by.
        #
        # A lost spot check counts in the blocks of the drift detector that
        # are open when it is given up. So that the model knows which
        # blocks those are, only a request the gate could not have answered
        # is lost in the first five rounds; any request may be lost in the
        # last three, and those are given up together at the end.
        lc_seed 67
        lc_srand 67
        set timeout 600
        set rounds 8
        lc_gcreate_timeout $timeout l:model ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        lc_model_init 0.2 0.2
        unset -nocomplain model_due model_id model_right model_round model_began
        # The model holds as long as the test keeps to its schedule: a round
        # takes less than a quarter of the timeout, an answer comes before
        # the timeout of its request, and the last three rounds together
        # take less than the timeout. If the machine was too slow for that,
        # the comparison ends there.
        set on_schedule 1
        set alarm ""
        set late_spot_checks 0
        set lost_spot_checks 0
        for {set round 1} {$round <= $rounds && $on_schedule && $alarm eq ""} {incr round} {
            set model_began($round) [clock milliseconds]
            # The commands of this round: 40 requests, and the answers due
            # from earlier rounds, each at a place of its own.
            set todo {}
            for {set k 0} {$k < 40} {incr k} {lappend todo ask}
            if {[info exists model_due($round)]} {
                foreach seq $model_due($round) {
                    set todo [linsert $todo [lc_rand [expr {[llength $todo] + 1}]] [list answer $seq]]
                }
            }
            set last_rounds [expr {$round >= $rounds - 2}]
            set lost 0
            for {set k 0} {$k < [llength $todo] && $alarm eq ""} {incr k} {
                set step [lindex $todo $k]
                if {$step eq "ask"} {
                    # Scores never fall on a threshold. The cheap result is
                    # wrong half of the time below 0.1 and nearly always
                    # right above.
                    set score [expr {([lc_rand 1000] + 0.5) / 1000}]
                    set correct [expr {[lc_rand 100] >= ($score < 0.1 ? 50 : 1)}]
                    lassign [lc_model_predict $score $round] want seq
                    lassign [r gpredict l:model SCORE $score] action id
                    set what "round $round, request $seq at $score"
                    set model_id($seq) $id
                    set model_right($seq) $correct
                    set model_round($seq) $round
                    set fate [lc_rand 100]
                    if {$fate >= 92 && ($last_rounds ? $lost < 4 : ($lost < 1 && $want ne "AUDIT"))} {
                        incr lost
                        if {$want eq "AUDIT"} {incr lost_spot_checks}
                    } elseif {$fate >= 70 && $fate < 92 && $round <= $rounds - 2} {
                        lappend model_due([expr {$round + ($fate < 85 ? 1 : 2)}]) $seq
                        if {$want eq "AUDIT"} {incr late_spot_checks}
                    } else {
                        set place [expr {$k + 1 + [lc_rand [expr {[llength $todo] - $k}]]}]
                        set todo [linsert $todo $place [list answer $seq]]
                    }
                } else {
                    set seq [lindex $step 1]
                    set what "round $round, the answer of request $seq"
                    set alarm [lc_model_learn $seq $model_right($seq) $round]
                    set used [r glearn l:model $model_id($seq) CORRECT $model_right($seq)]
                }
                set now [clock milliseconds]
                if {$now - $model_began($round) >= $timeout / 4 - 10 ||
                    ($step ne "ask" && $now - $model_began($model_round($seq)) >= $timeout - 10) ||
                    ($last_rounds && $now - $model_began([expr {$rounds - 2}]) >= $timeout - 10)} {
                    set on_schedule 0
                    break
                }
                if {$step eq "ask"} {
                    assert_equal $action $want $what
                    assert {$id ne {}}
                } else {
                    assert_equal $used 1 $what
                }
                if {$alarm eq ""} {lc_model_check l:model $what}
            }
            if {$round < $rounds} {after [expr {$timeout / 4 + 20}]}
        }
        if {$on_schedule} {
            # What these 320 requests are there for: the gate proved a
            # threshold, went on to prove the lowest one, and had spot
            # checks that failed, spot checks that were answered late and
            # spot checks that were never answered. None of that is a
            # reason to stop: the drift detector judges whole blocks.
            assert_equal $alarm ""
            assert_equal $::lc_m(policy) 0
            assert_morethan $::lc_m(audit_errors) 0
            assert_morethan $late_spot_checks 0
            assert_morethan $lost_spot_checks 0
            assert_equal [lc_get l:model state] ACTIVE
            # The feedback timeout passes for every request that is still
            # without its answer: they stay wrong guesses, and are missing.
            after [expr {$timeout + 20}]
            assert_equal [r gresume l:model] OK
            assert_equal [lc_model_give_up] ""
            lc_model_check l:model "after the feedback timeout"
            set i [r ginfo l:model]
            assert_equal [dict get $i pending] 0
            assert_equal [dict get $i missing] $::lc_m(unanswered)
            assert_equal [dict get $i missing] $::lc_m(missing)
            assert_morethan [dict get $i missing] 0
            assert_equal [dict get $i calibration-samples] [expr {40 * $rounds}]
            assert_equal [dict get $i generation] 1

            # Now the cheap result is wrong every time. The gate stops with
            # the failed spot check at which the model's detector says
            # "worse": not earlier and not later. How many that takes
            # depends on the blocks that are open, that is on everything
            # the gate was asked before.
            set failed 0
            while {$alarm eq ""} {
                if {$failed >= 100} {fail "the model's drift detector is silent after $failed failed spot checks in a row"}
                lassign [lc_model_predict 0.9 [expr {$rounds + 1}]] want seq
                lassign [r gpredict l:model SCORE 0.9] action id
                assert_equal $action AUDIT "failed spot check [expr {$failed + 1}]"
                set alarm [lc_model_learn $seq 0 [expr {$rounds + 1}]]
                assert_equal [r glearn l:model $id CORRECT 0] 1
                incr failed
                if {$alarm eq ""} {lc_model_check l:model "after $failed failed spot checks in a row"}
            }
            assert_equal $alarm worse
            assert_morethan_equal $failed 7
            set i [r ginfo l:model]
            assert_equal [dict get $i revocations] 1 "after $failed failed spot checks in a row"
            assert_equal [dict get $i state] CALIBRATING
            assert_equal [dict get $i generation] 0
            assert_equal [dict get $i candidate] 2
            assert_equal [dict get $i calibration-samples] 0
        }
        lc_assert_counters l:model
    }

    test {gate lifecycle: with 4096 requests waiting a gate abstains without an id until answers arrive} {
        lc_seed 18
        r gcreate f:full ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        set seen [dict create]
        for {set k 1} {$k <= 4096} {incr k} {
            lassign [r gpredict f:full SCORE 0.9] action id
            if {$action ne "ABSTAIN" || $id eq {} || [dict exists $seen $id]} {
                fail "request $k: '$action' '$id'"
            }
            dict set seen $id 1
            lappend ids $id
        }
        assert_equal [lc_get f:full pending] 4096

        for {set k 0} {$k < 5} {incr k} {
            assert_equal [r gpredict f:full SCORE 0.9] {ABSTAIN {}}
        }
        set i [r ginfo f:full]
        assert_equal [dict get $i pending] 4096
        assert_equal [dict get $i requests] 4101
        assert_equal [dict get $i abstained] 4101
        assert_equal [dict get $i state] CALIBRATING

        # The oldest answers arrive: there is room again.
        for {set k 0} {$k < 10} {incr k} {
            assert_equal [r glearn f:full [lindex $ids $k] CORRECT 1] 1
        }
        assert_equal [lc_get f:full pending] 4086
        lassign [r gpredict f:full SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert {$id ne {} && ![dict exists $seen $id]}
        assert_equal [lc_get f:full pending] 4087
        lc_assert_counters f:full
    }

    test {gate lifecycle: a full gate does not ACCEPT, not even a perfect score} {
        lc_seed 19
        r gcreate f:busy ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate f:busy 1
        assert_equal [lc_get f:busy state] ACTIVE

        # The gate answers, but its spot checks are never answered: they
        # pile up until 4096 of them wait.
        set audits {}
        set accepts 0
        for {set k 0} {$k < 30000} {incr k} {
            lassign [r gpredict f:busy SCORE 1] action id
            if {$action eq "ACCEPT"} {
                incr accepts
            } elseif {$action eq "AUDIT"} {
                lappend audits $id
            } else {
                break
            }
        }
        assert_equal [list $action $id] {ABSTAIN {}}
        assert_equal [llength $audits] 4096
        assert_morethan $accepts 1000
        assert_equal [lc_get f:busy pending] 4096
        assert_equal [lc_get f:busy accepted] $accepts

        for {set k 0} {$k < 50} {incr k} {
            assert_equal [r gpredict f:busy SCORE 1] {ABSTAIN {}}
        }
        assert_equal [lc_get f:busy accepted] $accepts
        assert_equal [lc_get f:busy audited] 4096
        assert_equal [lc_get f:busy pending] 4096

        # Some answers arrive: the gate answers again.
        for {set k 0} {$k < 100} {incr k} {
            assert_equal [r glearn f:busy [lindex $audits $k] CORRECT 1] 1
        }
        assert_equal [lc_get f:busy pending] 3996
        set again 0
        for {set k 0} {$k < 50} {incr k} {
            lassign [r gpredict f:busy SCORE 1] action id
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            if {$action eq "ACCEPT"} {incr again}
        }
        assert_morethan $again 0
        assert_equal [lc_get f:busy accepted] [expr {$accepts + $again}]
        lc_assert_counters f:busy
    }

    test {gate lifecycle: one lost answer does not fill a gate whose other requests are all answered} {
        # SPEC section 3: a gate holds at most 4096 requests waiting for
        # feedback, and lost feedback costs a little coverage instead of
        # shutting the gate. Here one request never gets its answer and
        # every other request is answered at once: at any moment one
        # request is waiting for feedback, not 4096. (The answers wait for
        # their turn behind the lost one, for 15 seconds at most with the
        # default timeout; a request that has its answer does not wait for
        # feedback.)
        lc_seed 20
        r gcreate f:one ALPHA 0.2 DELTA 0.2 SCORE
        lassign [r gpredict f:one SCORE 0.9] action lost
        assert {$lost ne {}}
        for {set k 1} {$k <= 4500} {incr k} {
            lassign [r gpredict f:one SCORE 0.9] action id
            if {$action eq "ABSTAIN" && $id eq {}} {
                fail "request $k: the gate is full (pending [lc_get f:one pending])\
                      although one request is waiting for its answer"
            }
            if {$id ne {}} {assert_equal [r glearn f:one $id CORRECT 1] 1}
        }
        assert_lessthan_equal [lc_get f:one pending] 1
        assert_equal [lc_get f:one requests] 4501
        lc_assert_counters f:one
    }
}

# ============================ No answers at all ===========================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a gate whose answers stop keeps answering until 32 requests in a row have ended unanswered, then stops} {
        # SPEC section 3, "No answers at all", and section 5: answers
        # missing in a row before the gate stops, 32. The gate answers from
        # a threshold between 0.3 and 0.9. With AUDIT 1 every request below
        # it is wanted as proof for the lower thresholds, and those are the
        # requests that go unanswered here. (The spot checks of what a gate
        # does answer are watched more closely than that: see "lost spot
        # checks" below.)
        lc_seed 17
        r config set gate.feedback-timeout 300
        lc_gate_between o:s AUDIT 1
        r config set gate.feedback-timeout 60000
        set thr [lc_threshold o:s]
        assert {$thr > 0.3 && $thr <= 0.9}

        # The feedback path breaks: no request gets its answer any more.
        set start [clock milliseconds]
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:s SCORE 0.3] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
        }
        assert_equal [lc_get o:s state] ACTIVE
        if {[clock milliseconds] - $start < 290} {assert_equal [lc_get o:s pending] 31}
        after 350

        # 31 requests have ended without an answer, one short of the limit.
        # (A gate does its bookkeeping when it is written to: GRESUME
        # changes nothing else here.) The gate answers.
        assert_equal [r gresume o:s] OK
        set i [r ginfo o:s]
        assert_equal [dict get $i missing] 31
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        assert_equal [lindex [dict get $i thresholds] 0] $thr
        # A request without an answer is not a spot check that failed.
        assert_equal [dict get $i audit-samples] 0
        assert_equal [dict get $i audit-errors] 0
        lassign [r gpredict o:s SCORE 0.9] action id
        assert_equal $action AUDIT
        assert_equal [lc_get o:s state] ACTIVE
        after 350

        # That spot check got no answer either. 32 in a row, and no answer
        # in between: the feedback path is broken and the gate cannot check
        # itself. It has stopped.
        lassign [r gpredict o:s SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo o:s]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 32
        assert {[dict get $i coverage] == 0}
        # Not one more request is answered, whatever its score.
        foreach score {1 0.9 0.5 0} {
            lassign [r gpredict o:s SCORE $score] action id
            assert_equal $action ABSTAIN "score $score"
        }
        lc_assert_counters o:s
    }

    test {gate lifecycle: a request that has seen an answer arrive since it was asked is a lost answer, not an outage} {
        # SPEC section 3, "No answers at all": 32 requests in a row end
        # without an answer AND no answer of any request has arrived since
        # the first of them was asked. The requests are counted in the
        # order they were asked, whenever they are given up: one request
        # whose answer is slow, 31 that never get one, the slow answer, and
        # 31 more that never get one. All 62 are given up together.
        lc_seed 25
        lc_gcreate_timeout 400 o:order ALPHA 0.2 DELTA 0.2 SCORE
        lassign [r gpredict o:order SCORE 0.9] action slow
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:order SCORE 0.9] action id
            assert {$id ne {}}
        }
        assert_equal [r glearn o:order $slow CORRECT 1] 1
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:order SCORE 0.9] action id
            assert {$id ne {}}
        }
        after 450

        # The first 31 were asked before the slow answer came: answers did
        # arrive after they were asked. The other 31 are one short of an
        # outage.
        assert_equal [r gresume o:order] OK
        set i [r ginfo o:order]
        assert_equal [dict get $i missing] 62
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i state] CALIBRATING \
            "62 lost answers, an answer in the middle of them, and the gate stopped testing"
        assert_equal [dict get $i candidate] 1
        # Each of the 62 counts as a wrong guess, next to the right one.
        assert_equal [dict get $i calibration-samples] 63

        # One more request without an answer: the 32nd since the last
        # answer arrived.
        lassign [r gpredict o:order SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert_equal [lc_get o:order state] CALIBRATING
        after 450
        lassign [r gpredict o:order SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo o:order]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i missing] 63
        assert_equal [dict get $i calibration-samples] 0
        # Nothing was answering: there is nothing the gate stopped doing.
        assert_equal [dict get $i revocations] 0
        assert_equal [r glearn o:order $id CORRECT 1] 1
        assert_equal [lc_get o:order state] CALIBRATING
        assert_equal [lc_get o:order candidate] 2
        lc_assert_counters o:order
    }

    test {gate lifecycle: an answer of any request ends a run of lost answers, also one that is proof of nothing} {
        # SPEC section 3, "No answers at all": no answer of ANY request has
        # arrived since the first of the 32 was asked. Here the snapshot
        # that is being tested says "escalate" to one message, a label this
        # gate never answers: that request is proof of nothing, and its
        # answer is an answer all the same. It arrives between two runs of
        # 31 requests that never get one.
        lc_seed 31
        r config set gate.feedback-timeout 400
        lc_exact_gate o:any {ignore respond escalate} ALWAYSDEFER 1 escalate
        r config set gate.feedback-timeout 60000
        assert_equal [lc_get o:any candidate] 1
        for {set run 0} {$run < 2} {incr run} {
            for {set k 0} {$k < 31} {incr k} {
                lassign [r gpredict o:any TEXT $::lc_one([lindex {ignore respond} [expr {$k % 2}]])] action id
                assert_equal $action ABSTAIN
                assert {$id ne {}}
            }
            if {$run == 0} {
                assert_equal [lc_exact_ask o:any escalate] ABSTAIN
                # It was no proof: 31 requests wait for their turn, not 32.
                assert_equal [lc_get o:any calibration-samples] 0
                assert_equal [lc_get o:any pending] 31
            }
        }
        after 450
        assert_equal [r gresume o:any] OK
        set i [r ginfo o:any]
        assert_equal [dict get $i missing] 62
        assert_equal [dict get $i state] CALIBRATING \
            "62 lost answers, an answer in the middle of them, and the gate stopped testing"
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i calibration-samples] 62
        assert_equal [dict get $i learned] 61

        # 31 requests have ended without an answer since that one. Another
        # answer that is proof of nothing: the feedback path works, and the
        # next request that is lost is the first of a new run.
        assert_equal [lc_exact_ask o:any escalate] ABSTAIN
        lassign [r gpredict o:any TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        after 450
        assert_equal [r gresume o:any] OK
        set i [r ginfo o:any]
        assert_equal [dict get $i missing] 63
        assert_equal [dict get $i state] CALIBRATING \
            "an answer arrived, one more request was lost, and the gate stopped testing"
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i learned] 62

        # 31 more, and no answer of any kind: 32 in a row.
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:any TEXT $::lc_one([lindex {ignore respond} [expr {$k % 2}]])] action id
            assert_equal $action ABSTAIN
        }
        after 450
        lassign [r gpredict o:any TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo o:any]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i missing] 94
        assert_equal [dict get $i learned] 62
        lc_assert_counters o:any
    }

    test {gate lifecycle: during an outage every request gets an id and no snapshot is taken; the first answer starts a new test} {
        lc_seed 18
        set ids [list [lc_outage o:ids]]
        set i [r ginfo o:ids]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 20

        # The gate needs one answer to start again, so it asks for one with
        # every request, whatever the score.
        for {set k 0} {$k < 40} {incr k} {
            lassign [r gpredict o:ids SCORE [lindex {0.9 0.1 1 0} [expr {$k % 4}]]] action id
            assert_equal $action ABSTAIN
            assert {$id ne {} && $id ni $ids}
            lappend ids $id
        }
        set i [r ginfo o:ids]
        if {[clock milliseconds] - $::lc_outage_found < 290} {assert_equal [dict get $i pending] 41}
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i candidate] 0
        after 350

        # Still no answer, and these requests are too old now. Nothing was
        # being tested, so they are not missing proof, and the outage has
        # not used up a snapshot number.
        lassign [r gpredict o:ids SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert {$id ne {} && $id ni $ids}
        set i [r ginfo o:ids]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i missing] 20
        assert_equal [dict get $i pending] 1
        assert_equal [dict get $i revocations] 1
        # An answer that comes too late is refused, and starts nothing.
        assert_equal [r glearn o:ids [lindex $ids 0] CORRECT 1] 0
        assert_equal [r glearn o:ids [lindex $ids 5] CORRECT 1] 0
        assert_equal [lc_get o:ids state] COLD

        # The feedback path is repaired: the first answer that is used.
        assert_equal [r glearn o:ids $id CORRECT 1] 1
        set i [r ginfo o:ids]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i pending] 0
        # That request was asked while nothing was tested: it proves nothing.
        assert_equal [dict get $i calibration-samples] 0

        # The gate proves itself again, as snapshot 2 with the smaller
        # share of the budget that goes with that number.
        assert_equal [lc_calibrate o:ids] [lc_proof_needed 0.2 0.2 2]
        set i [r ginfo o:ids]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 20
        lassign [r gpredict o:ids SCORE 0.9] action id
        assert_equal $action AUDIT
        assert_equal [r glearn o:ids $id CORRECT 1] 1
        lc_assert_counters o:ids
    }

    test {gate lifecycle: fewer than 32 lost answers in a row do not stop a gate} {
        lc_seed 19
        r config set gate.feedback-timeout 300
        lc_gate_between o:burst AUDIT 1
        r config set gate.feedback-timeout 60000
        set thr [lc_threshold o:burst]
        for {set burst 1} {$burst <= 2} {incr burst} {
            # 31 requests below the threshold, wanted as proof and never
            # answered.
            for {set k 0} {$k < 31} {incr k} {
                lassign [r gpredict o:burst SCORE 0.3] action id
                assert_equal $action ABSTAIN
                assert {$id ne {}}
            }
            after 350
            # An answer arrives: the feedback path works, and the run of
            # lost answers is over.
            assert_equal [lc_score o:burst 0.9 1] AUDIT
            set i [r ginfo o:burst]
            assert_equal [dict get $i missing] [expr {31 * $burst}] "burst $burst"
            assert_equal [dict get $i state] ACTIVE "burst $burst"
        }
        # 62 answers were lost, never 32 in a row. That costs nothing but
        # the proof they would have been: same snapshot, same threshold, no
        # failed spot check.
        set i [r ginfo o:burst]
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i revocations] 0
        assert_equal [lindex [dict get $i thresholds] 0] $thr
        assert_equal [dict get $i audit-samples] 2
        assert_equal [dict get $i audit-errors] 0
        assert_equal [dict get $i pending] 0
        lc_assert_counters o:burst
    }

    test {gate lifecycle: lost answers with answers in between do not stop a gate, however many of them are given up at once} {
        # SPEC section 3: when 32 requests in a row end without an answer
        # and no answer arrives in between, the feedback path is broken.
        # Here it works, nine times in ten: of 400 spot checks every tenth
        # gets no answer, the others get theirs at once. Then the gate is
        # left alone until the 40 are too old, so that the next command
        # gives them all up together (a gate does its bookkeeping when it
        # is written to). 40 lost answers, and never two in a row. Nor can
        # they hide that the gate got worse: one spot check in ten is half
        # of what ALPHA allows to fail.
        lc_seed 24
        lc_gcreate_timeout 400 o:tenth ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        lc_calibrate o:tenth
        for {set n 1} {$n <= 400} {incr n} {
            lassign [r gpredict o:tenth SCORE 0.9] action id
            assert_equal $action AUDIT
            if {$n % 10} {assert_equal [r glearn o:tenth $id CORRECT 1] 1}
        }
        assert_equal [lc_get o:tenth audit-samples] 360
        after 450

        lassign [r gpredict o:tenth SCORE 0.9] action id
        set i [r ginfo o:tenth]
        assert_equal [dict get $i state] ACTIVE \
            "360 of 400 spot checks were answered, and the gate stopped (missing [dict get $i missing],\
             revocations [dict get $i revocations])"
        assert_equal $action AUDIT
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        # A lost answer costs a little coverage and nothing else.
        assert_equal [dict get $i missing] 40
        assert_equal [dict get $i audit-samples] 360
        assert_equal [dict get $i audit-errors] 0
        assert {[lc_threshold o:tenth] == 0}
        lc_assert_counters o:tenth
    }

    test {gate lifecycle: a gate that is still calibrating when its answers stop stops testing, and never becomes ACTIVE without answers} {
        lc_seed 20
        lc_gcreate_timeout 300 o:cal ALPHA 0.2 DELTA 0.2 SCORE
        for {set k 0} {$k < 10} {incr k} {
            assert_equal [lc_score o:cal 0.9 1] ABSTAIN
        }
        assert_equal [lc_get o:cal calibration-samples] 10

        # The outage: 31 requests without an answer.
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:cal SCORE 0.9] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
        }
        after 350
        # And one more.
        lassign [r gpredict o:cal SCORE 0.9] action id
        assert_equal $action ABSTAIN
        set i [r ginfo o:cal]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i missing] 31
        # Each of them counts as a wrong guess.
        assert_equal [dict get $i calibration-samples] 41
        after 350

        # 32 in a row: the test of snapshot 1 is over. The gate was not
        # answering, so there is nothing it stopped doing.
        set ids {}
        for {set k 0} {$k < 60} {incr k} {
            lassign [r gpredict o:cal SCORE 0.9] action id
            assert_equal $action ABSTAIN
            assert {$id ne {} && $id ni $ids}
            lappend ids $id
        }
        set i [r ginfo o:cal]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i missing] 32
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i calibration-samples] 0
        assert_equal [dict get $i accepted] 0
        assert_equal [dict get $i audited] 0

        # The answers come back: the first one starts the test of snapshot
        # 2, and the gate is proven by the answers it gets from then on.
        assert_equal [r glearn o:cal [lindex $ids end] CORRECT 1] 1
        set i [r ginfo o:cal]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i calibration-samples] 0
        # Answers of requests asked during the outage are welcome, and
        # prove nothing.
        assert_equal [r glearn o:cal [lindex $ids end-1] CORRECT 1] 1
        assert_equal [lc_get o:cal calibration-samples] 0
        assert_equal [lc_calibrate o:cal] [lc_proof_needed 0.2 0.2 2]
        set i [r ginfo o:cal]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i revocations] 0
        lc_assert_counters o:cal
    }

    test {gate lifecycle: a classifier whose answers stop while it is tested keeps what it learned, and is tested again at the first answer} {
        lc_seed 21
        r config set gate.feedback-timeout 300
        lc_exact_gate o:c {ignore respond}
        r config set gate.feedback-timeout 60000
        for {set n 0} {$n < 5} {incr n} {
            assert_equal [lc_exact_ask o:c ignore] ABSTAIN
            assert_equal [lc_exact_ask o:c respond] ABSTAIN
        }
        set i [r ginfo o:c]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i calibration-samples] 10
        assert_equal [dict get $i learned] 50

        # 31 requests without an answer, one short of an outage.
        for {set k 0} {$k < 31} {incr k} {
            lassign [r gpredict o:c TEXT $::lc_one([lindex {ignore respond} [expr {$k % 2}]])] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
        }
        after 350
        lassign [r gpredict o:c TEXT $::lc_one(respond)] action id
        assert_equal $action ABSTAIN
        set i [r ginfo o:c]
        assert_equal [dict get $i missing] 31
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i calibration-samples] 41
        after 350

        # The 32nd in a row: the gate cannot check itself, and stops
        # testing.
        lassign [r gpredict o:c TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo o:c]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i missing] 32
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i calibration-samples] 0
        # Missing answers say nothing about the model: nothing is forgotten.
        assert_equal [dict get $i learned] 50

        # The first answer: the model the gate kept is frozen and tested at
        # once, under the next snapshot number.
        assert_equal [r glearn o:c $id LABEL ignore] 1
        set i [r ginfo o:c]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i learned] 51
        # It knows the answers already: one label is proven by exactly the
        # correct answers snapshot 2 needs.
        set need [lc_proof_needed 0.2 0.2 2 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get o:c state] CALIBRATING "after [expr {$n - 1}] of $need answers"
            assert_equal [lc_exact_ask o:c ignore] ABSTAIN
        }
        set i [r ginfo o:c]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i thresholds] {0.5 {}}
        assert_equal [dict get $i revocations] 0
        lc_assert_counters o:c
    }

    test {gate lifecycle: GRESET during an outage starts a new test at once} {
        lc_seed 22
        set id [lc_outage o:reset]
        assert_equal [lc_get o:reset state] COLD
        assert_equal [lc_get o:reset candidate] 0

        assert_equal [r greset o:reset] OK
        set i [r ginfo o:reset]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i generation] 0
        foreach field {requests accepted audited abstained audit-samples audit-errors
                       calibration-samples pending missing revocations} {
            assert_equal [dict get $i $field] 0 $field
        }
        # The id from before the reset is refused.
        assert_equal [r glearn o:reset $id CORRECT 1] 0
        assert_equal [lc_calibrate o:reset] [lc_proof_needed 0.2 0.2 2]
        assert_equal [lc_get o:reset state] ACTIVE
        assert_equal [lc_get o:reset generation] 2
        lc_assert_counters o:reset
    }

    test {gate lifecycle: what an outage leaves behind does not depend on when the gate was written to} {
        # Two gates, the same requests and the same answers: 32 spot checks
        # lost, one answered at once, seven more lost. The second gate is
        # written to a quarter of the timeout later (a GRESUME that changes
        # nothing), so its proof moves on without the lost answers before
        # they are too old. The first gate is left alone until they are.
        # Whatever a gate makes of 39 lost spot checks: it is the same 39
        # answers for both.
        set timeout 800
        foreach key {o:quiet o:written} {
            lc_seed 23
            lc_gate_spot $key $timeout
        }
        set start [clock milliseconds]
        foreach key {o:quiet o:written} {
            for {set k 0} {$k < 40} {incr k} {
                set id [lc_spot $key]
                if {$k == 32} {assert_equal [r glearn $key $id CORRECT 1] 1}
            }
        }
        set fast [expr {[clock milliseconds] - $start < $timeout - 10}]
        after [expr {$timeout / 4 + 50}]
        assert_equal [r gresume o:written] OK
        # Its proof has moved on: 32 wrong guesses for now, and the right one.
        set i [r ginfo o:written]
        assert_equal [dict get $i calibration-samples] [expr {[lc_proof_needed 0.2 0.2 1] + 33}]
        assert_equal [dict get $i missing] 0
        assert_equal [dict get $i state] ACTIVE
        after $timeout

        set replies {}
        foreach key {o:quiet o:written} {
            lassign [r gpredict $key SCORE 0.9] action id
            assert {$id ne {}}
            lappend replies $action
        }
        assert_equal [lindex $replies 1] [lindex $replies 0]
        set quiet [r ginfo o:quiet]
        set written [r ginfo o:written]
        foreach field {state generation candidate thresholds calibration-samples pending missing
                       revocations audit-samples audit-errors requests accepted audited abstained} {
            assert_equal [dict get $written $field] [dict get $quiet $field] \
                "$field of the gate that was written to (first) and of the gate that was not (second)"
        }
        # And SPEC section 5 says what that is. The lost spot checks are
        # judged against the 40 that were asked, one of them right: 467
        # million with the 27th lost one and 2,797 million with the 28th
        # (lc_drift_*). The gate stops, and what is given up after that was
        # proof for a snapshot that is gone.
        set d [lc_drift_new 0.2]
        for {set k 0} {$k < 40} {incr k} {lc_drift_ask d}
        set stop 0
        foreach number {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32} {
            incr stop
            if {[lc_drift_lost d $number] ne ""} break
        }
        assert_equal $stop 28
        assert_equal $replies {ABSTAIN ABSTAIN}
        assert_equal [dict get $quiet state] COLD
        assert_equal [dict get $quiet candidate] 0
        assert_equal [dict get $quiet revocations] 1
        if {$fast} {assert_equal [dict get $quiet missing] 28}
    }

    test {gate lifecycle: an outage that loses 95 answers in 100 costs a gate one snapshot number, not one for every lull} {
        # SPEC section 3, "Budget": every snapshot number takes its share
        # of DELTA for good, so a gate must not go through them while its
        # feedback is merely bad. Here one answer in twenty still arrives
        # (the 35th and the 40th of every 40 requests). The gate is used in
        # bursts of 200 requests with lulls longer than the feedback timeout
        # in between, so the 190 lost requests of a burst are all given up
        # together. They are no outage: answers arrived after each of them
        # was asked.
        lc_seed 29
        set timeout 400
        lc_gate_spot o:95 $timeout
        set numbers 1
        set on_schedule 1
        set answered_by_gate 0
        for {set burst 1} {$burst <= 4} {incr burst} {
            set start [clock milliseconds]
            for {set n 1} {$n <= 200} {incr n} {
                lassign [r gpredict o:95 SCORE 0.9] action id
                if {$action eq "AUDIT"} {incr answered_by_gate}
                if {$action eq "ACCEPT" || $id eq {} || ($burst > 1 && $action ne "ABSTAIN")} {
                    fail "burst $burst, request $n: '$action' '$id': [r ginfo o:95]"
                }
                if {$n % 40 == 35 || $n % 40 == 0} {assert_equal [r glearn o:95 $id CORRECT 1] 1}
            }
            if {[clock milliseconds] - $start >= $timeout - 10} {set on_schedule 0}
            set i [r ginfo o:95]
            set numbers [expr {max($numbers, [dict get $i generation], [dict get $i candidate])}]
            after [expr {$timeout + 50}]
        }
        assert_equal [r gresume o:95] OK
        set i [r ginfo o:95]
        set numbers [expr {max($numbers, [dict get $i generation], [dict get $i candidate])}]
        # It stopped answering, once, and is being tested again.
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i generation] 0
        assert_lessthan_equal $numbers 3 "snapshot numbers used up by the outage: [expr {$numbers - 1}]"
        if {$on_schedule} {
            # Burst 1: the gate answers, and its lost spot checks are
            # judged against the 200 that were asked: 615 million with the
            # 80th and 1,639 million with the 81st (lc_drift_*). The gate
            # stops and waits for an answer.
            set d [lc_drift_new 0.2]
            for {set n 1} {$n <= 200} {incr n} {lc_drift_ask d}
            set stop 0
            for {set n 1} {$n <= 200} {incr n} {
                if {$n % 40 == 35 || $n % 40 == 0} continue
                incr stop
                if {[lc_drift_lost d $n] ne ""} break
            }
            assert_equal $stop 81
            assert_equal $answered_by_gate 200
            # Burst 2: the answer of its 35th request starts the test of
            # snapshot 2, which goes on to the end. Of the 165 requests
            # after it 9 are answered, and 10 of every 200 from then on.
            assert_equal $numbers 2
            assert_equal [dict get $i state] CALIBRATING
            assert_equal [dict get $i candidate] 2
            assert_equal [dict get $i calibration-samples] [expr {165 + 2 * 200}]
            assert_equal [dict get $i missing] [expr {81 + 156 + 2 * 190}]
            assert_equal [dict get $i pending] 0

            # The outage is over: every answer arrives, and agrees. The 536
            # lost answers of snapshot 2 stay wrong guesses, and SPEC
            # section 5 says what that costs: the betting test, after 536
            # wrong answers. 29 of the correct answers it needs are in.
            set need [expr {[lc_proof_needed 0.2 0.2 2 536] - 29}]
            for {set n 1} {$n < $need} {incr n} {
                lassign [r gpredict o:95 SCORE 0.9] action id
                if {$action ne "ABSTAIN" || [r glearn o:95 $id CORRECT 1] != 1} {
                    fail "answer $n of $need after the outage: $action: [r ginfo o:95]"
                }
            }
            assert_equal [lc_get o:95 state] CALIBRATING
            assert_equal [lc_score o:95 0.9 1] ABSTAIN
            set i [r ginfo o:95]
            assert_equal [dict get $i state] ACTIVE
            assert_equal [dict get $i generation] 2
            assert_equal [dict get $i candidate] 0
            assert_equal [dict get $i revocations] 1
            assert {[lc_threshold o:95] == 0}
        }
        lc_assert_counters o:95
    }
}

# ===================== GPAUSE, GRESUME, GRESET, drift =====================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a paused gate does not answer but keeps checking itself, GRESUME lets it answer} {
        lc_seed 21
        r gcreate p:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate p:s
        set before [r ginfo p:s]

        assert_equal [r gpause p:s] OK
        set i [r ginfo p:s]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i thresholds] [dict get $before thresholds]

        # Shadow mode: nothing is answered, but the requests drawn as spot
        # checks still want their answer, and their answers still count.
        set ids 0
        for {set k 0} {$k < 60} {incr k} {
            lassign [r gpredict p:s SCORE 0.9] action id
            assert {$action ne "ACCEPT"}
            if {$id ne {}} {
                incr ids
                assert_equal [r glearn p:s $id CORRECT 1] 1
            }
        }
        assert_morethan $ids 0
        set i [r ginfo p:s]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i accepted] [dict get $before accepted]
        assert_equal [dict get $i requests] [expr {[dict get $before requests] + 60}]
        assert_equal [dict get $i audit-samples] [expr {[dict get $before audit-samples] + $ids}]
        assert_equal [dict get $i audit-errors] 0
        assert_equal [dict get $i pending] 0
        # Pausing twice changes nothing.
        assert_equal [r gpause p:s] OK
        assert_equal [lc_get p:s state] PAUSED

        assert_equal [r gresume p:s] OK
        assert_equal [lc_get p:s state] ACTIVE
        set accepts 0
        for {set k 0} {$k < 60} {incr k} {
            set action [lc_score p:s 0.9 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            if {$action eq "ACCEPT"} {incr accepts}
        }
        assert_morethan $accepts 0
        assert_equal [r gresume p:s] OK
        assert_equal [lc_get p:s state] ACTIVE
        lc_assert_counters p:s
    }

    test {gate lifecycle: a paused calibrating gate proves itself and GRESUME makes it ACTIVE at once} {
        lc_seed 22
        r gcreate p:shadow ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal [r gpause p:shadow] OK
        set i [r ginfo p:shadow]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i generation] 0

        # Feedback is accepted as before, and the proof is the proof of a
        # gate that was never paused.
        set need [lc_proof_needed 0.2 0.2 1]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_threshold p:shadow] {} "after [expr {$n - 1}] answers"
            lassign [r gpredict p:shadow SCORE 0.9] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
            assert_equal [r glearn p:shadow $id CORRECT 1] 1
        }
        set i [r ginfo p:shadow]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i generation] 1
        assert {[lindex [dict get $i thresholds] 0] ne {}}
        assert_equal [dict get $i calibration-samples] $need

        # Proven, but still paused: nothing is answered.
        for {set k 0} {$k < 40} {incr k} {
            assert {[lc_score p:shadow 0.9 1] ne "ACCEPT"}
        }
        assert_equal [lc_get p:shadow accepted] 0
        assert_equal [lc_get p:shadow state] PAUSED

        assert_equal [r gresume p:shadow] OK
        assert_equal [lc_get p:shadow state] ACTIVE
        set accepts 0
        for {set k 0} {$k < 40} {incr k} {
            set action [lc_score p:shadow 0.9 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            if {$action eq "ACCEPT"} {incr accepts}
        }
        assert_morethan $accepts 0
        lc_assert_counters p:shadow
    }

    test {gate lifecycle: a paused classifier keeps learning} {
        lc_seed 23
        lc_srand 23
        r config set gate.snapshot-examples 20
        r gcreate p:c ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        assert_equal [r gpause p:c] OK
        assert_equal [lc_get p:c state] PAUSED
        for {set k 1} {$k <= 20} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict p:c TEXT [lc_message $label]] action id
            assert_equal $action ABSTAIN
            assert_equal [r glearn p:c $id LABEL $label] 1
            assert_equal [lc_get p:c learned] $k
        }
        # Its first snapshot was taken on time, pause or not.
        set i [r ginfo p:c]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i candidate] 1
        assert_equal [r gresume p:c] OK
        assert_equal [lc_get p:c state] CALIBRATING
    }

    test {gate lifecycle: GRESET takes a score gate back to the start, under a new snapshot number} {
        lc_seed 24
        r gcreate r:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        assert_equal [lc_calibrate r:s] [lc_proof_needed 0.2 0.2 1]
        # A spot check that is still waiting for its answer.
        set old {}
        for {set k 0} {$k < 100 && $old eq {}} {incr k} {
            lassign [r gpredict r:s SCORE 0.9] action old
        }
        assert {$old ne {}}
        set before [r ginfo r:s]
        assert_equal [dict get $before state] ACTIVE
        assert_equal [dict get $before generation] 1
        assert_equal [dict get $before pending] 1

        assert_equal [r greset r:s] OK
        set i [r ginfo r:s]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i generation] 0
        assert_equal [lindex [dict get $i thresholds] 0] {}
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal [dict get $i $field] 0 $field
        }
        assert {[dict get $i coverage] == 0}
        # The configuration is kept.
        foreach field {kind alpha delta audit classes alphas} {
            assert_equal [dict get $i $field] [dict get $before $field] $field
        }
        # A snapshot number is never reused.
        assert_equal [dict get $i candidate] 2

        # Ids from before the reset are refused, and teach nothing.
        assert_equal [r glearn r:s $old CORRECT 1] 0
        assert_equal [lc_get r:s calibration-samples] 0

        # GRESET hands no budget back: snapshot 2 has a smaller share of
        # DELTA than snapshot 1 had, so its proof takes more answers.
        set second [lc_calibrate r:s]
        assert_equal $second [lc_proof_needed 0.2 0.2 2]
        assert_morethan $second [lc_proof_needed 0.2 0.2 1]
        assert_equal [lc_get r:s generation] 2

        assert_equal [r greset r:s] OK
        assert_equal [lc_get r:s candidate] 3
        assert_equal [r greset r:s] OK
        assert_equal [lc_get r:s candidate] 4
        assert_equal [lc_calibrate r:s] [lc_proof_needed 0.2 0.2 4]
        assert_equal [lc_get r:s generation] 4

        # The numbers and the budget belong to the key: a new key starts
        # from 1 again.
        r del r:s
        r gcreate r:s ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal [lc_get r:s candidate] 1
        assert_equal [r glearn r:s $old CORRECT 1] 0
        assert_equal [lc_get r:s calibration-samples] 0
    }

    test {gate lifecycle: GRESET makes a classifier forget its model} {
        lc_seed 25
        lc_srand 25
        r config set gate.snapshot-examples 100
        r gcreate r:c ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond CLASSALPHA 1 respond 0.1
        lc_history r:c {ignore respond} 100
        for {set k 0} {$k < 20} {incr k} {
            lc_decide r:c [lc_message [lc_pick {ignore respond}]]
        }
        lassign [r gpredict r:c TEXT [lc_message ignore]] action old
        assert {$old ne {}}
        set before [r ginfo r:c]
        assert_equal [dict get $before state] CALIBRATING
        assert_equal [dict get $before candidate] 1
        assert_equal [dict get $before learned] 120
        assert_equal [dict get $before pending] 1

        assert_equal [r greset r:c] OK
        set i [r ginfo r:c]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal [dict get $i $field] 0 $field
        }
        foreach field {kind alpha delta audit classes alphas} {
            assert_equal [dict get $i $field] [dict get $before $field] $field
        }
        assert_equal [r glearn r:c $old LABEL ignore] 0
        assert_equal [lc_get r:c learned] 0

        # It learns from nothing again, and its next snapshot gets a new number.
        lc_history r:c {ignore respond} 99
        assert_equal [lc_get r:c state] COLD
        lc_history r:c {ignore respond} 1
        set i [r ginfo r:c]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i learned] 100
    }

    test {gate lifecycle: GRESET does not resume a paused gate} {
        # SPEC.md does not say what GRESET does to a pause. Answering was
        # switched off by an operator, and nothing a gate forgets switches
        # it back on.
        lc_seed 26
        r gcreate r:p ALPHA 0.2 DELTA 0.2 SCORE
        lc_calibrate r:p
        r gpause r:p
        assert_equal [r greset r:p] OK
        set i [r ginfo r:p]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 2
        r gresume r:p
        assert_equal [lc_get r:p state] CALIBRATING
    }

    test {gate lifecycle: what happens to one gate leaves another gate alone} {
        lc_seed 29
        r gcreate x:a ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate x:b ALPHA 0.1 DELTA 0.05 AUDIT 0.5 SCORE
        lassign [r gpredict x:b SCORE 0.7] action id
        set b [r ginfo x:b]
        assert_equal [dict get $b pending] 1

        lc_calibrate x:a
        for {set k 0} {$k < 50} {incr k} {
            lc_score x:a 0.9 1
        }
        assert_equal [r ginfo x:b] $b
        r gpause x:a
        assert_equal [r ginfo x:b] $b
        r greset x:a
        assert_equal [r ginfo x:b] $b
        r del x:a
        assert_equal [r ginfo x:b] $b
        assert_equal [r glearn x:b $id CORRECT 1] 1
        assert_equal [lc_get x:b calibration-samples] 1
    }

    test {gate lifecycle: a score gate whose spot checks start to fail stops answering and calibrates again} {
        lc_seed 27
        r gcreate d:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate d:s
        # What SPEC section 5 makes of the spot checks of this gate, one
        # after the other (lc_drift_*): an AUDIT reply is a spot check.
        set d [lc_drift_new 0.2]
        for {set k 0} {$k < 40} {incr k} {
            if {[lc_score d:s 0.9 1] eq "AUDIT"} {lc_drift_ask d}
        }
        assert_equal [lc_get d:s audit-errors] 0

        # One wrong spot check is what an error rate of 0.2 allows.
        for {set k 0} {$k < 100} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "AUDIT"} {
                assert_equal [r glearn d:s $id CORRECT 0] 1
                assert_equal [lc_drift_wrong d [lc_drift_ask d]] ""
                break
            }
        }
        for {set k 0} {$k < 40} {incr k} {
            if {[lc_score d:s 0.9 1] eq "AUDIT"} {lc_drift_ask d}
        }
        set i [r ginfo d:s]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i audit-errors] 1
        assert_equal [dict get $i audit-samples] [dict get $d asked]
        assert_equal [dict get $i revocations] 0

        # Now the cheap result is wrong every time. A gross failure is
        # caught after a handful of spot checks, not after one, and not
        # after seven either unless a block of 64 has just begun: the
        # detector judges the block as a whole, and the right answers this
        # gate gave earlier in it count in its favour. The gate stops with
        # the failed spot check at which the detector of SPEC section 5
        # says "worse".
        set wrong_audits 0
        set wrong_accepts 0
        set verdict ""
        for {set k 0} {$k < 300} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr wrong_accepts
            } elseif {$action eq "AUDIT"} {
                assert_equal $verdict "" "the gate answers after failed spot check $wrong_audits, which should have stopped it"
                incr wrong_audits
                assert_equal [r glearn d:s $id CORRECT 0] 1
                set verdict [lc_drift_wrong d [lc_drift_ask d]]
            } else {
                break
            }
        }
        assert_equal $action ABSTAIN
        assert_equal $verdict worse "the gate stopped after $wrong_audits failed spot checks in a row"
        assert_morethan_equal $wrong_audits 7
        assert_lessthan $wrong_accepts 100
        set i [r ginfo d:s]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 2
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert {[dict get $i coverage] == 0}

        # It starts over by itself: that ABSTAIN was the first request of
        # the new test. The cheap result is right again.
        assert {$id ne {}}
        assert_equal [r glearn d:s $id CORRECT 1] 1
        assert_equal [lc_calibrate d:s] [expr {[lc_proof_needed 0.2 0.2 2] - 1}]
        set i [r ginfo d:s]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i revocations] 1
        set action [lc_score d:s 0.9 1]
        assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        lc_assert_counters d:s
    }

    test {gate lifecycle: a lost answer does not keep a gate that got worse answering} {
        # SPEC sections 1 and 3: a gate keeps spot-checking and stops by
        # itself if it gets worse; when the drift detector fires the gate
        # stops answering at once. One spot check that never got its answer
        # (a client that crashed between GPREDICT and GLEARN) is no reason
        # to ignore the spot checks that come after it.
        lc_seed 28
        lc_gate_spot d:lost
        set lost [lc_spot d:lost]
        set before [r ginfo d:lost]
        assert_equal [dict get $before audit-samples] 0
        assert_equal [dict get $before pending] 1

        # Now the cheap result is wrong every time, and every spot check
        # says so. A spot check counts as right until its answer says
        # otherwise, so the unanswered one weighs in the gate's favour: the
        # likelihood ratios of SPEC section 5 are multiplied by 0.4 / 0.2
        # = 2 and by 0.6 / 0.2 = 3 for a failed spot check and by 0.6 / 0.8
        # and 0.4 / 0.8 for any other, and their average reaches 1,000
        # with the 8th failure, not the 7th:
        # (2^7 x 0.75 + 3^7 x 0.5) / 2 = 594.75, (2^8 x 0.75 + 3^8 x 0.5) / 2 = 1736.25.
        for {set failed 1} {$failed <= 8} {incr failed} {
            set id [lc_spot d:lost]
            assert_equal [r glearn d:lost $id CORRECT 0] 1
            if {$failed < 8} {
                # As proof this answer waits for its turn behind the lost
                # one (for 15 seconds, with the default timeout). The drift
                # detector has heard of it already.
                set i [r ginfo d:lost]
                assert_equal [dict get $i state] ACTIVE "after $failed failed spot checks"
                assert_equal [dict get $i audit-errors] $failed
                assert_equal [dict get $i audit-samples] $failed
                assert_equal [dict get $i calibration-samples] [dict get $before calibration-samples]
            }
        }
        set i [r ginfo d:lost]
        assert_equal [dict get $i revocations] 1 "still answering after 8 failed spot checks in a row"
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i missing] 0
        # The spot check that was lost belongs to the snapshot that is
        # gone: its answer, if it comes after all, proves nothing.
        r glearn d:lost $lost CORRECT 1
        assert_equal [lc_get d:lost calibration-samples] 0
        assert_equal [lc_get d:lost state] CALIBRATING
        lc_assert_counters d:lost
    }

    test {gate lifecycle: a failed spot check counts the moment its answer arrives, late or not} {
        # Seven spot checks whose answers are late: proof has moved on
        # without them when they arrive, one after the other, each of them
        # saying that the cheap result was wrong. The gate answers from a
        # threshold between 0.3 and 0.9, so the requests at 0.3 that are
        # answered in the meantime are proof and no spot checks: the seven
        # are all there is in their block, and (2^7 + 3^7) / 2 = 1157.5.
        lc_seed 30
        set timeout 2000
        r config set gate.feedback-timeout $timeout
        lc_gate_between d:late AUDIT 1
        r config set gate.feedback-timeout 60000
        set late {}
        for {set k 0} {$k < 7} {incr k} {
            lassign [r gpredict d:late SCORE 0.9] action id
            assert_equal $action AUDIT
            lappend late $id
        }
        assert_equal [lc_score d:late 0.3 0] ABSTAIN
        after [expr {$timeout / 4 + 50}]
        assert_equal [lc_score d:late 0.3 0] ABSTAIN
        set i [r ginfo d:late]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i pending] 7
        assert_equal [dict get $i audit-samples] 0
        assert_equal [dict get $i audit-errors] 0
        set samples [dict get $i calibration-samples]

        set failed 0
        foreach id $late {
            assert_equal [lc_get d:late state] ACTIVE "after $failed failed spot checks"
            assert_equal [r glearn d:late $id CORRECT 0] 1
            incr failed
            if {$failed < 7} {
                set i [r ginfo d:late]
                assert_equal [dict get $i audit-samples] $failed
                assert_equal [dict get $i audit-errors] $failed
                # As proof they were wrong guesses already.
                assert_equal [dict get $i calibration-samples] $samples
            }
        }
        set i [r ginfo d:late]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i pending] 0
        lc_assert_counters d:late
    }
}

# ============================ The drift detector ==========================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: the drift detector judges a block of spot checks as a whole, the right ones included} {
        # SPEC section 5: likelihood ratios for 2x and 4x the error rate
        # (at most halfway to 1), averaged over the spot checks of a block,
        # alarm at 1,000. At ALPHA 0.2 a failed spot check multiplies them
        # by 0.4 / 0.2 = 2 and by 0.6 / 0.2 = 3, any other spot check by
        # 0.6 / 0.8 = 0.75 and by 0.4 / 0.8 = 0.5. At 0.05: by 2 and by 4,
        # and by 18/19 and 16/19. Here every spot check is answered before
        # the next is asked: 'right' agreeing ones, then disagreeing ones
        # until the gate stops, which it must do with the one called
        # 'alarm':
        #
        #   0.2,  none right   (2^6 + 3^6) / 2 = 396.5, (2^7 + 3^7) / 2 = 1157.5
        #   0.2,  2 right      (2^8 x 0.75^2 + 3^8 x 0.5^2) / 2 = 892.1,
        #                      (2^9 x 0.75^2 + 3^9 x 0.5^2) / 2 = 2604.4
        #   0.2,  10 right     374.8 with the 12th, 1009.1 with the 13th
        #   0.2,  57 right     the first block of 64 ends with its 7th wrong
        #                      one, far below 1 for all the right ones in
        #                      it; the second block starts from nothing, and
        #                      its 7th wrong one is the 14th in all
        #   0.2,  64 right     a new block: the 7th again
        #   0.05, none right   (2^5 + 4^5) / 2 = 528, (2^6 + 4^6) / 2 = 2080
        #   0.05, 5 right      891.7 with the 6th, 3518.0 with the 7th
        #   0.6,  none right   twice and four times 0.6 are no error rates:
        #                      both ratios are for 0.8, halfway to 1, and a
        #                      failed spot check multiplies them by
        #                      0.8 / 0.6: (4/3)^24 = 996.6, (4/3)^25 = 1328.8
        foreach {alpha right alarm} {
            0.2   0   7
            0.2   2   9
            0.2   10  13
            0.2   57  14
            0.2   64  7
            0.05  0   6
            0.05  5   7
            0.6   0   25
        } {
            lc_seed 74
            r del d:block
            lc_gate_spot d:block 60000 $alpha
            set what "ALPHA $alpha, $right right spot checks first"
            # The detector written down in this file says the same.
            assert_equal [lc_drift_wrong_needed $alpha $right] $alarm $what
            for {set n 0} {$n < $right} {incr n} {
                assert_equal [r glearn d:block [lc_spot d:block] CORRECT 1] 1
            }
            for {set n 1} {$n <= $alarm} {incr n} {
                set i [r ginfo d:block]
                assert_equal [dict get $i revocations] 0 "$what: after [expr {$n - 1}] failed spot checks"
                assert_equal [dict get $i state] ACTIVE $what
                assert_equal [dict get $i audit-errors] [expr {$n - 1}] $what
                assert_equal [r glearn d:block [lc_spot d:block] CORRECT 0] 1
            }
            set i [r ginfo d:block]
            assert_equal [dict get $i revocations] 1 "$what: after $alarm failed spot checks"
            assert_equal [dict get $i state] CALIBRATING $what
            assert_equal [dict get $i generation] 0 $what
            assert_equal [dict get $i candidate] 2 $what
            lc_assert_counters d:block
        }
    }

    test {gate lifecycle: spot checks that fail as often as ALPHA allows raise no alarm, spot checks that fail every other time do} {
        # One spot check in five fails, for 680 spot checks: that is the
        # error rate the gate was asked to keep, and each block of the
        # detector sinks. Then every other one fails: both likelihood
        # ratios grow by 2 x 0.75 = 3 x 0.5 = 1.5 for every two spot
        # checks, and 1.5^17 = 985, 1.5^18 = 1478. The gate is in the middle
        # of its blocks by then (spot check 681 is the 41st of a block of
        # 64), and the right answers earlier in them count in its favour:
        # the block of 64 ends before it gets anywhere (lc_drift_*), and
        # the next one starts from nothing and raises the alarm with its
        # 18th failure.
        lc_seed 75
        lc_gate_spot d:rate
        set d [lc_drift_new 0.2]
        for {set n 1} {$n <= 680} {incr n} {
            set id [lc_spot d:rate]
            set number [lc_drift_ask d]
            set wrong [expr {$n % 5 == 0}]
            assert_equal [r glearn d:rate $id CORRECT [expr {!$wrong}]] 1
            if {$wrong && [lc_drift_wrong d $number] ne ""} {
                fail "spot check $n: the detector of SPEC section 5 raises an alarm at an error rate of 0.2"
            }
        }
        set i [r ginfo d:rate]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i audit-samples] 680
        assert_equal [dict get $i audit-errors] 136

        set failed 0
        set verdict ""
        for {set n 681} {$n <= 800 && $verdict eq ""} {incr n} {
            assert_equal [lc_get d:rate revocations] 0 "spot check $n"
            set id [lc_spot d:rate]
            set number [lc_drift_ask d]
            set wrong [expr {$n % 2 == 0}]
            assert_equal [r glearn d:rate $id CORRECT [expr {!$wrong}]] 1
            if {$wrong} {
                incr failed
                set verdict [lc_drift_wrong d $number]
            }
        }
        assert_equal $verdict worse
        # Spot checks 681 to 704 end a block of 64; 705 opens one, and
        # 705 + 2 x 18 - 1 = 740 is its 18th failure.
        assert_equal $n 741
        assert_equal $failed 30
        set i [r ginfo d:rate]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] CALIBRATING
        lc_assert_counters d:rate
    }

    test {gate lifecycle: the verdict of the drift detector depends on how many answers disagree, not on the order they arrive in} {
        # Ten spot checks are asked before any of them is answered. One
        # answer agrees and nine disagree; they arrive oldest first,
        # newest first, or scattered, and the agreeing one first, last, or
        # in the middle. The block is the same ten spot checks every time:
        # (2^8 x 0.75^2 + 3^8 x 0.5^2) / 2 = 892.1 with eight failures known
        # and (2^9 x 0.75 + 3^9 x 0.5) / 2 = 5112.8 with nine. The gate
        # stops with the ninth disagreeing answer, whichever it is.
        foreach {order agrees} {
            {0 1 2 3 4 5 6 7 8 9}  0
            {0 1 2 3 4 5 6 7 8 9}  9
            {9 8 7 6 5 4 3 2 1 0}  9
            {9 8 7 6 5 4 3 2 1 0}  4
            {3 7 0 9 5 1 8 2 6 4}  5
            {3 7 0 9 5 1 8 2 6 4}  4
        } {
            lc_seed 76
            r del d:order
            lc_gate_spot d:order
            set what "answers in the order $order, spot check $agrees agrees"
            set ids {}
            for {set k 0} {$k < 10} {incr k} {lappend ids [lc_spot d:order]}
            set failed 0
            foreach k $order {
                set i [r ginfo d:order]
                assert_equal [dict get $i revocations] 0 "$what: after $failed disagreeing answers"
                assert_equal [dict get $i audit-errors] $failed $what
                assert_equal [r glearn d:order [lindex $ids $k] CORRECT [expr {$k == $agrees}]] 1
                if {$k != $agrees} {incr failed}
                if {$failed == 9} break
            }
            assert_equal [lc_get d:order revocations] 1 "$what: after 9 disagreeing answers"
            assert_equal [lc_get d:order state] CALIBRATING $what
        }

        # Answered one by one instead, the first seven failures are a block
        # of seven spot checks, all wrong, and that is an alarm (see above).
        # An answer that takes its time can only make the gate slower to
        # stop, never quicker: what arrives late was counted as right in
        # the meantime.
        lc_seed 76
        r del d:order
        lc_gate_spot d:order
        for {set k 0} {$k < 7} {incr k} {
            assert_equal [lc_get d:order revocations] 0
            assert_equal [r glearn d:order [lc_spot d:order] CORRECT 0] 1
        }
        assert_equal [lc_get d:order revocations] 1
    }

    test {gate lifecycle: a failed spot check whose block has ended counts in the longer blocks only} {
        # 71 spot checks are asked and none is answered: the first block of
        # 64 has ended, the second holds seven.
        lc_seed 77
        lc_gate_spot d:ended
        set ids {}
        for {set n 0} {$n < 71} {incr n} {lappend ids [lc_spot d:ended]}

        # Seven answers of the first block arrive, and all disagree. Among
        # the seven spot checks of the block that is open now they would be
        # an alarm; they do not belong there. They count in the block of
        # 512, which holds 71 spot checks: 7 wrong of 71 is nothing.
        for {set k 0} {$k < 7} {incr k} {
            assert_equal [r glearn d:ended [lindex $ids $k] CORRECT 0] 1
        }
        set i [r ginfo d:ended]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i audit-errors] 7
        assert_equal [dict get $i audit-samples] 7

        # The seven of the second block disagree as well: 7 wrong of 7.
        for {set k 64} {$k < 71} {incr k} {
            assert_equal [lc_get d:ended revocations] 0 "before the answer of spot check [expr {$k + 1}]"
            assert_equal [r glearn d:ended [lindex $ids $k] CORRECT 0] 1
        }
        set i [r ginfo d:ended]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] CALIBRATING
    }

    test {gate lifecycle: a gate that got worse is caught by a longer block when its answers come too late for the shorter ones} {
        # SPEC section 5: blocks of 64, 512 and 4,096 spot checks. After
        # 'right' spot checks the teacher agreed with, 'asked' spot checks
        # are asked before the first of their answers arrives, and then
        # these answers arrive oldest first, every one of them disagreeing.
        # The blocks that had ended when the first of them came are silent.
        # A block that was still open is judged with all the spot checks
        # asked in it, the unanswered ones counted as right. Its 2x ratio
        # is the one that gets there:
        #
        #   80 asked    block of 512: 2^31 x 0.75^49 / 2 = 811,
        #               2^32 x 0.75^48 / 2 = 2165: the 32nd answer
        #   520 asked   block of 4,096 (the first block of 512 has ended as
        #               well): 2^160 x 0.75^360 / 2 = 769,
        #               2^161 x 0.75^359 / 2 = 2050: the 161st answer
        #   80 asked after 512 right ones    the second block of 512 holds
        #               the 80 and nothing else: the 32nd answer again. (The
        #               block of 4,096 holds the 512 right ones as well, and
        #               is far from an alarm.)
        foreach {right asked alarm} {0 80 32  0 520 161  512 80 32} {
            lc_seed 78
            r del d:long
            lc_gate_spot d:long
            set what "$asked asked after $right right ones"
            set d [lc_drift_new 0.2]
            for {set n 0} {$n < $right} {incr n} {
                assert_equal [r glearn d:long [lc_spot d:long] CORRECT 1] 1
                lc_drift_ask d
            }
            set ids {}
            for {set n 0} {$n < $asked} {incr n} {
                lappend ids [lc_spot d:long]
                lc_drift_ask d
            }
            for {set n 1} {$n <= $alarm} {incr n} {
                if {$n % 16 == 1 || $n >= $alarm - 1} {
                    assert_equal [lc_get d:long revocations] 0 "$what: before disagreeing answer $n"
                }
                assert_equal [r glearn d:long [lindex $ids [expr {$n - 1}]] CORRECT 0] 1
                assert_equal [lc_drift_wrong d [expr {$right + $n}]] [expr {$n == $alarm ? "worse" : ""}] \
                    "$what: answer $n"
            }
            set i [r ginfo d:long]
            assert_equal [dict get $i revocations] 1 "$what: after $alarm disagreeing answers"
            assert_equal [dict get $i state] CALIBRATING
            assert_equal [dict get $i candidate] 2
        }
    }

    test {gate lifecycle: a teacher whose disagreeing answers arrive in batches does not stop a healthy gate} {
        # The teacher disagrees with every eighth spot check: 12.5 in 100,
        # well inside an ALPHA of 0.2. Its agreeing answers come at once.
        # The disagreeing ones are slow and come ten at a time, after every
        # 80 spot checks: ten failed spot checks in a row, as the gate sees
        # them arrive. They belong to 80 spot checks of which 70 were
        # right, and that is how the detector of SPEC section 5 judges
        # them.
        lc_seed 73
        lc_gate_spot d:batch
        set d [lc_drift_new 0.2]
        set held {}
        for {set n 1} {$n <= 640} {incr n} {
            set id [lc_spot d:batch]
            set number [lc_drift_ask d]
            if {$n % 8 == 0} {
                lappend held $id $number
            } else {
                assert_equal [r glearn d:batch $id CORRECT 1] 1
            }
            if {$n % 80 == 0} {
                foreach {id number} $held {
                    assert_equal [r glearn d:batch $id CORRECT 0] 1
                    if {[lc_drift_wrong d $number] ne ""} {
                        fail "spot check $number: the detector of SPEC section 5 raises an alarm"
                    }
                }
                set held {}
                set i [r ginfo d:batch]
                assert_equal [dict get $i state] ACTIVE "after $n spot checks and [expr {$n / 8}] failures in batches of ten"
                assert_equal [dict get $i revocations] 0
                assert_equal [dict get $i audit-samples] $n
                assert_equal [dict get $i audit-errors] [expr {$n / 8}]
                assert_equal [dict get $i pending] 0
            }
        }
        set i [r ginfo d:batch]
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i missing] 0
        assert_equal [dict get $i calibration-samples] [expr {[lc_proof_needed 0.2 0.2 1] + 640}]
        assert {[lc_threshold d:batch] == 0}
        lc_assert_counters d:batch
    }

    test {gate lifecycle: lost spot checks stop a gate when there are too many of them to rule out a disagreement, and not before} {
        # SPEC section 3, "Getting worse", and section 5: the detector
        # raises a second alarm, at 10^9, with every lost answer counted as
        # a disagreement. Each gate here is asked 'asked' spot checks at
        # once; the first 'right' of them are answered (the teacher
        # agrees), the others never. When they are given up, oldest first,
        # their block is judged against all the spot checks that were asked
        # in it:
        #
        #   0.2,  19 asked     (2^19 + 3^19) / 2 = 581 million
        #   0.2,  20 asked     (2^20 + 3^20) / 2 = 1,744 million: the 20th
        #   0.2,  32 asked     552 million with the 24th of the 32 and
        #                      3,312 million with the 25th (3^25 x 0.5^7 / 2
        #                      and a little from the other ratio); the other
        #                      7 were proof for a snapshot that is gone
        #   0.2,  40 asked, 13 right    27 lost: 467 million
        #   0.2,  40 asked, 12 right    28 lost: 2,797 million
        #   0.05, 15 asked     (2^15 + 4^15) / 2 = 537 million
        #   0.05, 16 asked     (2^16 + 4^16) / 2 = 2,148 million: the 16th
        #
        # 'missing' is what the gate must show afterwards, 'stops' whether
        # it has stopped. (Fewer than 32 answers are lost in a row every
        # time: this is not the outage of "No answers at all".)
        set cases {
            0.2   19  0   19  0
            0.2   20  0   20  1
            0.2   32  0   25  1
            0.2   40  13  27  0
            0.2   40  12  28  1
            0.05  15  0   15  0
            0.05  16  0   16  1
        }
        lc_seed 79
        set k 0
        foreach {alpha asked right missing stops} $cases {
            lc_gate_spot d:quiet:[incr k] 400 $alpha
        }
        set start [clock milliseconds]
        set k 0
        foreach {alpha asked right missing stops} $cases {
            set key d:quiet:[incr k]
            for {set n 0} {$n < $asked} {incr n} {
                set id [lc_spot $key]
                if {$n < $right} {assert_equal [r glearn $key $id CORRECT 1] 1}
            }
        }
        set fast [expr {[clock milliseconds] - $start < 390}]
        after 450
        set k 0
        foreach {alpha asked right missing stops} $cases {
            set key d:quiet:[incr k]
            set what "ALPHA $alpha, $asked spot checks, [expr {$asked - $right}] of them lost"
            # The detector written down in this file says the same.
            set d [lc_drift_new $alpha]
            for {set n 1} {$n <= $asked} {incr n} {lc_drift_ask d}
            set verdict ""
            for {set n [expr {$right + 1}]} {$n <= $asked && $verdict eq ""} {incr n} {
                set verdict [lc_drift_lost d $n]
            }
            assert_equal $verdict [expr {$stops ? "unchecked" : ""}] $what
            assert_equal [expr {$n - 1 - $right}] $missing $what

            # GRESUME changes nothing; the gate does its bookkeeping.
            assert_equal [r gresume $key] OK
            set i [r ginfo $key]
            assert_equal [dict get $i revocations] $stops $what
            if {$fast} {assert_equal [dict get $i missing] $missing $what}
            assert_equal [dict get $i pending] 0 $what
            # A lost answer is not a failed spot check.
            assert_equal [dict get $i audit-errors] 0 $what
            if {$stops} {
                # The gate stopped answering. It did not start over: it
                # waits for an answer, and has not used a snapshot number.
                assert_equal [dict get $i state] COLD $what
                assert_equal [dict get $i generation] 0 $what
                assert_equal [dict get $i candidate] 0 $what
                assert_equal [lindex [dict get $i thresholds] 0] {} $what
            } else {
                assert_equal [dict get $i state] ACTIVE $what
                assert_equal [dict get $i generation] 1 $what
                assert_equal [dict get $i audit-samples] $right $what
            }
            lc_assert_counters $key
        }

        # The first gate again: 19 lost spot checks, one short. The next
        # spot check is answered, and its answer disagrees. One failed spot
        # check is no reason to think the gate got worse, but it is the
        # 20th of 20 that may have failed: the gate can no longer be
        # checked. It stops as it does for a lost answer, and waits.
        set id [lc_spot d:quiet:1]
        assert_equal [r glearn d:quiet:1 $id CORRECT 0] 1
        set i [r ginfo d:quiet:1]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        # The next answer starts the test of snapshot 2.
        lassign [r gpredict d:quiet:1 SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert_equal [r glearn d:quiet:1 $id CORRECT 1] 1
        assert_equal [lc_get d:quiet:1 state] CALIBRATING
        assert_equal [lc_get d:quiet:1 candidate] 2
    }

    test {gate lifecycle: a new block starts from nothing, for lost answers as well} {
        # 19 spot checks are lost, one short of what stops a gate (see
        # above). The block of 64 is then filled up with 45 spot checks the
        # teacher agrees with, and of the next block the first six are
        # answered and 19 are lost again. That block is 25 spot checks, 19
        # of them lost: 3^19 x 0.5^6 / 2 = 9 million. (The block of 512
        # holds all 89, and 38 lost ones among 51 right ones are nothing.)
        # What the first block lost is no part of it.
        lc_seed 87
        lc_gate_spot d:fresh 400
        for {set k 0} {$k < 19} {incr k} {lc_spot d:fresh}
        after 450
        for {set k 0} {$k < 51} {incr k} {
            assert_equal [r glearn d:fresh [lc_spot d:fresh] CORRECT 1] 1
        }
        assert_equal [lc_get d:fresh missing] 19
        for {set k 0} {$k < 19} {incr k} {lc_spot d:fresh}
        after 450
        assert_equal [r gresume d:fresh] OK
        set i [r ginfo d:fresh]
        assert_equal [dict get $i state] ACTIVE \
            "19 lost spot checks in each of two blocks of 64, and the gate stopped (missing [dict get $i missing])"
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i missing] 38
        assert_equal [dict get $i audit-samples] 51
        assert_equal [dict get $i audit-errors] 0
        lc_assert_counters d:fresh
    }

    test {gate lifecycle: a gate that got worse stops however many spot checks it was asked before it is told} {
        # SPEC sections 1 and 3: a gate keeps spot-checking and stops by
        # itself if it gets worse; the verdict of the drift detector does
        # not depend on the order or the timing of the answers. Section 5:
        # blocks of 64, 512 and 4,096 spot checks in the order they were
        # asked, and a spot check counts as right until its answer says
        # otherwise.
        #
        # The teacher disagrees with two of every five spot checks, twice
        # what ALPHA allows, and those answers are slow: they arrive when
        # every spot check has been asked, oldest first. Its agreeing
        # answers come at once. Answered one by one, this stream stops a
        # gate with its 75th spot check (lc_drift_*). The first gate is
        # asked 4,096 spot checks: one block, and with the 1,210th slow
        # answer its 2x ratio is 2^1210 x 0.75^2886 / 2 = 2,355, up from
        # 883. The second gate is asked 64 more, which changes nothing
        # about its first 4,096 spot checks and their answers: it must
        # have stopped when every answer is in.
        #
        # (gate_stats.h: a disagreeing answer whose block has ended counts
        # only in the longer blocks that are still open. For the second
        # gate the block of 4,096 has ended as well when the slow answers
        # arrive, and by that rule they would count nowhere. SPEC knows no
        # such limit, and a gate that shows 1,664 failed spot checks of
        # 4,160 at an ALPHA of 0.2 has got worse.)
        set d [lc_drift_new 0.2]
        for {set n 0} {$n < 100} {incr n} {
            set number [lc_drift_ask d]
            if {$n % 5 >= 3 && [lc_drift_wrong d $number] ne ""} break
        }
        assert_equal $number 75
        set d [lc_drift_new 0.2]
        for {set n 0} {$n < 4096} {incr n} {lc_drift_ask d}
        set want 0
        for {set n 0} {$n < 4096} {incr n} {
            if {$n % 5 < 3} continue
            incr want
            if {[lc_drift_wrong d [expr {$n + 1}]] ne ""} break
        }
        assert_equal $want 1210

        foreach {key asked} {d:slow:1 4096 d:slow:2 4160} {
            lc_seed 85
            lc_gate_spot $key
            set held {}
            for {set n 0} {$n < $asked} {incr n} {
                set id [lc_spot $key]
                if {$n % 5 < 3} {
                    assert_equal [r glearn $key $id CORRECT 1] 1
                } else {
                    lappend held $id
                }
            }
            set told 0
            foreach id $held {
                # (An answer that is refused: the gate has stopped, and has
                # forgotten the request.)
                if {[r glearn $key $id CORRECT 0] != 1} break
                incr told
                if {($told % 50 == 0 || $told >= $want - 10) && [lc_get $key revocations] != 0} break
            }
            set i [r ginfo $key]
            assert_equal [dict get $i revocations] 1 \
                "$asked spot checks, [llength $held] of them failed, $told of these answers are in, and the gate answers\
                 (audit-errors [dict get $i audit-errors], audit-samples [dict get $i audit-samples])"
            if {$asked == 4096} {
                assert_equal $told $want "$asked spot checks: slow answers before the gate stopped"
            }
            assert_equal [dict get $i state] CALIBRATING
            assert_equal [dict get $i candidate] 2
        }
    }

    test {gate lifecycle: lost spot checks stop a gate however many spot checks it was asked in the meantime} {
        # SPEC section 3, "Getting worse": a spot check whose answer is
        # lost may hide a disagreement; when so many are lost that the gate
        # can no longer rule that out, it stops answering. Two gates are
        # asked the same spot checks, and two of every five never get an
        # answer: twice the share ALPHA allows to fail. The first gate is
        # asked 4,096 of them and the second 4,200; both are then left alone
        # until the unanswered ones are too old, and given up together.
        #
        # For the first gate the detector of SPEC section 5 has one block
        # of 4,096 spot checks, 1,638 of them lost, and the 1,224th lost
        # answer is one too many (lc_drift_*). The second gate has lost
        # 1,680 of 4,200 and can rule out a disagreement no better than
        # the first: it must have stopped as well.
        #
        # (gate_stats.h: a lost answer whose block has ended counts only in
        # the longer blocks that are still open. When the answers of the
        # second gate are given up the block of 4,096 has ended as well,
        # 104 spot checks ago, and by that rule they would count nowhere:
        # a gate that is asked more than 4,096 spot checks within its
        # feedback timeout could lose any share of their answers. SPEC
        # knows no such limit.)
        set d [lc_drift_new 0.2]
        for {set n 0} {$n < 4096} {incr n} {lc_drift_ask d}
        set want 0
        for {set n 0} {$n < 4096} {incr n} {
            if {$n % 5 < 3} continue
            incr want
            if {[lc_drift_lost d [expr {$n + 1}]] ne ""} break
        }
        assert_equal $want 1224

        lc_seed 84
        set timeout 2000
        set fast 1
        foreach {key asked} {d:busy:1 4096 d:busy:2 4200} {
            lc_gate_spot $key $timeout
            set start [clock milliseconds]
            for {set n 0} {$n < $asked && $fast} {incr n} {
                lassign [r gpredict $key SCORE 0.9] action id
                if {$action ne "AUDIT"} {
                    set fast 0
                } elseif {$n % 5 < 3} {
                    r glearn $key $id CORRECT 1
                }
            }
            if {[clock milliseconds] - $start >= $timeout - 100} {set fast 0}
        }
        # Had this machine been too slow for that, answers would have been
        # given up while the requests were still coming: another test.
        if {$fast} {
            foreach {key lost} {d:busy:1 1638 d:busy:2 1680} {
                set i [r ginfo $key]
                assert_equal [dict get $i state] ACTIVE
                assert_equal [dict get $i pending] $lost
                assert_equal [dict get $i audit-errors] 0
            }
            after [expr {$timeout + 50}]
            foreach {key asked lost} {d:busy:1 4096 1638 d:busy:2 4200 1680} {
                assert_equal [r gresume $key] OK
                set i [r ginfo $key]
                assert_equal [dict get $i state] COLD \
                    "$lost of $asked spot checks were never answered, and the gate answers\
                     (missing [dict get $i missing], revocations [dict get $i revocations])"
                assert_equal [dict get $i revocations] 1
                if {$asked == 4096} {
                    assert_equal [dict get $i missing] $want "$asked spot checks: missing"
                }
                assert_equal [dict get $i generation] 0
                assert_equal [dict get $i candidate] 0
            }
        }
    }

    test {gate lifecycle: a paused gate whose spot checks go unanswered does not come back answering} {
        # SPEC section 2: a paused gate keeps proving itself (shadow mode).
        # It keeps checking itself as well: the requests it would have
        # answered are spot-checked as before, and 20 of those without an
        # answer are as many as an answering gate may lose (see above).
        lc_seed 86
        lc_gate_spot d:shadow 400
        assert_equal [r gpause d:shadow] OK
        for {set k 0} {$k < 20} {incr k} {
            lassign [r gpredict d:shadow SCORE 0.9] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
        }
        set i [r ginfo d:shadow]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i generation] 1
        assert {[lc_threshold d:shadow] == 0}
        after 450
        assert_equal [r gresume d:shadow] OK
        set i [r ginfo d:shadow]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i missing] 20
        lassign [r gpredict d:shadow SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert_equal [dict get [r ginfo d:shadow] accepted] 0
        lc_assert_counters d:shadow
    }

    test {gate lifecycle: a gate that got worse starts over, lost answers or not} {
        # SPEC section 3: when the drift detector fires the gate forgets
        # its model and starts over; when it can no longer be checked for
        # all the answers that were lost, it stops and keeps its model.
        # gate_stats.h: if a disagreeing answer makes both true at once,
        # the gate got worse. Eight spot checks are lost, and then every
        # spot check is answered and fails. With the 11th failure the block
        # holds 19 spot checks: (2^11 x 0.75^8 + 3^11 x 0.5^8) / 2 = 449 for
        # the failures alone, and (2^19 + 3^19) / 2 = 581 million with the
        # lost ones. With the 12th: 1,243, and 1,744 million. Both alarms
        # at once, and the first one counts.
        lc_seed 83
        lc_gate_spot d:both 400
        for {set k 0} {$k < 8} {incr k} {lc_spot d:both}
        after 450
        assert_equal [r gresume d:both] OK
        assert_equal [lc_get d:both state] ACTIVE
        assert_equal [lc_get d:both missing] 8
        for {set failed 1} {$failed <= 12} {incr failed} {
            set i [r ginfo d:both]
            assert_equal [dict get $i revocations] 0 "after [expr {$failed - 1}] failed spot checks"
            assert_equal [dict get $i state] ACTIVE
            assert_equal [r glearn d:both [lc_spot d:both] CORRECT 0] 1
        }
        set i [r ginfo d:both]
        assert_equal [dict get $i revocations] 1
        # Starting over, not waiting for an answer: the next test has begun.
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i generation] 0
        lc_assert_counters d:both
    }

    test {gate lifecycle: a gate whose disagreeing spot checks are never answered stops answering, and proves itself again when answers arrive} {
        # SPEC section 3, "Getting worse": a spot check whose answer is
        # lost may hide a disagreement. Here the answers that are lost are
        # exactly the ones that matter. Of every three spot checks the
        # teacher agrees with the first, and the client reports that one at
        # once; it disagrees with the other two, and those are never
        # reported (a client that fails on the hard cases). Not one failed
        # spot check reaches the gate, and its feedback path is alive: the
        # last answer is never more than two requests old.
        lc_seed 80
        lc_gate_spot d:hide 400
        set d [lc_drift_new 0.2]
        set hidden {}
        set start [clock milliseconds]
        for {set n 0} {$n < 60} {incr n} {
            set id [lc_spot d:hide]
            set number [lc_drift_ask d]
            if {$n % 3 == 0} {
                assert_equal [r glearn d:hide $id CORRECT 1] 1
            } else {
                lappend hidden $number
            }
        }
        set fast [expr {[clock milliseconds] - $start < 390}]
        set i [r ginfo d:hide]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i audit-samples] 20
        assert_equal [dict get $i audit-errors] 0
        after 450

        # The 40 are given up, oldest first. For the detector of SPEC
        # section 5 each is a lost answer among 60 spot checks, 20 of them
        # right: 758 million with the 35th and 4,508 million with the 36th
        # (3^36 x 0.5^24 / 2, and a little from the other ratio).
        set stop 0
        foreach number $hidden {
            incr stop
            if {[lc_drift_lost d $number] ne ""} break
        }
        assert_equal $stop 36
        lassign [r gpredict d:hide SCORE 0.9] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo d:hide]
        assert_equal [dict get $i state] COLD \
            "40 of 60 spot checks were never answered, and the gate answers (missing [dict get $i missing])"
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [lindex [dict get $i thresholds] 0] {}
        assert_equal [dict get $i revocations] 1
        if {$fast} {assert_equal [dict get $i missing] 36}
        assert {[dict get $i coverage] == 0}

        # Answers arrive again, all of them: the gate proves itself again,
        # as snapshot 2.
        assert_equal [r glearn d:hide $id CORRECT 1] 1
        assert_equal [lc_get d:hide state] CALIBRATING
        assert_equal [lc_get d:hide candidate] 2
        assert_equal [lc_calibrate d:hide] [lc_proof_needed 0.2 0.2 2]
        set i [r ginfo d:hide]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i revocations] 1
        assert_equal [lc_score d:hide 0.9 1] AUDIT
        lc_assert_counters d:hide
    }

    test {gate lifecycle: a classifier whose disagreeing spot checks are never answered stops answering and keeps its model} {
        # The same for a classifier. The teacher has changed its mind about
        # "help", and the client never reports an answer the gate would not
        # have given. "thanks" is spot-checked and answered as before.
        lc_seed 81
        r config set gate.feedback-timeout 400
        lc_exact_gate d:chide {ignore respond} AUDIT 1
        r config set gate.feedback-timeout 60000
        set need [lc_proof_needed 0.2 0.2 1 0 2]
        for {set n 0} {$n < $need} {incr n} {
            lc_exact_ask d:chide ignore
            lc_exact_ask d:chide respond
        }
        set i [r ginfo d:chide]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i thresholds] {0.5 0.5}
        set learned [dict get $i learned]
        assert_equal $learned [expr {40 + 2 * $need}]

        set start [clock milliseconds]
        for {set n 0} {$n < 24} {incr n} {
            assert_equal [lc_exact_ask d:chide ignore] AUDIT
            lassign [r gpredict d:chide TEXT $::lc_one(respond)] action id
            assert_equal $action AUDIT
        }
        incr learned 24
        set fast [expr {[clock milliseconds] - $start < 390}]
        set i [r ginfo d:chide]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i audit-samples] 24
        assert_equal [dict get $i audit-errors] 0
        after 450

        # Every label has a detector of its own. The one of "respond" has
        # 24 spot checks, all lost, and they are judged among the 24 that
        # were asked: 654 million with the 21st (3^21 x 0.5^3 / 2) and
        # 3,924 million with the 22nd.
        set d [lc_drift_new 0.2]
        for {set n 1} {$n <= 24} {incr n} {lc_drift_ask d}
        for {set stop 1} {$stop <= 24} {incr stop} {
            if {[lc_drift_lost d $stop] ne ""} break
        }
        assert_equal $stop 22
        lassign [r gpredict d:chide TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo d:chide]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i revocations] 1
        if {$fast} {assert_equal [dict get $i missing] 22}
        # The gate stopped answering. It did not forget its model: nothing
        # it was told says that the model is wrong.
        assert_equal [dict get $i learned] $learned

        # The first answer: the model the gate kept is frozen and tested at
        # once, as snapshot 3 (snapshot 2 was being tested when the gate
        # stopped). The teacher agrees with it again, and each label is
        # proven by exactly the correct answers snapshot 3 needs.
        assert_equal [r glearn d:chide $id LABEL ignore] 1
        incr learned
        set i [r ginfo d:chide]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 3
        assert_equal [dict get $i learned] $learned
        set need [lc_proof_needed 0.2 0.2 3 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get d:chide state] CALIBRATING "after [expr {$n - 1}] of $need answers"
            assert_equal [lc_exact_ask d:chide ignore] ABSTAIN
        }
        assert_equal [lc_get d:chide thresholds] {0.5 {}}
        assert_equal [lc_get d:chide generation] 3
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get d:chide thresholds] {0.5 {}} "after [expr {$n - 1}] of $need answers"
            assert_equal [lc_exact_ask d:chide respond] ABSTAIN
        }
        set i [r ginfo d:chide]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 3
        assert_equal [dict get $i thresholds] {0.5 0.5}
        assert_equal [dict get $i revocations] 1
        lc_assert_counters d:chide
    }

    test {gate lifecycle: a classifier whose spot checks go unanswered stops for the label that lost too many, and keeps what it learned} {
        lc_seed 82
        r config set gate.feedback-timeout 300
        lc_exact_gate d:cspot {ignore respond} AUDIT 1
        r config set gate.feedback-timeout 60000
        set need [lc_proof_needed 0.2 0.2 1 0 2]
        for {set n 0} {$n < $need} {incr n} {
            lc_exact_ask d:cspot ignore
            lc_exact_ask d:cspot respond
        }
        set i [r ginfo d:cspot]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i thresholds] {0.5 0.5}
        set learned [dict get $i learned]
        assert_equal $learned [expr {40 + 2 * $need}]
        # Every label has a detector of its own (lc_drift_*).
        unset -nocomplain drift
        set drift(ignore) [lc_drift_new 0.2]
        set drift(respond) [lc_drift_new 0.2]

        # 30 spot checks without an answer, 15 for each label, then one
        # with: fewer than 32 in a row, and (2^15 + 3^15) / 2 = 7 million
        # for each label, far from 10^9.
        set lost {}
        for {set k 0} {$k < 30} {incr k} {
            set label [lindex {ignore respond} [expr {$k % 2}]]
            lassign [r gpredict d:cspot TEXT $::lc_one($label)] action id
            assert_equal $action AUDIT
            lappend lost $label [lc_drift_ask drift($label)]
        }
        after 350
        assert_equal [lc_exact_ask d:cspot ignore] AUDIT
        lc_drift_ask drift(ignore)
        incr learned
        foreach {label number} $lost {
            assert_equal [lc_drift_lost drift($label) $number] ""
        }
        set i [r ginfo d:cspot]
        assert_equal [dict get $i missing] 30
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        # Missing answers say nothing about the model: they are not failed
        # spot checks, and nothing is forgotten.
        assert_equal [dict get $i audit-samples] 1
        assert_equal [dict get $i audit-errors] 0
        assert_equal [dict get $i learned] $learned
        assert_equal [dict get $i candidate] 2

        # 32 more, 16 for each label. They are given up in the order they
        # were asked, and each is judged with the spot checks its label had
        # before: "respond" has lost 15 of 15 and now loses the 24th of 31
        # with its 9th here, which is one too many (184 million with the
        # 23rd, 1,104 million with the 24th); "ignore", with one right
        # answer, would have got to its 10th.
        set lost {}
        for {set k 0} {$k < 32} {incr k} {
            set label [lindex {ignore respond} [expr {$k % 2}]]
            lassign [r gpredict d:cspot TEXT $::lc_one($label)] action id
            assert_equal $action AUDIT
            lappend lost $label [lc_drift_ask drift($label)]
        }
        after 350
        set stop 0
        foreach {label number} $lost {
            incr stop
            if {[lc_drift_lost drift($label) $number] ne ""} break
        }
        assert_equal $stop 18
        assert_equal $label respond
        lassign [r gpredict d:cspot TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo d:cspot]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 48
        # The gate stopped answering. It did not forget its model.
        assert_equal [dict get $i learned] $learned

        # The first answer: the model the gate kept is frozen and tested
        # at once, under the next snapshot number.
        assert_equal [r glearn d:cspot $id LABEL ignore] 1
        incr learned
        set i [r ginfo d:cspot]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 3
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i learned] $learned
        # It knows the answers already: one label is proven by exactly the
        # correct answers snapshot 3 needs.
        set need [lc_proof_needed 0.2 0.2 3 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get d:cspot state] CALIBRATING "after [expr {$n - 1}] of $need answers"
            assert_equal [lc_exact_ask d:cspot ignore] ABSTAIN
        }
        set i [r ginfo d:cspot]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 3
        assert_equal [dict get $i thresholds] {0.5 {}}
        assert_equal [dict get $i revocations] 1
        lc_assert_counters d:cspot
    }
}

# ============================ Classifier gates ============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a classifier is COLD until it has learned gate.snapshot-examples examples} {
        lc_seed 31
        lc_srand 31
        r config set gate.snapshot-examples 20
        assert_equal [r gcreate c:cold ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond] OK
        set i [r ginfo c:cold]
        assert_equal [dict get $i kind] classifier
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i classes] {ignore respond}
        assert_equal [llength [dict get $i alphas]] 2
        foreach alpha [dict get $i alphas] {assert {$alpha == 0.2}}
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i learned] 0

        # Ten examples from history: GADD replies with the number learned.
        for {set k 1} {$k <= 10} {incr k} {
            set label [lc_pick {ignore respond}]
            assert_equal [r gadd c:cold TEXT [lc_message $label] LABEL $label] $k
        }
        # Nine live requests: the gate knows nothing, asks, and learns.
        for {set k 11} {$k <= 19} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:cold TEXT [lc_message $label]] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
            assert_equal [r glearn c:cold $id LABEL $label] 1
            set i [r ginfo c:cold]
            assert_equal [dict get $i learned] $k
            assert_equal [dict get $i state] COLD
            assert_equal [dict get $i candidate] 0
            assert_equal [dict get $i calibration-samples] 0
        }
        # The twentieth example: the first snapshot is taken and tested.
        set label [lc_pick {ignore respond}]
        lassign [r gpredict c:cold TEXT [lc_message $label]] action id
        assert_equal [r glearn c:cold $id LABEL $label] 1
        set i [r ginfo c:cold]
        assert_equal [dict get $i learned] 20
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i thresholds] {{} {}}

        # While it is tested every request goes to the teacher, and every
        # teacher answer counts as proof.
        for {set k 1} {$k <= 6} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:cold TEXT [lc_message $label]] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
            assert_equal [r glearn c:cold $id LABEL $label] 1
            assert_equal [lc_get c:cold calibration-samples] $k
        }
        assert_equal [lc_get c:cold learned] 26
        lc_assert_counters c:cold
    }

    test {gate lifecycle: GADD makes the model better and never counts as proof} {
        lc_seed 32
        lc_srand 32
        r config set gate.snapshot-examples 20
        r gcreate c:add ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        lc_history c:add {ignore respond} 20
        assert_equal [lc_get c:add state] CALIBRATING
        lc_history c:add {ignore respond} 1000
        set i [r ginfo c:add]
        assert_equal [dict get $i learned] 1020
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i calibration-samples] 0
        assert_equal [dict get $i requests] 0
        assert_equal [dict get $i thresholds] {{} {}}
    }

    test {gate lifecycle: a classifier becomes ACTIVE on a learnable problem, through the client loop alone} {
        lc_seed 33
        lc_srand 33
        r config set gate.snapshot-examples 20
        r gcreate c:live ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        set states {}
        set wrong 0
        set by_gate 0
        set asked 0
        for {set n 1} {$n <= 3000} {incr n} {
            set label [lc_pick {ignore respond}]
            set text [lc_message $label]
            incr asked
            set calls $::lc_teacher_calls
            set decision [lc_decide c:live $text]
            if {$::lc_teacher_calls == $calls} {
                incr by_gate
                if {$decision ne $label} {incr wrong}
            } else {
                assert_equal $decision $label
            }
            if {$n % 5 == 0} {
                set state [lc_get c:live state]
                if {[lindex $states end] ne $state} {lappend states $state}
                if {$n % 50 == 0 && [lc_all_proven c:live]} break
            }
        }
        # COLD until 20 examples, then CALIBRATING, then ACTIVE: never back.
        assert_equal $states {COLD CALIBRATING ACTIVE}
        assert {[lc_all_proven c:live]}
        set i [r ginfo c:live]
        assert_equal [dict get $i state] ACTIVE
        assert_morethan [dict get $i generation] 0
        foreach threshold [dict get $i thresholds] {
            # SPEC section 5: two labels, thresholds from 0.5 to 0.999.
            assert {$threshold >= 0.5 && $threshold <= 0.999}
        }
        assert_morethan $by_gate 0
        assert_equal [dict get $i accepted] $by_gate
        assert {$wrong <= 0.2 * $by_gate}
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i missing] 0
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i requests] $asked
        lc_assert_counters c:live
    }

    test {gate lifecycle: a classifier answers messages it has never seen} {
        lc_seed 34
        lc_srand 34
        lc_trained_gate c:new {ignore respond} 400 1000
        # Two signal words of a label and a word the gate has never met:
        # no message of the history or of the traffic looked like this.
        set accepts 0
        set wrong 0
        foreach novel {folks crew hey yo btw fwiw imo tbh hmm okay} {
            foreach label {ignore respond} {
                set a [lc_pick $::lc_signal($label)]
                set b [lc_pick $::lc_signal($label)]
                set text "$a $novel $b"
                assert {![dict exists $::lc_seen $text]}
                lassign [r gpredict c:new TEXT $text] action arg
                if {$action eq "ACCEPT"} {
                    incr accepts
                    if {$arg ne $label} {incr wrong}
                } elseif {$arg ne {}} {
                    r glearn c:new $arg LABEL $label
                }
            }
        }
        assert_morethan_equal $accepts 15
        assert_equal $wrong 0
        lc_assert_counters c:new
    }

    test {gate lifecycle: an ALWAYSDEFER label is never returned by ACCEPT} {
        lc_seed 35
        lc_srand 35
        set labels {ignore respond escalate}
        lc_trained_gate c:defer $labels 300 2000 ALWAYSDEFER 1 escalate
        set i [r ginfo c:defer]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i classes] $labels
        lassign [dict get $i alphas] a_ignore a_respond a_escalate
        assert {$a_ignore == 0.2 && $a_respond == 0.2 && $a_escalate == 0}
        lassign [dict get $i thresholds] t_ignore t_respond t_escalate
        # SPEC section 5: three labels, thresholds from 1/3 to 0.999.
        assert {$t_ignore >= 0.333 && $t_ignore <= 0.999}
        assert {$t_respond >= 0.333 && $t_respond <= 0.999}
        assert_equal $t_escalate {}

        foreach label $labels {
            set acc_by($label) 0
            set req_by($label) 0
        }
        set wrong 0
        for {set n 0} {$n < 900} {incr n} {
            set label [lc_pick $labels]
            incr req_by($label)
            lassign [r gpredict c:defer TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                assert {$arg ne "escalate"}
                incr acc_by($arg)
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                # Deferred or spot-checked: the teacher is asked, and the
                # gate learns from the answer.
                assert_equal [r glearn c:defer $arg LABEL $label] 1
            }
        }
        assert_equal $acc_by(escalate) 0
        assert_morethan $acc_by(ignore) [expr {$req_by(ignore) / 2}]
        assert_morethan $acc_by(respond) [expr {$req_by(respond) / 2}]
        assert {$wrong <= 0.2 * ($acc_by(ignore) + $acc_by(respond))}
        assert_equal [lindex [lc_get c:defer thresholds] 2] {}
        lc_assert_counters c:defer
    }

    test {gate lifecycle: a label with a stricter CLASSALPHA gets its threshold later than the other} {
        lc_seed 36
        lc_srand 36
        r config set gate.snapshot-examples 400
        r gcreate c:strict ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond CLASSALPHA 1 respond 0.05
        lassign [lc_get c:strict alphas] a_ignore a_respond
        assert {$a_ignore == 0.2 && $a_respond == 0.05}
        lc_history c:strict {ignore respond} 400

        set first_at(ignore) 0
        set first_at(respond) 0
        set req_by(ignore) 0
        set req_by(respond) 0
        for {set n 1} {$n <= 2000 && (!$first_at(ignore) || !$first_at(respond))} {incr n} {
            set label [lc_pick {ignore respond}]
            incr req_by($label)
            lc_decide c:strict [lc_message $label]
            foreach l {ignore respond} threshold [lc_get c:strict thresholds] {
                if {$threshold ne {} && !$first_at($l)} {
                    set first_at($l) $n
                    set req_then($l) $req_by($l)
                }
            }
            if {$first_at(ignore) == $n} {
                # The label with the plain error rate is answered; the
                # strict one is not yet.
                assert_equal $first_at(respond) 0
                assert_equal [lc_get c:strict state] ACTIVE
            }
        }
        assert_morethan $first_at(ignore) 0
        assert_morethan $first_at(respond) $first_at(ignore)
        assert_morethan $req_then(respond) $req_then(ignore)
        # A label is proven by correct answers with that label, and SPEC
        # section 5 says how many it takes at the very least: the betting
        # test at the label's own error rate, against the share of the
        # budget one of 2 x 32 thresholds of the first snapshot gets. (A
        # later snapshot needs more.)
        assert_morethan_equal $req_then(ignore) [lc_proof_needed 0.2 0.2 1 0 2]
        assert_morethan_equal $req_then(respond) [lc_proof_needed 0.05 0.2 1 0 2]
        lc_assert_counters c:strict
    }

    test {gate lifecycle: a label is answered after exactly the proof SPEC section 5 asks for, counted for that label alone} {
        # lc_exact_gate: a snapshot that is never wrong. Every answer to
        # the message of a label is one correct answer for that label, and
        # the label gets its threshold with the answer that takes the
        # wealth of its test to 32 x labels x 1 x 2 / DELTA. "Labels" are
        # the labels the gate may answer; a stricter CLASSALPHA makes every
        # correct answer worth less. The lowest of the 32 thresholds is
        # max(1 / classes, 0.25): a snapshot that is always right is
        # answered from there.
        foreach {labels options alphas tested} {
            {ignore respond}           {}                                              {0.2 0.2}     2
            {ignore respond}           {CLASSALPHA 1 respond 0.05}                     {0.2 0.05}    2
            {ignore respond escalate}  {}                                              {0.2 0.2 0.2} 3
            {ignore respond escalate}  {ALWAYSDEFER 1 escalate}                        {0.2 0.2 0}   2
            {escalate respond ignore}  {CLASSALPHA 1 ignore 0.1 ALWAYSDEFER 1 escalate} {0 0.2 0.1}   2
        } {
            lc_seed 46
            r del c:exact
            lc_exact_gate c:exact $labels {*}$options
            set what "CLASSES $labels $options"
            foreach got [lc_get c:exact alphas] want $alphas {
                assert {$got == $want}
            }
            set lowest [expr {max(1.0 / [llength $labels], 0.25)}]
            set k 0
            foreach label $labels alpha $alphas {
                if {$alpha == 0} {
                    # Never tested, never answered, however often the gate
                    # is right about it. The gate still learns from it.
                    for {set n 0} {$n < 60} {incr n} {
                        assert_equal [lc_exact_ask c:exact $label] ABSTAIN
                    }
                    assert_equal [lindex [lc_get c:exact thresholds] $k] {} "$what: $label"
                } else {
                    set need [lc_proof_needed $alpha 0.2 1 0 $tested]
                    for {set n 1} {$n <= $need} {incr n} {
                        assert_equal [lindex [lc_get c:exact thresholds] $k] {} \
                            "$what: $label after [expr {$n - 1}] of $need correct answers"
                        assert_equal [lc_exact_ask c:exact $label] ABSTAIN
                    }
                    set threshold [lindex [lc_get c:exact thresholds] $k]
                    if {$threshold eq {} || abs($threshold - $lowest) > 1e-9} {
                        fail "$what: threshold '$threshold' of $label after $need correct answers"
                    }
                    assert_equal [lc_get c:exact state] ACTIVE
                    assert_equal [lc_get c:exact generation] 1
                }
                incr k
            }
            # Proven is proven: the labels that were first still have their
            # thresholds.
            foreach alpha $alphas threshold [lc_get c:exact thresholds] {
                assert_equal [expr {$threshold ne {}}] [expr {$alpha != 0}] $what
            }
            assert_equal [lc_get c:exact revocations] 0
            lc_assert_counters c:exact
        }
    }

    test {gate lifecycle: FEATURES decide the answer when the text does not} {
        lc_seed 37
        lc_srand 37
        set texts {"status update" "meeting notes" "quick sync" "weekly report"
                   "fyi see thread" "new build is out"}
        r config set gate.snapshot-examples 400
        r gcreate c:feat ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        # The same texts get both labels: only the feature tells them apart.
        for {set k 0} {$k < 400} {incr k} {
            set mentioned [lc_rand 2]
            r gadd c:feat TEXT [lc_pick $texts] FEATURES 1 mentioned $mentioned \
                LABEL [expr {$mentioned ? "respond" : "ignore"}]
        }
        set wrong 0
        set accepts 0
        for {set n 1} {$n <= 1000} {incr n} {
            set mentioned [lc_rand 2]
            set label [expr {$mentioned ? "respond" : "ignore"}]
            lassign [r gpredict c:feat TEXT [lc_pick $texts] FEATURES 1 mentioned $mentioned] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:feat $arg LABEL $label
            }
            if {$n % 10 == 0 && [lc_all_proven c:feat]} break
        }
        assert {[lc_all_proven c:feat]}
        assert {$wrong <= 0.2 * $accepts}

        foreach {mentioned label} {1 respond 0 ignore 1 respond 0 ignore} {
            set answers {}
            for {set k 0} {$k < 30} {incr k} {
                lassign [r gpredict c:feat TEXT "status update" FEATURES 1 mentioned $mentioned] action arg
                if {$action eq "ACCEPT"} {
                    lappend answers $arg
                } elseif {$arg ne {}} {
                    r glearn c:feat $arg LABEL $label
                }
            }
            assert_morethan [llength $answers] 0
            assert_equal [lsort -unique $answers] $label "mentioned $mentioned"
        }
        lc_assert_counters c:feat
    }

    test {gate lifecycle: the order of GLEARN does not change what a classifier snapshot proves} {
        foreach key {c:fwd c:rev} {
            lc_seed 38
            lc_srand 38
            r config set gate.snapshot-examples 400
            r gcreate $key ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
            lc_history $key {ignore respond} 400
            set ids {}
            set labels {}
            for {set k 0} {$k < 150} {incr k} {
                set label [lc_pick {ignore respond}]
                lassign [r gpredict $key TEXT [lc_message $label]] action id
                assert_equal $action ABSTAIN
                lappend ids $id
                lappend labels $label
            }
            # The model keeps learning from every answer, in whatever order
            # they come. The proof uses the guesses recorded when the
            # requests were asked, in the order they were asked.
            for {set j 0} {$j < 150} {incr j} {
                set k [expr {$key eq "c:fwd" ? $j : 149 - $j}]
                assert_equal [r glearn $key [lindex $ids $k] LABEL [lindex $labels $k]] 1
            }
        }
        set fwd [r ginfo c:fwd]
        set rev [r ginfo c:rev]
        assert_equal [dict get $fwd state] ACTIVE
        assert_equal [dict get $fwd calibration-samples] 150
        assert_equal [dict get $fwd learned] 550
        foreach field {state thresholds generation candidate calibration-samples learned
                       pending missing requests abstained} {
            assert_equal [dict get $rev $field] [dict get $fwd $field] $field
        }
    }

    test {gate lifecycle: a gate never becomes ACTIVE when the labels do not depend on the input} {
        lc_seed 39
        lc_srand 39
        r config set gate.snapshot-examples 100
        r gcreate c:coin ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        set tried {}
        for {set n 1} {$n <= 3000} {incr n} {
            # The teacher tosses a coin, whatever the message says.
            set text [lc_message [lc_pick {ignore respond}]]
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:coin TEXT $text] action id
            if {$action ne "ABSTAIN" || $id eq {}} {fail "request $n: '$action' '$id'"}
            assert_equal [r glearn c:coin $id LABEL $label] 1
            # The request at which each snapshot was taken.
            if {[llength $tried] < 3 && [lc_get c:coin candidate] > [llength $tried]} {
                lappend tried $n
                assert_equal [lc_get c:coin candidate] [llength $tried]
            }
        }
        set i [r ginfo c:coin]
        assert {[dict get $i state] ne "ACTIVE"}
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i accepted] 0
        assert_equal [dict get $i audited] 0
        assert_equal [dict get $i abstained] 3000
        # A snapshot that proves nothing is dropped and the next one is
        # tried, under a new number.
        assert_morethan [dict get $i candidate] 1
        # SPEC section 5 says when: after 10 times the proof a flawless
        # threshold needs, per label. Here every answer is proof for the
        # snapshot being tested, and both labels may be answered. A
        # flawless threshold of the first snapshot needs 39 correct answers
        # and one of the second 44 (lc_proof_needed): the first snapshot,
        # taken with the 100th answer, lasted for 10 x 2 x 39 = 780 pieces
        # of proof and the second for 10 x 2 x 44 = 880, and the next one
        # was taken with the answer that ended it.
        assert_equal [lc_snapshot_budget 0.2 0.2 1 2] 780
        assert_equal [lc_snapshot_budget 0.2 0.2 2 2] 880
        assert_equal $tried {100 880 1760}
        lc_assert_counters c:coin

        # With a stricter error rate for one label a flawless threshold of
        # that label needs more: 169 correct answers at 0.05, not 39. SPEC
        # does not say which of the two numbers "the proof a flawless
        # threshold needs, per label" is then. Whatever the reading, the
        # strict label is given 10 times its own proof, and no reading
        # gives more than 10 times the larger number for each label.
        lc_seed 39
        r gcreate c:coin2 ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond CLASSALPHA 1 respond 0.05
        set strict [lc_proof_needed 0.05 0.2 1 0 2]
        assert_equal $strict 169
        for {set n 1} {$n <= 100 + 10 * 2 * $strict} {incr n} {
            set text [lc_message [lc_pick {ignore respond}]]
            lassign [r gpredict c:coin2 TEXT $text] action id
            if {$action ne "ABSTAIN" || $id eq {}} {fail "request $n: '$action' '$id'"}
            assert_equal [r glearn c:coin2 $id LABEL [lc_pick {ignore respond}]] 1
            if {$n == 100 + 10 * $strict - 1} {
                assert_equal [lc_get c:coin2 candidate] 1 "after [expr {$n - 100}] pieces of proof"
            }
        }
        assert_equal [lc_get c:coin2 candidate] 2 "after [expr {$n - 101}] pieces of proof"
        assert_equal [lc_get c:coin2 generation] 0
    }

    test {gate lifecycle: with a noisy teacher the share of wrong ACCEPTs stays under ALPHA} {
        lc_seed 40
        lc_srand 40
        r config set gate.snapshot-examples 300
        r gcreate c:noisy ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        # The teacher says the other label one time in twenty (lc_noisy).
        for {set k 0} {$k < 300} {incr k} {
            set label [lc_pick {ignore respond}]
            r gadd c:noisy TEXT [lc_message $label] LABEL [lc_noisy $label]
        }
        set accepts 0
        set wrong 0
        set generation 0
        for {set n 1} {$n <= 5000} {incr n} {
            set label [lc_pick {ignore respond}]
            set text [lc_message $label]
            set teacher [lc_noisy $label]
            lassign [r gpredict c:noisy TEXT $text] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $teacher} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:noisy $arg LABEL $teacher
            }
            if {$n % 100 == 0} {
                # Snapshot numbers only grow, and the one being tested is
                # newer than the one answering.
                set i [r ginfo c:noisy]
                assert_morethan_equal [dict get $i generation] $generation
                set generation [dict get $i generation]
                if {[dict get $i candidate] != 0} {
                    assert_morethan [dict get $i candidate] $generation
                }
            }
        }
        set i [r ginfo c:noisy]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i accepted] $accepts
        # The gate is useful, and it disagrees with the teacher about as
        # often as the teacher disagrees with itself: far less than ALPHA.
        assert_morethan $accepts 2000
        assert_morethan $wrong 0
        assert {$wrong < 0.2 * $accepts}
        # Its own spot checks tell the same story, and never raised an alarm.
        assert_morethan [dict get $i audit-samples] 0
        assert_morethan [dict get $i audit-errors] 0
        assert {[dict get $i audit-errors] < 0.2 * [dict get $i audit-samples]}
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i pending] 0
        lc_assert_counters c:noisy
    }

    test {gate lifecycle: when the teacher changes its mind a classifier notices, forgets and learns again} {
        # A classifier with dice (AUDIT 0.3), noise of its own and snapshots
        # that take over. Every label has a drift detector of its own, and
        # a snapshot that takes over starts new ones. The test keeps both
        # as SPEC section 5 describes them (lc_drift_*): an AUDIT reply is
        # a spot check of the label the gate guessed. With two labels that
        # is the teacher's label if the answer agreed and the other one if
        # it did not, and GINFO says which (audit-errors).
        lc_seed 41
        unset -nocomplain drift
        lc_srand 41
        r config set gate.snapshot-examples 300
        r gcreate d:c ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 ignore respond
        lc_history d:c {ignore respond} 300
        set generation 0
        set errors 0
        set changed 0        ;# the teacher has changed its mind
        set failed 0         ;# spot checks that failed since
        set wrong 0
        set verdict ""
        set learned 300
        for {set n 1} {$n <= 1900 && $verdict eq ""} {incr n} {
            if {!$changed && $n % 10 == 1 && $n > 1 && [lc_all_proven d:c]} {
                # Every label is answered. From now on the teacher answers
                # the other label.
                assert_equal [lc_get d:c state] ACTIVE
                set first [lc_get d:c generation]
                set learned [lc_get d:c learned]
                assert_morethan $learned 300
                set changed $n
            }
            if {$changed && $n >= $changed + 400} break
            set label [lc_pick {ignore respond}]
            set answer [expr {$changed ? [lc_other $label] : $label}]
            lassign [r gpredict d:c TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                if {$changed && $arg ne $answer} {incr wrong}
                continue
            }
            if {$arg eq {}} continue
            assert_equal [r glearn d:c $arg LABEL $answer] 1
            set i [r ginfo d:c]
            if {[dict get $i revocations] > 0} {
                # The gate has stopped: on a spot check that failed, the
                # one at which the detector of its label says "worse".
                assert_equal $action AUDIT "request $n"
                incr failed
                set guessed [lc_other $answer]
                set verdict [lc_drift_wrong drift($guessed) [lc_drift_ask drift($guessed)]]
                assert_equal $verdict worse \
                    "request $n: the gate stopped at failed spot check $failed since the teacher changed its mind"
                break
            }
            if {[dict get $i generation] != $generation} {
                # A snapshot took over with this answer: new detectors,
                # and the request was no spot check of the new snapshot.
                set generation [dict get $i generation]
                set drift(ignore) [lc_drift_new 0.2]
                set drift(respond) [lc_drift_new 0.2]
                set errors 0
            } elseif {$action eq "AUDIT"} {
                set disagreed [expr {[dict get $i audit-errors] - $errors}]
                set errors [dict get $i audit-errors]
                set guessed [expr {$disagreed ? [lc_other $answer] : $answer}]
                set number [lc_drift_ask drift($guessed)]
                if {$disagreed} {
                    if {$changed} {incr failed}
                    assert_equal [lc_drift_wrong drift($guessed) $number] "" \
                        "request $n: the detector of '$guessed' says the gate got worse, and the gate answers"
                }
            }
            # Until the alarm nothing is forgotten.
            if {$changed} {assert_morethan_equal [dict get $i learned] $learned}
        }
        assert_morethan $changed 0
        assert_equal $verdict worse "still answering $failed failed spot checks after the teacher changed its mind"
        # The gate stopped answering at once and forgot its model.
        set i [r ginfo d:c]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i learned] 0
        assert_lessthan $wrong 150
        # Seven failed spot checks of one label are the least that can do
        # it (a block that has just begun), and with the 7th of the next
        # block of 64 a label is there at the latest.
        assert_range $failed 7 139

        # It starts over by itself, and what it answers now is what the
        # teacher says now.
        set accepts 0
        set wrong 0
        for {set n 1} {$n <= 3000} {incr n} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict d:c TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne [lc_other $label]} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn d:c $arg LABEL [lc_other $label]
            }
            if {$accepts >= 200} break
        }
        assert_equal $accepts 200
        assert {$wrong <= 0.2 * $accepts}
        set i [r ginfo d:c]
        assert_equal [dict get $i state] ACTIVE
        assert_morethan [dict get $i generation] $first
        assert_equal [dict get $i revocations] 1
        lc_assert_counters d:c
    }

    test {gate lifecycle: every label has a drift detector of its own, tuned to its error rate} {
        # SPEC section 5, drift detector: likelihood ratios for 2x and 4x
        # the error rate (at most halfway to 1), averaged over a block of
        # spot checks, alarm at 1,000. At 0.2 a failed spot check
        # multiplies them by 2 and by 3, and the 7th of a block in which
        # nothing else was asked takes their average to 1,000 (see the
        # score gates). At 0.05 it multiplies them by 0.1 / 0.05 = 2 and by
        # 0.2 / 0.05 = 4: (2^5 + 4^5) / 2 = 528 and (2^6 + 4^6) / 2 = 2080,
        # so the 6th does. A spot check of one label says nothing about the
        # other: failing in turns, it takes the 7th of the first label,
        # which is the 13th in all. 'warm' spot checks of each label that
        # the teacher agreed with count in the gate's favour: after two of
        # them it takes the 9th failure at 0.2, and after five the 7th at
        # 0.05 (see "the drift detector judges a block" for the sums).
        foreach {options failing warm alarm} {
            {}                           {ignore}          0  7
            {}                           {ignore}          2  9
            {CLASSALPHA 1 respond 0.05}  {ignore}          0  7
            {CLASSALPHA 1 respond 0.05}  {respond}         0  6
            {CLASSALPHA 1 respond 0.05}  {respond}         5  7
            {}                           {ignore respond}  0  13
            {}                           {ignore respond}  2  17
        } {
            lc_seed 48
            r del d:label
            lc_exact_gate d:label {ignore respond} AUDIT 1 {*}$options
            set what "$options, failing: $failing, after $warm right spot checks"
            foreach label {ignore respond} alpha [lc_get d:label alphas] {
                set need [lc_proof_needed $alpha 0.2 1 0 2]
                for {set n 0} {$n < $need} {incr n} {lc_exact_ask d:label $label}
                set label_alpha($label) $alpha
            }
            # Both labels are answered, and the teacher agrees.
            for {set n 0} {$n < $warm} {incr n} {
                assert_equal [lc_exact_ask d:label ignore] AUDIT
                assert_equal [lc_exact_ask d:label respond] AUDIT
            }
            set i [r ginfo d:label]
            assert_equal [dict get $i state] ACTIVE $what
            assert_equal [dict get $i generation] 1 $what
            assert_equal [dict get $i thresholds] {0.5 0.5} $what
            assert_equal [dict get $i audit-samples] [expr {2 * $warm}] $what
            assert_equal [dict get $i audit-errors] 0 $what
            assert_morethan [dict get $i learned] 100
            # The detector written down in this file says the same.
            set first [lindex $failing 0]
            assert_equal [expr {[llength $failing] * ([lc_drift_wrong_needed $label_alpha($first) $warm] - 1) + 1}] \
                $alarm $what

            # The teacher changes its mind about the messages of 'failing'.
            for {set n 1} {$n <= $alarm} {incr n} {
                set i [r ginfo d:label]
                assert_equal [dict get $i revocations] 0 "$what: after [expr {$n - 1}] failed spot checks"
                assert_equal [dict get $i audit-errors] [expr {$n - 1}] $what
                set label [lindex $failing [expr {($n - 1) % [llength $failing]}]]
                assert_equal [lc_exact_ask d:label $label [lc_other $label]] AUDIT $what
            }
            # The gate stopped answering at once, forgot its model, and
            # starts over by itself.
            set i [r ginfo d:label]
            assert_equal [dict get $i revocations] 1 "$what: after $alarm failed spot checks"
            assert_equal [dict get $i state] COLD $what
            assert_equal [dict get $i generation] 0 $what
            assert_equal [dict get $i candidate] 0 $what
            assert_equal [dict get $i thresholds] {{} {}} $what
            assert_equal [dict get $i learned] 0 $what
            assert_equal [lc_exact_ask d:label ignore] ABSTAIN
            assert_equal [lc_get d:label learned] 1
            lc_assert_counters d:label
        }
    }

    test {gate lifecycle: a newer snapshot does not take over while it answers less than the current one} {
        # SPEC section 3: a newer snapshot takes over only when it has
        # proven thresholds of its own and answers more of the same
        # spot-checked requests than the current one. Here snapshot 1
        # answers both labels. For a while every request is "help":
        # snapshot 2 proves that it can answer those too, which is not
        # more than snapshot 1 does, and it has proven nothing about
        # "thanks". With AUDIT 1 every request is a spot check, and no
        # dice are involved.
        lc_seed 49
        lc_exact_gate c:less {ignore respond} AUDIT 1
        set need [lc_proof_needed 0.2 0.2 1 0 2]
        foreach label {ignore respond} {
            for {set n 0} {$n < $need} {incr n} {lc_exact_ask c:less $label}
        }
        set i [r ginfo c:less]
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i thresholds] {0.5 0.5}

        for {set n 1} {$n <= 100} {incr n} {
            assert_equal [lc_exact_ask c:less respond] AUDIT
        }
        set i [r ginfo c:less]
        assert_equal [dict get $i thresholds] {0.5 0.5} \
            "after 100 more requests for one label, snapshot [dict get $i generation] answers"
        assert_equal [dict get $i generation] 1
        # "thanks" is back, and it is answered as before.
        for {set n 1} {$n <= 50} {incr n} {
            assert_equal [lc_exact_ask c:less ignore] AUDIT "request $n for the other label"
        }
        assert_equal [lc_get c:less revocations] 0
        lc_assert_counters c:less
    }

    test {gate lifecycle: a newer snapshot takes over with the answer that makes it answer more than the current one, and not before} {
        # SPEC section 3: a newer snapshot takes over only when it has
        # proven thresholds of its own and answers more of the same
        # spot-checked requests than the current one. gate_core.h: each
        # snapshot is judged by the guess it recorded, the current one with
        # the thresholds it has now. Here the first snapshot has learned
        # one message, "thanks team", and takes "help team" for more of the
        # same, with less confidence. So it proves a threshold for "ignore"
        # that lies above the lowest one, and answers "thanks team" and
        # nothing else. With AUDIT 1 every request is a spot check, and no
        # dice are involved.
        lc_seed 54
        r config set gate.snapshot-examples 40
        r gcreate c:more ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 ignore respond
        for {set k 0} {$k < 40} {incr k} {
            r gadd c:more TEXT "thanks team" LABEL ignore
        }
        # One request, answered with the teacher's label. Returns the action.
        set ask {{text} {
            lassign [r gpredict c:more TEXT $text] action id
            if {$action eq "ACCEPT" || $id eq {}} {fail "gate c:more replied '$action' '$id' to '$text': [r ginfo c:more]"}
            assert_equal [r glearn c:more $id LABEL [expr {$text eq "thanks team" ? "ignore" : "respond"}]] 1
            return $action
        }}

        # Both messages in turns. The first snapshot needs 39 correct
        # answers for a threshold (lc_proof_needed): it has them with the
        # 39th "thanks team", for the thresholds no "help team" came up to.
        set need [lc_proof_needed 0.2 0.2 1 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get c:more generation] 0 "after [expr {$n - 1}] of $need correct answers"
            assert_equal [apply $ask "help team"] ABSTAIN
            assert_equal [apply $ask "thanks team"] ABSTAIN
        }
        set i [r ginfo c:more]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        lassign [dict get $i thresholds] t_ignore t_respond
        assert {$t_ignore > 0.5 && $t_ignore <= 0.999}
        assert_equal $t_respond {}
        # The model went on learning both messages, and the second
        # snapshot was taken with the answer that made the first take over.
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i calibration-samples] 0

        # Both messages in turns again, "thanks team" first. Snapshot 1
        # answers that one and leaves the other to the teacher. Snapshot 2
        # is right about both, and needs 44 correct answers for a
        # threshold.
        set need [lc_proof_needed 0.2 0.2 2 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [apply $ask "thanks team"] AUDIT
            set i [r ginfo c:more]
            assert_equal [dict get $i generation] 1 "after $n requests for each message, less one"
            assert_equal [dict get $i candidate] 2
            assert_equal [dict get $i calibration-samples] [expr {2 * $n - 1}]
            assert_equal [apply $ask "help team"] ABSTAIN
            if {$n < $need} {assert_equal [lc_get c:more generation] 1 "after $n requests for each message"}
        }
        # With its 44th "thanks team" snapshot 2 had a proven threshold of
        # its own: it would answer 44 of the 87 requests it was tested on,
        # the very ones snapshot 1 answers. That is not more. With its 44th
        # "help team" it answers all 88, and snapshot 1 still 44.
        set i [r ginfo c:more]
        assert_equal [dict get $i generation] 2
        assert_equal [dict get $i thresholds] {0.5 0.5}
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i revocations] 0
        # The gate answers what it answered before, and more.
        for {set n 0} {$n < 5} {incr n} {
            assert_equal [apply $ask "thanks team"] AUDIT
            assert_equal [apply $ask "help team"] AUDIT
        }
        lc_assert_counters c:more
    }

    test {gate lifecycle: a snapshot that leaves to the teacher what the gate may answer is dropped, also while another one answers} {
        # SPEC section 5, snapshot dropped: after 10 times the proof a
        # flawless threshold needs, per label; requests it left to the
        # teacher although their label is one the gate may answer count
        # like proof. gate_core.h: the requests it could have been tested
        # on, which is every request while nothing answers and the
        # spot-checked share afterwards.
        #
        # Snapshot 1 knows that "thanks" is to be ignored, and answers.
        # Then somebody teaches the gate that "thanks" must be escalated,
        # which this gate never answers, and snapshot 2 is taken of that.
        # The teacher goes on saying "ignore". The gate is paused, so that
        # every request goes to the teacher and has an id: the half of them
        # that were drawn as spot checks are the ones snapshot 2 could have
        # been tested on, and GINFO counts them (audit-samples).
        lc_seed 55
        r config set gate.snapshot-examples 200
        r gcreate c:shadow ALPHA 0.2 DELTA 0.2 AUDIT 0.5 CLASSES 2 ignore escalate ALWAYSDEFER 1 escalate
        for {set k 0} {$k < 200} {incr k} {
            r gadd c:shadow TEXT thanks LABEL ignore
        }
        # One label may be answered: 35 correct answers.
        set need [lc_proof_needed 0.2 0.2 1 0 1]
        for {set n 0} {$n < $need} {incr n} {
            lassign [r gpredict c:shadow TEXT thanks] action id
            assert_equal $action ABSTAIN
            assert_equal [r glearn c:shadow $id LABEL ignore] 1
        }
        assert_equal [lc_get c:shadow state] ACTIVE
        assert_equal [lc_get c:shadow candidate] 0
        assert_equal [r gpause c:shadow] OK
        for {set k 0} {$k < 165} {incr k} {
            r gadd c:shadow TEXT thanks LABEL escalate
        }
        set i [r ginfo c:shadow]
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i learned] 400

        # Snapshot 2 is given 10 x 1 x 41 = 410 of them, and is dropped
        # with the answer of the 410th. (On the way, 40 requests are asked
        # whose answers are slow: they come when snapshot 2 is gone.)
        set budget [lc_snapshot_budget 0.2 0.2 2 1]
        assert_equal $budget 410
        set dropped 0
        set slow {}
        for {set n 1} {$n <= 2000 && !$dropped} {incr n} {
            lassign [r gpredict c:shadow TEXT thanks] action id
            assert_equal $action ABSTAIN
            assert {$id ne {}}
            if {$n > 300 && $n <= 340} {
                lappend slow $id
                continue
            }
            assert_equal [r glearn c:shadow $id LABEL ignore] 1
            set i [r ginfo c:shadow]
            # It guesses a label that is never tested: no proof.
            if {[dict get $i candidate] == 2} {assert_equal [dict get $i calibration-samples] 0}
            if {[dict get $i audit-samples] < $budget} {
                assert_equal [dict get $i candidate] 2 \
                    "request $n: snapshot 2 was dropped after [dict get $i audit-samples] spot-checked requests of $budget"
            } else {
                assert_equal [dict get $i candidate] 3 \
                    "request $n: snapshot 2 is still tested after [dict get $i audit-samples] spot-checked requests"
                set dropped 1
            }
        }
        assert {$dropped}
        # About every other request was drawn: 410 of some 820.
        assert_range $n 750 1000
        set i [r ginfo c:shadow]
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i state] PAUSED
        assert_equal [dict get $i revocations] 0

        # Snapshot 3 was taken with that answer. It has learned from the
        # teacher again, says "ignore", and is tested on the spot-checked
        # requests: 44 correct answers prove it (lc_proof_needed), but it
        # answers nothing snapshot 1 does not answer, so it does not take
        # over, and it is dropped with its 10 x 1 x 44 = 440th piece of
        # proof. The 40 slow answers arrive now. They are answers to
        # requests snapshot 3 never saw: they do not use up its budget.
        foreach id $slow {
            assert_equal [r glearn c:shadow $id LABEL ignore] 1
        }
        set budget [lc_snapshot_budget 0.2 0.2 3 1]
        assert_equal $budget 440
        set dropped 0
        for {set n 1} {$n <= 2000 && !$dropped} {incr n} {
            lassign [r gpredict c:shadow TEXT thanks] action id
            assert_equal $action ABSTAIN
            assert_equal [r glearn c:shadow $id LABEL ignore] 1
            set i [r ginfo c:shadow]
            assert_equal [dict get $i generation] 1
            if {[dict get $i candidate] == 3} {
                assert_lessthan [dict get $i calibration-samples] $budget
            } else {
                assert_equal [dict get $i candidate] 4 "request $n"
                # The last thing GINFO showed of snapshot 3.
                assert_equal $samples [expr {$budget - 1}] \
                    "snapshot 3 was dropped after $samples pieces of proof"
                set dropped 1
            }
            set samples [dict get $i calibration-samples]
        }
        assert {$dropped}
        lc_assert_counters c:shadow
    }

    test {gate lifecycle: a newer snapshot takes over while the gate keeps answering, and the gate answers no less for it} {
        lc_seed 44
        lc_srand 44
        r config set gate.snapshot-examples 150
        r gcreate c:next ALPHA 0.2 DELTA 0.2 AUDIT 0.5 CLASSES 2 ignore respond
        # The history knows one label well and the other hardly at all: the
        # first snapshot is sure of itself on "ignore" only. The model goes
        # on learning from the traffic, where both labels are as frequent.
        for {set k 0} {$k < 150} {incr k} {
            set label [expr {$k < 10 ? "respond" : "ignore"}]
            r gadd c:next TEXT [lc_message $label] LABEL $label
        }
        set generation 0
        set takeovers 0
        set compared 0
        set accepts 0
        set wrong 0
        set answering 0
        set recent {}        ;# the last 100 requests: 1 if the gate could answer
        set after_left 0     ;# requests still to come of the 100 after a takeover
        for {set n 1} {$n <= 3000} {incr n} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:next TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:next $arg LABEL $label
            }
            set answered [expr {$action ne "ABSTAIN"}]
            if {$after_left > 0} {
                incr after_sum $answered
                if {[incr after_left -1] == 0} {
                    # SPEC section 3: a newer snapshot takes over only when
                    # it answers more of the same spot-checked requests
                    # than the current one. The traffic does not change
                    # here, so what the gate answers of the next 100
                    # requests is no less than what it answered of the last
                    # 100 (two shares of 100 requests each: a tenth is more
                    # than chance moves their difference).
                    assert_morethan_equal [expr {$after_sum / 100.0}] [expr {$before_share - 0.1}] \
                        "snapshot $generation took over at request [expr {$n - 100}]"
                    incr compared
                }
            }
            lappend recent $answered
            if {[llength $recent] > 100} {set recent [lrange $recent 1 end]}
            set i [r ginfo c:next]
            if {$answering} {
                # Only a drift alarm or a GRESET stops a gate that answers:
                # a change of snapshot does not.
                assert_equal [dict get $i state] ACTIVE "request $n"
            } elseif {[dict get $i state] eq "ACTIVE"} {
                set answering 1
            }
            if {[dict get $i generation] != $generation} {
                # A snapshot takes over under a higher number, with proven
                # thresholds of its own.
                assert_morethan [dict get $i generation] $generation
                assert {[lsearch -exact -not [dict get $i thresholds] {}] >= 0}
                if {$generation != 0} {
                    incr takeovers
                    set before_share [expr {[tcl::mathop::+ {*}$recent] / double([llength $recent])}]
                    set after_left 100
                    set after_sum 0
                }
                set generation [dict get $i generation]
            }
            if {[dict get $i candidate] != 0} {
                assert_morethan [dict get $i candidate] $generation
            }
            if {$compared > 0 && $after_left == 0 && [lc_all_proven c:next]} break
        }
        assert_morethan $takeovers 0
        assert_morethan $compared 0
        assert {[lc_all_proven c:next]}
        set i [r ginfo c:next]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i revocations] 0
        assert {$wrong <= 0.2 * $accepts}
        lc_assert_counters c:next
    }

    test {gate lifecycle: requests for an ALWAYSDEFER label do not use up a snapshot} {
        # SPEC section 3: a snapshot that has not taken over after a
        # generous amount of proof is dropped. A request whose label is
        # never tested is proof of nothing, however many of them there are.
        lc_seed 45
        r config set gate.snapshot-examples 1
        r gcreate c:only ALPHA 0.2 DELTA 0.2 CLASSES 2 respond escalate ALWAYSDEFER 1 escalate
        assert_equal [r gadd c:only TEXT "the site is down" LABEL escalate] 1
        assert_equal [lc_get c:only candidate] 1
        for {set n 1} {$n <= 1000} {incr n} {
            lassign [r gpredict c:only TEXT "the site is down"] action id
            assert_equal $action ABSTAIN
            if {$id ne {}} {assert_equal [r glearn c:only $id LABEL escalate] 1}
            if {$n % 100 == 0} {
                assert_equal [lc_get c:only candidate] 1 \
                    "after $n requests for a label that is never tested"
            }
        }
        assert_equal [lc_get c:only state] CALIBRATING
        assert_equal [lc_get c:only calibration-samples] 0
        assert_equal [lc_get c:only learned] 1001
    }

    test {gate lifecycle: a snapshot that only knows an ALWAYSDEFER label is dropped and the next one is tried} {
        # SPEC section 3: a snapshot that has not taken over after a
        # generous amount of proof is dropped and the next one is tried;
        # section 5: after 10 times the proof a flawless threshold needs,
        # per label. Here the history comes sorted by label, the label that
        # is never answered first, and the first snapshot is taken when it
        # has seen nothing else. It says "escalate" to every message, which
        # is proof of nothing. The model has long learned the other two
        # labels: the same history in turns (lc_exact_gate) makes a gate
        # that answers both after 39 requests each. A gate must not wait
        # for ever for a snapshot that cannot prove anything: SPEC section
        # 5, requests it left to the teacher although their label is one
        # the gate may answer count like proof.
        lc_seed 47
        set labels {ignore respond escalate}
        r config set gate.snapshot-examples 50
        r gcreate c:stale ALPHA 0.2 DELTA 0.2 CLASSES 3 {*}$labels ALWAYSDEFER 1 escalate
        foreach label {escalate ignore respond} {
            for {set k 0} {$k < 50} {incr k} {
                r gadd c:stale TEXT $::lc_one($label) LABEL $label
            }
        }
        set i [r ginfo c:stale]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 1
        assert_equal [dict get $i learned] 150

        # Two requests in three are for a label the gate may answer, the
        # third for "escalate". The first snapshot is dropped with the
        # 10 x 2 x 39 = 780th answer it left to the teacher although the
        # gate may give it, which is the answer of request 1170, and not
        # one answer earlier. The second snapshot is taken with that answer
        # and is never wrong: it needs 44 correct answers for a label
        # (lc_proof_needed), and "respond" gets its 44th with request 1300.
        assert_equal [lc_snapshot_budget 0.2 0.2 1 2] 780
        assert_equal [lc_proof_needed 0.2 0.2 2 0 2] 44
        set accepts 0
        set wrong 0
        for {set n 1} {$n <= 4000 && $accepts < 20} {incr n} {
            set label [lindex $labels [expr {$n % 3}]]
            lassign [r gpredict c:stale TEXT $::lc_one($label)] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                assert_equal [r glearn c:stale $arg LABEL $label] 1
            }
            if {$n == 1169 || $n == 1170 || $n == 1299 || $n == 1300} {
                set i [r ginfo c:stale]
                assert_equal [dict get $i candidate] [expr {$n == 1169 ? 1 : $n == 1300 ? 3 : 2}] "after request $n"
                assert_equal [dict get $i generation] [expr {$n == 1300 ? 2 : 0}] "after request $n"
                assert_equal [dict get $i state] [expr {$n == 1300 ? "ACTIVE" : "CALIBRATING"}] "after request $n"
                assert_equal [dict get $i accepted] 0
            }
        }
        set i [r ginfo c:stale]
        assert_equal [dict get $i state] ACTIVE \
            "after [dict get $i requests] requests and [dict get $i learned] examples snapshot\
             [dict get $i candidate] is still being tested, with [dict get $i calibration-samples] pieces of proof"
        assert_equal [dict get $i generation] 2
        assert_equal $accepts 20
        assert_equal $wrong 0
        lc_assert_counters c:stale
    }

    test {gate lifecycle: a gate proves its labels when most requests carry an ALWAYSDEFER label} {
        # 96 requests in 100 are for a label the gate must never answer.
        # The two labels it may answer are as learnable as ever, and by the
        # end of this run each of them has had some 120 requests: a proof
        # needs 39 correct answers (lc_proof_needed 0.2 0.2 1 0 2). Requests
        # of a label that is never tested prove nothing, so they cannot be
        # the "generous amount of proof" after which a snapshot is dropped
        # (SPEC section 3).
        lc_seed 42
        lc_srand 42
        set labels {ignore respond escalate}
        r config set gate.snapshot-examples 300
        r gcreate c:rare ALPHA 0.2 DELTA 0.2 CLASSES 3 {*}$labels ALWAYSDEFER 1 escalate
        lc_history c:rare $labels 300
        set accepts 0
        set wrong 0
        for {set n 1} {$n <= 6000} {incr n} {
            if {[lc_rand 100] < 96} {
                set label escalate
            } else {
                set label [lc_pick {ignore respond}]
            }
            lassign [r gpredict c:rare TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                assert {$arg ne "escalate"}
                incr accepts
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:rare $arg LABEL $label
            }
            if {$n % 50 == 0 && [lc_all_proven c:rare]} break
        }
        set i [r ginfo c:rare]
        assert_equal [dict get $i state] ACTIVE "after $n requests, candidate [dict get $i candidate]"
        assert {[lc_all_proven c:rare]}
        assert {$wrong <= 0.2 * $accepts}
    }

    foreach count {32 64} {
        test "gate lifecycle: a gate with $count evenly used labels becomes ACTIVE on a learnable problem" {
            # SPEC section 2 allows 2 to 64 labels. Here every label has two
            # words of its own and one request in $count, and the model is
            # taught 30 examples of each before its first snapshot. Every
            # threshold is a test of its own, and there are 32 x $count of
            # them: a label needs 53 correct answers for its proof with 32
            # labels and 56 with 64 (lc_proof_needed), about 1700 and 3600
            # requests in all. The snapshot is given 10 times that much.
            lc_seed 43
            lc_srand 43
            set labels {}
            for {set k 0} {$k < $count} {incr k} {lappend labels topic$k}
            r config set gate.snapshot-examples [expr {30 * $count}]
            r del c:many
            r gcreate c:many ALPHA 0.2 DELTA 0.2 CLASSES $count {*}$labels
            for {set k 0} {$k < 30 * $count} {incr k} {
                set topic [expr {$k % $count}]
                r gadd c:many TEXT [lc_topic_message $topic] LABEL topic$topic
            }
            assert_equal [lc_get c:many state] CALIBRATING
            set need [lc_proof_needed 0.2 0.2 1 0 $count]
            set accepts 0
            set wrong 0
            set first 0
            unset -nocomplain seen_topic
            for {set n 1} {$n <= 125 * $count} {incr n} {
                set topic [lc_rand $count]
                incr seen_topic($topic)
                lassign [r gpredict c:many TEXT [lc_topic_message $topic]] action arg
                if {$action eq "ACCEPT"} {
                    incr accepts
                    if {$arg ne "topic$topic"} {incr wrong}
                    # No label is answered before it had the requests its
                    # proof needs.
                    if {!$first} {
                        set first $n
                        set answered [string range $arg 5 end]
                        assert {[info exists seen_topic($answered)] && $seen_topic($answered) >= $need}
                    }
                } elseif {$arg ne {}} {
                    r glearn c:many $arg LABEL topic$topic
                }
                if {$accepts >= 100} break
            }
            set i [r ginfo c:many]
            assert_equal [dict get $i state] ACTIVE "after $n requests, candidate [dict get $i candidate]"
            assert_equal [dict get $i generation] 1
            assert_equal $accepts 100
            assert {$wrong <= 0.2 * $accepts}
            assert_equal [dict get $i revocations] 0
            lc_assert_counters c:many
        }
    }
}

# ============================= The client loop ============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: with the default settings a classifier wants 1000 examples and then its proof} {
        # Nothing was configured on this server. SPEC section 5: 1,000
        # examples, and a feedback timeout of one minute.
        assert_equal [r config get gate.snapshot-examples] {gate.snapshot-examples 1000}
        assert_equal [r config get gate.feedback-timeout] {gate.feedback-timeout 60000}
        lc_seed 50
        lc_srand 50
        r gcreate c:default ALPHA 0.05 CLASSES 2 ignore respond
        set i [r ginfo c:default]
        assert {[dict get $i alpha] == 0.05}
        assert {[dict get $i delta] == 0.01}
        assert {[dict get $i audit] == 0.05}

        set wrong 0
        set by_gate 0
        set first_by_gate 0
        for {set n 1} {$n <= 4000} {incr n} {
            set label [lc_pick {ignore respond}]
            set calls $::lc_teacher_calls
            set decision [lc_decide c:default [lc_message $label]]
            if {$::lc_teacher_calls == $calls} {
                incr by_gate
                if {$decision ne $label} {incr wrong}
                if {!$first_by_gate} {set first_by_gate $n}
            }
            if {$n == 999 || $n == 1000} {
                # SPEC section 3: the first snapshot after 1,000 examples.
                set i [r ginfo c:default]
                assert_equal [dict get $i learned] $n
                assert_equal [dict get $i state] [expr {$n == 999 ? "COLD" : "CALIBRATING"}]
                assert_equal [dict get $i candidate] [expr {$n == 999 ? 0 : 1}]
            }
        }
        set i [r ginfo c:default]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert {[lc_all_proven c:default]}
        # The first snapshot is taken with the 1,000th example, and a label
        # is proven by correct answers with that label: at an error rate
        # of 0.05, against the share of DELTA = 0.01 that one of 2 x 32
        # thresholds of the first snapshot gets, that takes 235 of them
        # (lc_proof_needed). No request was answered before that.
        assert_equal [lc_proof_needed 0.05 0.01 1 0 2] 235
        assert_morethan $first_by_gate [expr {1000 + 235}]
        assert_morethan $by_gate 1000
        assert_equal [dict get $i accepted] $by_gate
        assert {$wrong <= 0.05 * $by_gate}
        assert_equal [dict get $i requests] 4000
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i revocations] 0
        lc_assert_counters c:default
    }

    foreach proto {2 3} {
        if {$proto == 3 && [lsearch $::denytags "resp3"] >= 0} continue
        if {$proto == 2 && $::force_resp3} continue

        test "gate lifecycle: the client loop of SPEC section 2 for 2000 requests (RESP$proto)" {
            r hello $proto
            lc_seed 51
            lc_srand 51
            set labels {ignore respond}
            r config set gate.snapshot-examples 200
            assert_equal [r gcreate loop:$proto ALPHA 0.2 DELTA 0.2 CLASSES 2 {*}$labels] OK
            lc_history loop:$proto $labels 200

            set ::lc_teacher_calls 0
            set wrong 0
            for {set n 0} {$n < 2000} {incr n} {
                set label [lc_pick $labels]
                set decision [lc_decide loop:$proto [lc_message $label]]
                assert {$decision in $labels}
                if {$decision ne $label} {incr wrong}
            }

            set i [r ginfo loop:$proto]
            assert_equal [dict size $i] 23
            assert_equal [dict get $i state] ACTIVE
            assert_equal [dict get $i requests] 2000
            # The teacher was called for every ABSTAIN and every AUDIT and
            # for nothing else, and the gate spared most of the calls.
            assert_equal [expr {[dict get $i audited] + [dict get $i abstained]}] $::lc_teacher_calls
            assert_equal [dict get $i accepted] [expr {2000 - $::lc_teacher_calls}]
            assert_lessthan $::lc_teacher_calls 1000
            # Every id was used, at once: nothing waits, nothing was lost,
            # and every teacher answer taught the model.
            assert_equal [dict get $i pending] 0
            assert_equal [dict get $i missing] 0
            assert_equal [dict get $i learned] [expr {200 + $::lc_teacher_calls}]
            assert_equal [dict get $i revocations] 0
            # What the caller got is what the teacher would have said.
            assert {$wrong <= 0.2 * [dict get $i accepted]}
            lc_assert_counters loop:$proto
            r hello [expr {$::force_resp3 ? 3 : 2}]
        }
    }

    test {gate lifecycle: the client loop of a score gate} {
        lc_seed 52
        lc_srand 52
        r gcreate loop:score ALPHA 0.2 DELTA 0.2 SCORE
        # A cache: the cheap result is right when the similarity is high.
        set teacher_calls 0
        set wrong 0
        for {set n 0} {$n < 2000} {incr n} {
            set similarity [expr {[lc_rand 1000] / 1000.0}]
            set cheap_is_right [expr {$similarity >= 0.6 ? 1 : ([lc_rand 100] < 30)}]
            lassign [r gpredict loop:score SCORE $similarity] action arg
            if {$action eq "ACCEPT"} {
                assert_equal $arg {}
                if {!$cheap_is_right} {incr wrong}
                continue
            }
            incr teacher_calls
            if {$arg ne {}} {
                r glearn loop:score $arg CORRECT $cheap_is_right
            }
        }
        set i [r ginfo loop:score]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i accepted] [expr {2000 - $teacher_calls}]
        assert_morethan [dict get $i accepted] 200
        assert_lessthan $teacher_calls 2000
        # The cheap result is wrong 70 times in 100 below 0.6: the gate
        # found a threshold that keeps most of that out.
        set thr [lindex [dict get $i thresholds] 0]
        assert {$thr > 0.3 && $thr < 0.998}
        assert {$wrong <= 0.2 * [dict get $i accepted]}
        assert_equal [dict get $i pending] 0
        assert_equal [dict get $i missing] 0
        lc_assert_counters loop:score
    }

    test {gate lifecycle: the client loop with a teacher that is sometimes slow and sometimes fails} {
        # The loop of SPEC section 2 for 4000 requests, as many clients at
        # once would run it. Of 100 calls of the teacher 3 fail (README:
        # "If the teacher fails, send nothing") and 12 are slow: their
        # answer comes 20, 90 or 400 milliseconds later, that is in time,
        # after proof has moved on (a quarter of the feedback timeout is 60
        # milliseconds here), or after the feedback timeout itself. The
        # teacher says the other label one time in twenty (lc_noisy).
        #
        # How many answers come too late depends on how fast this test
        # runs. What is asserted does not.
        lc_seed 53
        lc_srand 53
        set timeout 240
        r config set gate.snapshot-examples 200
        lc_gcreate_timeout $timeout loop:slow ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        for {set k 0} {$k < 200} {incr k} {
            set label [lc_pick {ignore respond}]
            r gadd loop:slow TEXT [lc_message $label] LABEL [lc_noisy $label]
        }

        set in_flight {}    ;# slow teacher calls: when the answer comes, when the gate was asked, id, label
        set accepts 0
        set wrong 0
        set failed 0
        set used 0
        set refused 0
        for {set n 1} {$n <= 4000} {incr n} {
            # The slow answers that have come by now.
            set now [clock milliseconds]
            set later {}
            foreach call $in_flight {
                lassign $call due asked id label
                if {$due > $now} {
                    lappend later $call
                    continue
                }
                set reply [r glearn loop:slow $id LABEL $label]
                set age [expr {[clock milliseconds] - $asked}]
                if {$reply == 1} {
                    incr used
                } else {
                    # GLEARN replies 0 to an id that is too old, and to
                    # no other id of this loop.
                    incr refused
                    assert_equal $reply 0
                    if {$age < $timeout - 5} {fail "request $n: an answer was refused $age ms after its GPREDICT"}
                }
            }
            set in_flight $later

            set label [lc_pick {ignore respond}]
            set text [lc_message $label]
            set teacher [lc_noisy $label]
            set fate [lc_rand 100]
            set delay [lc_pick {20 90 400}]

            # action, arg = r.execute_command("GPREDICT", key, "TEXT", text)
            set asked [clock milliseconds]
            lassign [r gpredict loop:slow TEXT $text] action arg
            # if action == b"ACCEPT": return arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $teacher} {incr wrong}
                continue
            }
            # label = teacher(text)
            if {$fate < 3} {
                incr failed
                continue
            }
            # if arg is not None: r.execute_command("GLEARN", key, arg, "LABEL", label)
            if {$arg ne {}} {
                if {$fate < 15} {
                    lappend in_flight [list [expr {[clock milliseconds] + $delay}] $asked $arg $teacher]
                } else {
                    set reply [r glearn loop:slow $arg LABEL $teacher]
                    if {$reply == 1} {
                        incr used
                    } elseif {[clock milliseconds] - $asked < $timeout - 5} {
                        fail "request $n: its answer was refused at once"
                    } else {
                        incr refused
                    }
                }
            }
            # Time goes by.
            if {$n % 25 == 0} {after 1}
        }

        set i [r ginfo loop:slow]
        # The gate did its job: it answers, most of the time.
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i accepted] $accepts
        assert_morethan $accepts 1000
        # The promise: of the answers the caller got from the gate, fewer
        # than ALPHA are not what the teacher would have said. (About one
        # in twenty: as often as the teacher disagrees with itself.)
        assert_morethan $wrong 0
        assert {$wrong < 0.2 * $accepts}
        # Lost answers were counted as wrong guesses, not as failed spot
        # checks: nothing was revoked and nothing forgotten. Every answer
        # that was used taught the model.
        assert_equal [dict get $i revocations] 0
        assert_equal [dict get $i learned] [expr {200 + $used}]
        assert_morethan $failed 0
        assert_morethan [dict get $i missing] 0
        assert_lessthan_equal [dict get $i missing] [expr {$failed + $refused + [llength $in_flight]}]
        assert_lessthan_equal [dict get $i pending] [expr {$failed + [llength $in_flight]}]
        assert_lessthan_equal [dict get $i audit-errors] [dict get $i audit-samples]
        lc_assert_counters loop:slow
    }
}
