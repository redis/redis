# Gate data type: what a gate does over its life, through its commands only.
#
# Every test states the contract of modules/gate/SPEC.md. Where a number is
# asserted exactly, the comment next to it says which sentence of the
# specification it follows from. Two rules are stated more exactly in the
# file comments of gate_core.h and gate_stats.h than in SPEC.md, and the
# tests that lean on them say so: how many answered requests may wait
# behind a late one, and where the drift detector starts from.
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

# A score gate that answers every score: 'count' of its spot checks, none
# of them answered. Returns their ids. The requests the gate answers by
# itself come in between; any other reply fails the test.
proc lc_lose_audits {key count} {
    set ids {}
    for {set k 0} {[llength $ids] < $count} {incr k} {
        if {$k >= 100 * $count + 100} {fail "gate $key: no spot check in $k requests: [r ginfo $key]"}
        lassign [r gpredict $key SCORE 0.9] action id
        if {$action eq "AUDIT"} {
            lappend ids $id
        } elseif {$action ne "ACCEPT"} {
            fail "gate $key replied $action where it answers: [r ginfo $key]"
        }
    }
    return $ids
}

# A score gate that has stopped answering because its answers stopped
# coming (SPEC section 3, "No answers at all"): proven, and then 32 spot
# checks in a row were left without an answer until they were too old.
# Created with AUDIT 1, so that every request it could answer is a spot
# check. Returns the id given to the request that found the gate stopped;
# ::lc_outage_found is the time just before that request.
proc lc_outage {key} {
    lc_gcreate_timeout 300 $key ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
    lc_calibrate $key
    for {set k 0} {$k < 32} {incr k} {
        lassign [r gpredict $key SCORE 0.9] action id
        if {$action ne "AUDIT"} {fail "gate $key replied $action to request $k of the outage"}
    }
    after 350
    set ::lc_outage_found [clock milliseconds]
    lassign [r gpredict $key SCORE 0.9] action id
    if {$action ne "ABSTAIN" || $id eq {}} {
        fail "gate $key replied '$action' '$id' after 32 lost answers: [r ginfo $key]"
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

# No threshold is ever proven with fewer correct answers than this: what the
# boldest of the four bets would need if it held the whole stake.
proc lc_proof_floor {alpha delta generation {labels 1}} {
    set bar [expr {32.0 * $labels * $generation * ($generation + 1) / $delta}]
    return [expr {int(ceil(log($bar) / log(1 + 0.9 * $alpha / (1 - $alpha))))}]
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
#   spot checks  the answer of a request the gate could have answered
#                reaches the drift detector the moment it arrives:
#                likelihood ratios for 2x and 4x ALPHA (at most halfway to
#                1), each starting again from 1 when it has fallen below
#                (gate_stats.h), and an alarm when their average reaches
#                1,000

proc lc_model_init {alpha delta} {
    upvar #0 ::lc_m m
    array unset m
    array set m {policy -1 samples 0 audits 0 audit_errors 0 waiting 0 unanswered 0 next 1}
    set m(queue) {}
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
    set m(drift) {0.0 0.0}
    set m(drift_right) {}
    set m(drift_wrong) {}
    foreach times {2 4} {
        set p [expr {min($times * $alpha, (1 + $alpha) / 2.0)}]
        lappend m(drift_right) [expr {log((1 - $p) / (1 - $alpha))}]
        lappend m(drift_wrong) [expr {log($p / $alpha)}]
    }
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
    array set m [list score,$seq $score round,$seq $round answered,$seq 0 moved_on,$seq 0]
    set m(spot_check,$seq) [expr {$m(policy) >= 0 && $score >= $m(grid,$m(policy))}]
    lappend m(queue) $seq
    incr m(unanswered)
    return [list [expr {$m(spot_check,$seq) ? "AUDIT" : "ABSTAIN"}] $seq]
}

# The answer of a request. Returns 1 if the drift detector fires.
proc lc_model_learn {seq correct round} {
    upvar #0 ::lc_m m
    lc_model_settle $round
    set m(answered,$seq) 1
    set m(correct,$seq) $correct
    incr m(unanswered) -1
    if {$m(spot_check,$seq)} {
        incr m(audits)
        if {!$correct} {incr m(audit_errors)}
        set drift {}
        set average 0.0
        foreach w $m(drift) step [expr {$correct ? $m(drift_right) : $m(drift_wrong)}] {
            set w [expr {max($w, 0.0) + $step}]
            lappend drift $w
            set average [expr {$average + 0.5 * exp($w)}]
        }
        set m(drift) $drift
        if {$average >= 1000} {return 1}
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
    return 0
}

# The feedback timeout has passed for every request: those without an
# answer stay wrong guesses.
proc lc_model_give_up {} {
    upvar #0 ::lc_m m
    foreach seq $m(queue) {
        lc_model_count $seq [expr {$m(answered,$seq) ? !$m(correct,$seq) : 1}]
    }
    set m(queue) {}
    set m(waiting) 0
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

    test {gate lifecycle: whatever the order and the delay of the answers, a gate proves what the betting test of SPEC section 5 proves} {
        # 8 rounds of 40 requests to a gate with AUDIT 1, checked after
        # every command against lc_model_*, the specification written down
        # a second time. Of the answers, about 7 in 10 come within the round
        # (in any order), 2 in 10 one or two rounds later, when proof has
        # moved on without them, and one per round never. Between two
        # rounds more than a quarter of the feedback timeout goes by.
        lc_seed 67
        lc_srand 67
        set timeout 600
        set rounds 8
        lc_gcreate_timeout $timeout l:model ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        lc_model_init 0.2 0.2
        unset -nocomplain model_due model_id model_right model_round model_began
        # The model holds as long as the test keeps to its schedule: a round
        # takes less than a quarter of the timeout, and an answer comes
        # before the timeout of its request. If the machine was too slow
        # for that, the comparison ends there.
        set on_schedule 1
        set alarm 0
        set late_spot_checks 0
        for {set round 1} {$round <= $rounds && $on_schedule && !$alarm} {incr round} {
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
            set lost 0
            for {set k 0} {$k < [llength $todo] && !$alarm} {incr k} {
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
                    if {$fate >= 92 && $lost < 1} {
                        incr lost
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
                    ($step ne "ask" && $now - $model_began($model_round($seq)) >= $timeout - 10)} {
                    set on_schedule 0
                    break
                }
                if {$step eq "ask"} {
                    assert_equal $action $want $what
                    assert {$id ne {}}
                } else {
                    assert_equal $used 1 $what
                }
                if {$alarm} {
                    # Too many failed spot checks in a row: the gate has
                    # stopped answering and tests itself again.
                    assert_equal [lc_get l:model revocations] 1 $what
                    assert_equal [lc_get l:model generation] 0 $what
                } else {
                    lc_model_check l:model $what
                }
            }
            after [expr {$timeout / 4 + 20}]
        }
        if {$on_schedule} {
            # What these 320 requests are there for: the gate proved a
            # threshold, went on to prove the lowest one, and had spot
            # checks that failed and spot checks that were answered late.
            assert_equal $alarm 0
            assert_equal $::lc_m(policy) 0
            assert_morethan $::lc_m(audit_errors) 0
            assert_morethan $late_spot_checks 0
            assert_equal [lc_get l:model state] ACTIVE
            # The feedback timeout passes for every request that is still
            # without its answer: they stay wrong guesses, and are missing.
            after [expr {$timeout + 20}]
            assert_equal [r gresume l:model] OK
            lc_model_give_up
            lc_model_check l:model "after the feedback timeout"
            set i [r ginfo l:model]
            assert_equal [dict get $i pending] 0
            assert_equal [dict get $i missing] $::lc_m(unanswered)
            assert_morethan [dict get $i missing] 0
            assert_equal [dict get $i calibration-samples] [expr {40 * $rounds}]
            assert_equal [dict get $i revocations] 0
            assert_equal [dict get $i generation] 1
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
        # missing in a row before the gate stops, 32.
        lc_seed 17
        lc_gcreate_timeout 300 o:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        assert_equal [lc_calibrate o:s] [lc_proof_needed 0.2 0.2 1]
        assert {[lc_threshold o:s] == 0}

        # The feedback path breaks: no spot check gets its answer any more.
        set start [clock milliseconds]
        lc_lose_audits o:s 31
        assert_equal [lc_get o:s state] ACTIVE
        if {[clock milliseconds] - $start < 290} {assert_equal [lc_get o:s pending] 31}
        after 350

        # 31 requests have ended without an answer, one short of the limit.
        # The gate answers.
        lassign [r gpredict o:s SCORE 0.9] action id
        assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        set i [r ginfo o:s]
        assert_equal [dict get $i missing] 31
        assert_equal [dict get $i pending] [expr {$action eq "AUDIT"}]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i revocations] 0
        # A spot check without an answer is not a spot check that failed.
        assert_equal [dict get $i audit-samples] 0
        assert_equal [dict get $i audit-errors] 0

        # One more spot check is lost.
        if {$action ne "AUDIT"} {lc_lose_audits o:s 1}
        assert_equal [lc_get o:s state] ACTIVE
        after 350

        # 32 in a row, and no answer in between: the feedback path is
        # broken and the gate cannot check itself. It has stopped.
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
        # Until then it did answer.
        assert_morethan [dict get $i accepted] 0
        # Not one more request is answered, whatever its score.
        foreach score {1 0.9 0.5 0} {
            lassign [r gpredict o:s SCORE $score] action id
            assert_equal $action ABSTAIN "score $score"
        }
        lc_assert_counters o:s
    }

    test {gate lifecycle: during an outage every request gets an id and no snapshot is taken; the first answer starts a new test} {
        lc_seed 18
        set ids [list [lc_outage o:ids]]
        set i [r ginfo o:ids]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 32

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
        assert_equal [dict get $i missing] 32
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
        assert_equal [dict get $i missing] 32
        lassign [r gpredict o:ids SCORE 0.9] action id
        assert_equal $action AUDIT
        assert_equal [r glearn o:ids $id CORRECT 1] 1
        lc_assert_counters o:ids
    }

    test {gate lifecycle: fewer than 32 lost answers in a row do not stop a gate} {
        lc_seed 19
        lc_gcreate_timeout 300 o:burst ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate o:burst
        set thr [lc_threshold o:burst]
        for {set burst 1} {$burst <= 2} {incr burst} {
            lc_lose_audits o:burst 31
            after 350
            # An answer arrives: the feedback path works, and the run of
            # lost answers is over.
            set answered 0
            for {set k 0} {$k < 200 && !$answered} {incr k} {
                lassign [r gpredict o:burst SCORE 0.9] action id
                assert {$action eq "ACCEPT" || $action eq "AUDIT"}
                if {$action eq "AUDIT"} {
                    assert_equal [r glearn o:burst $id CORRECT 1] 1
                    set answered 1
                }
            }
            assert {$answered}
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
        assert_morethan [dict get $i accepted] 0
        lc_assert_counters o:burst
    }

    test {gate lifecycle: lost answers with answers in between do not stop a gate, however many of them are given up at once} {
        # SPEC section 3: when 32 requests in a row end without an answer
        # and no answer arrives in between, the feedback path is broken.
        # Here it works, nine times in ten: of 400 spot checks every tenth
        # gets no answer, the others get theirs at once. Then the gate is
        # left alone until the 40 are too old, so that the next command
        # gives them all up together (a gate does its bookkeeping when it
        # is written to). 40 lost answers, and never two in a row.
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

    test {gate lifecycle: a classifier whose answers stop keeps what it learned, and is tested again at the first answer} {
        lc_seed 21
        r config set gate.feedback-timeout 300
        lc_exact_gate o:c {ignore respond} AUDIT 1
        r config set gate.feedback-timeout 60000
        set need [lc_proof_needed 0.2 0.2 1 0 2]
        for {set n 0} {$n < $need} {incr n} {
            lc_exact_ask o:c ignore
            lc_exact_ask o:c respond
        }
        set i [r ginfo o:c]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 1
        assert_equal [dict get $i candidate] 2
        assert_equal [dict get $i thresholds] {0.5 0.5}
        set learned [dict get $i learned]
        assert_equal $learned [expr {40 + 2 * $need}]

        # 30 spot checks without an answer, then one with: fewer than 32
        # in a row.
        for {set k 0} {$k < 30} {incr k} {
            lassign [r gpredict o:c TEXT $::lc_one([lindex {ignore respond} [expr {$k % 2}]])] action id
            assert_equal $action AUDIT
        }
        after 350
        assert_equal [lc_exact_ask o:c ignore] AUDIT
        incr learned
        set i [r ginfo o:c]
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

        # The outage: 32 in a row.
        for {set k 0} {$k < 32} {incr k} {
            lassign [r gpredict o:c TEXT $::lc_one([lindex {ignore respond} [expr {$k % 2}]])] action id
            assert_equal $action AUDIT
        }
        after 350
        lassign [r gpredict o:c TEXT $::lc_one(ignore)] action id
        assert_equal $action ABSTAIN
        assert {$id ne {}}
        set i [r ginfo o:c]
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i missing] 62
        # The gate stopped answering. It did not forget its model.
        assert_equal [dict get $i learned] $learned

        # The first answer: the model the gate kept is frozen and tested
        # at once, under the next snapshot number.
        assert_equal [r glearn o:c $id LABEL ignore] 1
        incr learned
        set i [r ginfo o:c]
        assert_equal [dict get $i state] CALIBRATING
        assert_equal [dict get $i candidate] 3
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i learned] $learned
        # It knows the answers already: one label is proven by exactly the
        # correct answers snapshot 3 needs.
        set need [lc_proof_needed 0.2 0.2 3 0 2]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal [lc_get o:c state] CALIBRATING "after [expr {$n - 1}] of $need answers"
            assert_equal [lc_exact_ask o:c ignore] ABSTAIN
        }
        set i [r ginfo o:c]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i generation] 3
        assert_equal [dict get $i thresholds] {0.5 {}}
        assert_equal [dict get $i revocations] 1
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
        # Whatever a gate makes of 39 lost answers, 32 of them in a row:
        # it is the same 39 answers for both.
        set timeout 800
        foreach key {o:quiet o:written} {
            lc_seed 23
            lc_gcreate_timeout $timeout $key ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
            lc_calibrate $key
        }
        foreach key {o:quiet o:written} {
            for {set k 0} {$k < 40} {incr k} {
                lassign [r gpredict $key SCORE 0.9] action id
                assert_equal $action AUDIT
                if {$k == 32} {assert_equal [r glearn $key $id CORRECT 1] 1}
            }
        }
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
        for {set k 0} {$k < 40} {incr k} {
            lc_score d:s 0.9 1
        }
        assert_equal [lc_get d:s audit-errors] 0

        # One wrong spot check is what an error rate of 0.2 allows.
        for {set k 0} {$k < 100} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "AUDIT"} {
                assert_equal [r glearn d:s $id CORRECT 0] 1
                break
            }
        }
        for {set k 0} {$k < 40} {incr k} {
            lc_score d:s 0.9 1
        }
        set i [r ginfo d:s]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i audit-errors] 1
        assert_equal [dict get $i revocations] 0

        # Now the cheap result is wrong every time.
        set wrong_audits 0
        set wrong_accepts 0
        for {set k 0} {$k < 300} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr wrong_accepts
            } elseif {$action eq "AUDIT"} {
                incr wrong_audits
                assert_equal [r glearn d:s $id CORRECT 0] 1
            } else {
                break
            }
        }
        # A gross failure is caught after a handful of spot checks, not
        # after one. SPEC section 5 says after how many: the detector
        # multiplies two likelihood ratios, by 0.4 / 0.2 = 2 and by
        # 0.6 / 0.2 = 3 for a failed spot check (twice and four times the
        # error rate, the latter at most halfway to 1), and sounds the alarm
        # when their average reaches 1,000. What the gate did right before
        # does not count in its favour, so the ratios start from 1:
        # (2^6 + 3^6) / 2 = 396.5 and (2^7 + 3^7) / 2 = 1157.5.
        assert_equal $action ABSTAIN
        assert_equal $wrong_audits 7
        assert_lessthan $wrong_accepts 60
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
        r gcreate d:lost ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate d:lost
        for {set k 0} {$k < 40} {incr k} {
            lc_score d:lost 0.9 1
        }
        set lost {}
        for {set k 0} {$k < 100 && $lost eq {}} {incr k} {
            lassign [r gpredict d:lost SCORE 0.9] action lost
        }
        assert {$lost ne {}}
        set before [r ginfo d:lost]
        assert_equal [dict get $before audit-errors] 0

        # Now the cheap result is wrong every time, and every spot check
        # says so.
        set wrong_audits 0
        set wrong_accepts 0
        for {set k 0} {$k < 400} {incr k} {
            lassign [r gpredict d:lost SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr wrong_accepts
            } elseif {$action eq "AUDIT"} {
                incr wrong_audits
                assert_equal [r glearn d:lost $id CORRECT 0] 1
                if {$wrong_audits < 7} {
                    # As proof this answer waits for its turn behind the
                    # lost one (for 15 seconds, with the default timeout).
                    # The drift detector has heard of it already.
                    set i [r ginfo d:lost]
                    assert_equal [dict get $i state] ACTIVE
                    assert_equal [dict get $i audit-errors] $wrong_audits
                    assert_equal [dict get $i audit-samples] [expr {[dict get $before audit-samples] + $wrong_audits}]
                    assert_equal [dict get $i calibration-samples] [dict get $before calibration-samples]
                }
            } else {
                break
            }
        }
        assert_equal $action ABSTAIN \
            "still answering after $wrong_audits failed spot checks in a row and $wrong_accepts wrong answers"
        # Seven failed spot checks in a row raise the alarm, with or without
        # an older spot check that is still unanswered (see the test above).
        assert_equal $wrong_audits 7
        set i [r ginfo d:lost]
        assert_equal [dict get $i revocations] 1
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
        # saying that the cheap result was wrong.
        lc_seed 30
        set timeout 2000
        lc_gcreate_timeout $timeout d:late ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        lc_calibrate d:late
        set late {}
        for {set k 0} {$k < 7} {incr k} {
            lassign [r gpredict d:late SCORE 0.9] action id
            assert_equal $action AUDIT
            lappend late $id
        }
        assert_equal [lc_score d:late 0.9 1] AUDIT
        after [expr {$timeout / 4 + 50}]
        assert_equal [lc_score d:late 0.9 1] AUDIT
        set i [r ginfo d:late]
        assert_equal [dict get $i state] ACTIVE
        assert_equal [dict get $i pending] 7
        assert_equal [dict get $i audit-samples] 2
        assert_equal [dict get $i audit-errors] 0
        set samples [dict get $i calibration-samples]

        set failed 0
        foreach id $late {
            assert_equal [lc_get d:late state] ACTIVE "after $failed failed spot checks"
            assert_equal [r glearn d:late $id CORRECT 0] 1
            incr failed
            if {$failed < 7} {
                set i [r ginfo d:late]
                assert_equal [dict get $i audit-samples] [expr {2 + $failed}]
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
        # flawless threshold needs lc_proof_needed correct answers, and
        # could never do with fewer than lc_proof_floor: each snapshot
        # lasted between 10 x 2 times the one and 10 x 2 times the other,
        # and the next one was taken with the answer that ended it.
        assert_equal [llength $tried] 3
        assert_equal [lindex $tried 0] 100
        foreach snapshot {1 2} {
            set lasted [expr {[lindex $tried $snapshot] - [lindex $tried $snapshot-1]}]
            assert_range $lasted [expr {10 * 2 * [lc_proof_floor 0.2 0.2 $snapshot 2]}] \
                [expr {10 * 2 * [lc_proof_needed 0.2 0.2 $snapshot 0 2]}]
        }
        lc_assert_counters c:coin
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
        lc_seed 41
        lc_srand 41
        lc_trained_gate d:c {ignore respond} 300 1500 AUDIT 0.3
        assert_equal [lc_get d:c state] ACTIVE
        set first [lc_get d:c generation]

        set learned [lc_get d:c learned]
        assert_morethan $learned 300

        # From now on the teacher answers the other label.
        set wrong 0
        set failed 0
        for {set n 1} {$n <= 400} {incr n} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict d:c TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                if {$arg ne [lc_other $label]} {incr wrong}
            } elseif {$arg ne {}} {
                if {$action eq "AUDIT"} {incr failed}
                r glearn d:c $arg LABEL [lc_other $label]
            }
            if {[lc_get d:c revocations] > 0} break
            # Until the alarm nothing is forgotten.
            assert_morethan_equal [lc_get d:c learned] $learned
        }
        # The gate stopped answering at once and forgot its model.
        set i [r ginfo d:c]
        assert_equal [dict get $i revocations] 1
        assert_equal [dict get $i state] COLD
        assert_equal [dict get $i generation] 0
        assert_equal [dict get $i candidate] 0
        assert_equal [dict get $i thresholds] {{} {}}
        assert_equal [dict get $i learned] 0
        assert_lessthan $wrong 100
        # Every label has a detector of its own, and seven failed spot
        # checks in a row set one off (see the score gates above): with two
        # labels that is the 7th failure at the earliest and the 13th at the
        # latest.
        assert_range $failed 7 13

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
        # the error rate (at most halfway to 1), alarm at 1,000. At 0.2 a
        # failed spot check multiplies them by 2 and by 3, and the 7th in a
        # row takes their average to 1,000 (see the score gates). At 0.05
        # it multiplies them by 0.1 / 0.05 = 2 and by 0.2 / 0.05 = 4:
        # (2^5 + 4^5) / 2 = 528 and (2^6 + 4^6) / 2 = 2080, so the 6th
        # does. A failed spot check of one label says nothing about the
        # other: failing in turns, it takes the 7th of the first label,
        # which is the 13th in all.
        foreach {options failing alarm} {
            {}                           {ignore}          7
            {CLASSALPHA 1 respond 0.05}  {ignore}          7
            {CLASSALPHA 1 respond 0.05}  {respond}         6
            {}                           {ignore respond}  13
        } {
            lc_seed 48
            r del d:label
            lc_exact_gate d:label {ignore respond} AUDIT 1 {*}$options
            set what "$options, failing: $failing"
            foreach label {ignore respond} alpha [lc_get d:label alphas] {
                set need [lc_proof_needed $alpha 0.2 1 0 2]
                for {set n 0} {$n < $need} {incr n} {lc_exact_ask d:label $label}
            }
            # Both labels are answered, and the teacher agrees.
            for {set n 0} {$n < 2} {incr n} {
                assert_equal [lc_exact_ask d:label ignore] AUDIT
                assert_equal [lc_exact_ask d:label respond] AUDIT
            }
            set i [r ginfo d:label]
            assert_equal [dict get $i state] ACTIVE $what
            assert_equal [dict get $i generation] 1 $what
            assert_equal [dict get $i audit-samples] 4 $what
            assert_equal [dict get $i audit-errors] 0 $what
            assert_morethan [dict get $i learned] 100

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

    test {gate lifecycle: a newer snapshot takes over while the gate keeps answering} {
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
        set accepts 0
        set wrong 0
        set answering 0
        for {set n 1} {$n <= 3000} {incr n} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:next TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne $label} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:next $arg LABEL $label
            }
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
                if {$generation != 0} {incr takeovers}
                set generation [dict get $i generation]
            }
            if {[dict get $i candidate] != 0} {
                assert_morethan [dict get $i candidate] $generation
            }
            if {$takeovers > 0 && [lc_all_proven c:next]} break
        }
        assert_morethan $takeovers 0
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
        # for ever for a snapshot that cannot prove anything.
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

        # 4000 requests, two in three for a label the gate may answer: more
        # than three times the 10 x 2 x 39 pieces of proof after which a
        # snapshot that proved nothing is dropped.
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
        }
        set i [r ginfo c:stale]
        assert_equal [dict get $i state] ACTIVE \
            "after [dict get $i requests] requests and [dict get $i learned] examples snapshot\
             [dict get $i candidate] is still being tested, with [dict get $i calibration-samples] pieces of proof"
        assert_morethan [dict get $i generation] 1
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
