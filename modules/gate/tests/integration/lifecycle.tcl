# Gate data type: what a gate does over its life, through its commands only.
#
# Every test states the contract of modules/gate/SPEC.md. Where a number is
# asserted exactly, the comment next to it says which sentence of the
# specification it follows from.
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
    assert_equal [dict get $i requests] $sum "requests of $key = accepted + audited + abstained"
}

# 1 if every label the gate may answer has a threshold.
proc lc_all_proven {key} {
    set i [r ginfo $key]
    foreach alpha [dict get $i alphas] threshold [dict get $i thresholds] {
        if {$alpha != 0 && $threshold eq {}} {return 0}
    }
    return 1
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

# SPEC section 3 (Proof, Budget) and section 5 (betting fractions, threshold
# order of a score gate): the number of correct answers in a row, after
# 'errors' wrong ones, that prove a threshold for snapshot number
# 'generation' of a score gate.
#
# A threshold is tested by betting. The wealth starts at 1; a bet stakes a
# fraction c of the largest stake that cannot go broke, 1 / (1 - alpha), so
# a correct answer multiplies its wealth by 1 + c * alpha / (1 - alpha) and
# a wrong one by 1 - c; the four fractions carry equal weight. The threshold
# is proven once the wealth reaches 1 / share, where the share is what the
# snapshot may spend, DELTA / (g (g + 1)), split over the 32 thresholds a
# score gate tests one by one.
proc lc_proof_needed {alpha delta generation {errors 0}} {
    set bar [expr {$generation * ($generation + 1) * 32.0 / $delta}]
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

# ============================== Score gates ===============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a score gate starts its first test at GCREATE} {
        lc_seed 1
        assert_equal OK [r gcreate s:new ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal gate-type [r type s:new]

        set i [r ginfo s:new]
        assert_equal score [dict get $i kind]
        assert_equal CALIBRATING [dict get $i state]
        assert {[dict get $i alpha] == 0.2}
        assert {[dict get $i delta] == 0.2}
        assert {[dict get $i audit] == 0.05}
        assert_equal 0 [llength [dict get $i classes]]
        assert_equal 1 [llength [dict get $i alphas]]
        assert {[lindex [dict get $i alphas] 0] == 0.2}
        assert_equal 1 [llength [dict get $i thresholds]]
        assert_equal {} [lindex [dict get $i thresholds] 0]
        assert_equal 0 [dict get $i generation]
        assert_equal 1 [dict get $i candidate]
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal 0 [dict get $i $field] $field
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
        assert_equal 35 $need
        set seen [dict create]
        for {set n 1} {$n <= $need} {incr n} {
            foreach {score correct} {0.9 1 0.3 0} {
                if {$n == $need && $score == 0.3} break
                assert_equal CALIBRATING [lc_get s:warm state] "before answer $n at $score"
                lassign [r gpredict s:warm SCORE $score] action id
                assert_equal ABSTAIN $action
                assert {$id ne {}}
                assert {![dict exists $seen $id]}
                dict set seen $id 1
                assert_equal 1 [r glearn s:warm $id CORRECT $correct]
            }
        }

        # Nothing was wasted while warming up: every answer counted as proof.
        set i [r ginfo s:warm]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 1 [dict get $i generation]
        assert_equal 0 [dict get $i candidate]
        assert_equal [expr {2 * $need - 1}] [dict get $i calibration-samples]
        assert_equal [expr {2 * $need - 1}] [dict get $i requests]
        assert_equal [expr {2 * $need - 1}] [dict get $i abstained]
        assert_equal 0 [dict get $i accepted]
        assert_equal 0 [dict get $i audited]
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [dict get $i missing]
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
                assert_equal 1 [r glearn s:bet $id CORRECT 0]
            }
            set need [lc_proof_needed $alpha $delta 1 $errors]
            for {set n 1} {$n <= $need} {incr n} {
                assert_equal CALIBRATING [lc_get s:bet state] \
                    "$options, $errors wrong: after [expr {$n - 1}] of $need correct answers"
                lassign [r gpredict s:bet SCORE 0.9] action id
                assert_equal ABSTAIN $action
                assert_equal 1 [r glearn s:bet $id CORRECT 1]
            }
            assert_equal ACTIVE [lc_get s:bet state] "$options, $errors wrong: after $need correct answers"
            assert_equal [expr {$errors + $need}] [lc_get s:bet calibration-samples]
        }
    }

    test {gate lifecycle: an ACTIVE score gate answers from its threshold up and abstains below it} {
        lc_seed 4
        lc_gate_between s:thr
        set i [r ginfo s:thr]
        assert_equal 1 [dict get $i generation]
        assert_equal 0 [dict get $i candidate]

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
                    assert_equal {} $id
                } else {
                    assert_equal AUDIT $action "score $score"
                    assert {$id ne {}}
                    assert_equal 1 [r glearn s:thr $id CORRECT 1]
                }
            }
        }
        foreach score {0.33 0.3 0.1 0} {
            for {set k 0} {$k < 25} {incr k} {
                lassign [r gpredict s:thr SCORE $score] action id
                assert_equal ABSTAIN $action "score $score"
                if {$id ne {}} {assert_equal 1 [r glearn s:thr $id CORRECT 0]}
            }
        }

        # 100 requests the gate could answer and 100 it could not.
        set i [r ginfo s:thr]
        assert_equal ACTIVE [dict get $i state]
        assert_equal $thr [lindex [dict get $i thresholds] 0]
        assert_equal [expr {$asked + 200}] [dict get $i requests]
        assert_equal 100 [expr {[dict get $i accepted] + [dict get $i audited]}]
        assert_equal [expr {$asked + 100}] [dict get $i abstained]
        assert_morethan [dict get $i accepted] 0
        assert_range [dict get $i coverage] 0.45 0.55
        assert_equal 0 [dict get $i pending]
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
                assert_equal {} $id
                # Nothing waits for an answer and nothing was added to the
                # proof: the gate simply answered.
                set after [r ginfo s:acc]
                assert_equal [dict get $before pending] [dict get $after pending]
                assert_equal [dict get $before calibration-samples] [dict get $after calibration-samples]
                assert_equal [expr {[dict get $before accepted] + 1}] [dict get $after accepted]
                assert_equal [expr {[dict get $before requests] + 1}] [dict get $after requests]
            } else {
                assert_equal AUDIT $action
                assert_equal 1 [r glearn s:acc $id CORRECT 1]
            }
        }
        assert_equal 100 $accepts
        assert_equal ACTIVE [lc_get s:acc state]
        assert_equal 0 [lc_get s:acc pending]
        lc_assert_counters s:acc
    }

    test {gate lifecycle: with AUDIT 1 a gate never ACCEPTs, it replies AUDIT} {
        lc_seed 6
        lc_gate_between s:audit1 AUDIT 1
        assert {[lc_get s:audit1 audit] == 1}
        for {set k 0} {$k < 50} {incr k} {
            lassign [r gpredict s:audit1 SCORE 0.9] action id
            assert_equal AUDIT $action
            assert {$id ne {}}
            assert_equal 1 [r glearn s:audit1 $id CORRECT 1]
            assert_equal ABSTAIN [lc_score s:audit1 0.3 0]
        }
        set i [r ginfo s:audit1]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 0 [dict get $i accepted]
        assert_equal 50 [dict get $i audited]
        assert_equal 50 [dict get $i audit-samples]
        assert_equal 0 [dict get $i audit-errors]
        assert_equal 0 [dict get $i pending]
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
                assert_equal {} $id
            } elseif {$action eq "AUDIT"} {
                incr audits
                assert_equal 1 [r glearn s:share $id CORRECT 1]
            } else {
                fail "request $k: $action from a gate that answers every score"
            }
        }
        # 3000 draws at 0.1: 300 expected, with a standard deviation of 16.
        assert_range $audits 235 365
        set i [r ginfo s:share]
        assert_equal $accepts [dict get $i accepted]
        assert_equal $audits [dict get $i audited]
        assert_equal $audits [dict get $i audit-samples]
        assert_equal 0 [dict get $i audit-errors]
        assert_equal $asked [dict get $i abstained]
        assert {[dict get $i coverage] > 0.999}
        assert_equal 0 [dict get $i pending]
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
            assert_equal ABSTAIN $action
            if {$id eq {}} {
                incr without
            } else {
                incr with
                assert_equal 1 [r glearn s:low $id CORRECT 0]
            }
        }
        # 400 draws at 0.25: 100 expected, with a standard deviation of 9.
        assert_range $with 65 135
        assert_equal 400 [expr {$with + $without}]
        # The lower thresholds are still wrong half of the time: none of
        # them was proven, so the gate answers where it answered before.
        assert_equal $thr [lc_threshold s:low]
        assert_equal ACTIVE [lc_get s:low state]
        assert_equal 0 [lc_get s:low pending]
        lc_assert_counters s:low
    }

    test {gate lifecycle: a score gate keeps testing lower thresholds, its threshold only moves down} {
        lc_seed 9
        r gcreate s:down ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        # Two early requests at 0.5 on which the cheap result is wrong, then
        # a cheap result that is right at 0.9.
        for {set k 0} {$k < 2} {incr k} {
            lassign [r gpredict s:down SCORE 0.5] action id
            assert_equal 1 [r glearn s:down $id CORRECT 0]
        }
        lc_calibrate s:down
        set first [lc_threshold s:down]
        assert {$first > 0.5 && $first <= 0.9}
        assert_equal ABSTAIN [lc_score s:down 0.5 1]

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
        assert_equal ACTIVE [lc_get s:down state]
        assert_equal 1 [lc_get s:down generation]
        assert_equal 0 [lc_get s:down revocations]
        assert_morethan [lc_get s:down coverage] 0
        lc_assert_counters s:down
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
                assert_equal ABSTAIN $action
                lappend ids $id
            }
            assert_equal 120 [lc_get $key pending]
            for {set j 0} {$j < 120} {incr j} {
                switch $key {
                    f:fwd {set k $j}
                    f:rev {set k [expr {119 - $j}]}
                    f:mix {set k [expr {($j * 37 + 5) % 120}]}
                }
                assert_equal 1 [r glearn $key [lindex $ids $k] CORRECT [expr {$k % 2 ? 0 : 1}]]
            }
        }
        set fwd [r ginfo f:fwd]
        assert_equal ACTIVE [dict get $fwd state]
        assert_equal 1 [dict get $fwd generation]
        assert_equal 120 [dict get $fwd calibration-samples]
        assert_equal 0 [dict get $fwd pending]
        assert {[lindex [dict get $fwd thresholds] 0] > 0.3}
        foreach key {f:rev f:mix} {
            set other [r ginfo $key]
            foreach field {state thresholds generation candidate calibration-samples audit-samples
                           pending missing revocations requests abstained} {
                assert_equal [dict get $fwd $field] [dict get $other $field] "$field of $key"
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
        # needs, but none of it may be used before the oldest request.
        for {set k 59} {$k >= 1} {incr k -1} {
            assert_equal 1 [r glearn f:wait [lindex $ids $k] CORRECT 1]
        }
        assert_equal CALIBRATING [lc_get f:wait state]
        assert_equal 0 [lc_get f:wait calibration-samples]
        # An answer that waits for its turn is used: it cannot be given twice.
        assert_equal 0 [r glearn f:wait [lindex $ids 59] CORRECT 0]

        assert_equal 1 [r glearn f:wait [lindex $ids 0] CORRECT 1]
        set i [r ginfo f:wait]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 60 [dict get $i calibration-samples]
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [dict get $i missing]
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

        assert_equal 0 [r glearn f:a $other CORRECT 1]
        assert_equal 1 [r glearn f:a $first CORRECT 1]
        # Retrying is always safe, with the same answer or with another one.
        assert_equal 0 [r glearn f:a $first CORRECT 1]
        assert_equal 0 [r glearn f:a $first CORRECT 0]
        assert_equal 1 [lc_get f:a calibration-samples]
        assert_equal 1 [lc_get f:a pending]
        assert_equal 1 [r glearn f:a $second CORRECT 1]
        assert_equal 2 [lc_get f:a calibration-samples]
        assert_equal 0 [lc_get f:a pending]

        # The other gate saw none of this.
        assert_equal 1 [lc_get f:b pending]
        assert_equal 0 [lc_get f:b calibration-samples]
        assert_equal 1 [r glearn f:b $other CORRECT 1]
        assert_equal 1 [lc_get f:b calibration-samples]
    }

    test {gate lifecycle: pending counts the requests waiting for GLEARN and returns to 0} {
        lc_seed 14
        r gcreate f:pend ALPHA 0.2 DELTA 0.2 SCORE
        set ids {}
        for {set k 1} {$k <= 5} {incr k} {
            lassign [r gpredict f:pend SCORE 0.9] action id
            lappend ids $id
            assert_equal $k [lc_get f:pend pending]
        }
        set left 5
        foreach id $ids {
            assert_equal 1 [r glearn f:pend $id CORRECT 1]
            incr left -1
            assert_equal $left [lc_get f:pend pending]
        }

        # A classifier that is still learning waits for its labels too.
        lc_srand 14
        r config set gate.snapshot-examples 1000
        r gcreate f:cpend ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        set ids {}
        foreach label {ignore respond ignore} {
            lassign [r gpredict f:cpend TEXT [lc_message $label]] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        assert_equal 3 [lc_get f:cpend pending]
        set left 3
        foreach id [lreverse $ids] label {ignore respond ignore} {
            assert_equal 1 [r glearn f:cpend $id LABEL $label]
            incr left -1
            assert_equal $left [lc_get f:cpend pending]
        }
        assert_equal 3 [lc_get f:cpend learned]
    }

    test {gate lifecycle: a request never answered in time counts as missing and the gate keeps working} {
        lc_seed 15
        r config set gate.feedback-timeout 200
        r gcreate f:late ALPHA 0.2 DELTA 0.2 SCORE
        r config set gate.feedback-timeout 300000
        set lost {}
        for {set k 0} {$k < 3} {incr k} {
            lassign [r gpredict f:late SCORE 0.9] action id
            lappend lost $id
        }
        assert_equal 3 [lc_get f:late pending]
        after 260

        # Too old: the answer is refused.
        assert_equal 0 [r glearn f:late [lindex $lost 0] CORRECT 1]
        set i [r ginfo f:late]
        assert_equal 3 [dict get $i missing]
        assert_equal 0 [dict get $i pending]
        assert_equal CALIBRATING [dict get $i state]

        # The cautious reading: each of them counted as a wrong guess. The
        # proof takes longer than after a clean start, but it comes.
        set n 0
        while {[lc_get f:late state] ne "ACTIVE" && $n < 600} {
            incr n
            lc_score f:late 0.9 1
        }
        assert_equal ACTIVE [lc_get f:late state]
        assert_morethan_equal $n [lc_proof_needed 0.2 0.2 1 3]
        assert_morethan $n [lc_proof_needed 0.2 0.2 1]
        assert_morethan_equal [lc_get f:late missing] 3
        lc_assert_counters f:late
    }

    test {gate lifecycle: answers that waited behind a lost request are applied when it times out} {
        lc_seed 16
        r config set gate.feedback-timeout 200
        r gcreate f:held ALPHA 0.2 DELTA 0.2 SCORE
        r config set gate.feedback-timeout 300000
        lassign [r gpredict f:held SCORE 0.9] action lost
        # 60 answered requests behind one that never gets its answer.
        for {set k 0} {$k < 60} {incr k} {
            lc_score f:held 0.9 1
        }
        after 260

        # The lost request counts as a wrong guess, the 60 answers behind
        # it are applied, and that is proof enough: this very request is
        # answered by the gate.
        lassign [r gpredict f:held SCORE 0.9] action id
        assert {$action eq "ACCEPT" || $action eq "AUDIT"}
        if {$id ne {}} {r glearn f:held $id CORRECT 1}
        set i [r ginfo f:held]
        assert_equal ACTIVE [dict get $i state]
        assert_morethan_equal [dict get $i missing] 1
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [r glearn f:held $lost CORRECT 1]
        lc_assert_counters f:held
    }

    test {gate lifecycle: a gate whose spot checks are never answered stops answering by itself} {
        lc_seed 17
        r config set gate.feedback-timeout 200
        r gcreate f:deaf ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE
        r config set gate.feedback-timeout 300000
        lc_calibrate f:deaf
        # The feedback path breaks: ten spot checks get no answer.
        set ids {}
        for {set k 0} {$k < 10} {incr k} {
            lassign [r gpredict f:deaf SCORE 0.9] action id
            assert_equal AUDIT $action
            lappend ids $id
        }
        assert_equal ACTIVE [lc_get f:deaf state]
        after 260

        # Each lost spot check counts as a wrong guess (SPEC section 3), and
        # that many wrong guesses in a row are what the drift detector is
        # there for.
        lassign [r gpredict f:deaf SCORE 0.9] action id
        assert_equal ABSTAIN $action
        set i [r ginfo f:deaf]
        assert_equal 1 [dict get $i revocations]
        assert_equal 0 [dict get $i generation]
        assert_morethan_equal [dict get $i missing] 1
        assert {[dict get $i state] ne "ACTIVE"}
        # The answers that come back when the feedback path is repaired are
        # too old, and they are not proof for the new test.
        foreach old $ids {
            assert_equal 0 [r glearn f:deaf $old CORRECT 1]
        }
        assert_equal 0 [lc_get f:deaf calibration-samples]
        assert_equal 1 [lc_get f:deaf pending]
        lc_assert_counters f:deaf
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
        assert_equal 4096 [lc_get f:full pending]

        for {set k 0} {$k < 5} {incr k} {
            assert_equal {ABSTAIN {}} [r gpredict f:full SCORE 0.9]
        }
        set i [r ginfo f:full]
        assert_equal 4096 [dict get $i pending]
        assert_equal 4101 [dict get $i requests]
        assert_equal 4101 [dict get $i abstained]
        assert_equal CALIBRATING [dict get $i state]

        # The oldest answers arrive: there is room again.
        for {set k 0} {$k < 10} {incr k} {
            assert_equal 1 [r glearn f:full [lindex $ids $k] CORRECT 1]
        }
        assert_equal 4086 [lc_get f:full pending]
        lassign [r gpredict f:full SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert {$id ne {} && ![dict exists $seen $id]}
        assert_equal 4087 [lc_get f:full pending]
        lc_assert_counters f:full
    }

    test {gate lifecycle: a full gate does not ACCEPT, not even a perfect score} {
        lc_seed 19
        r gcreate f:busy ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate f:busy 1
        assert_equal ACTIVE [lc_get f:busy state]

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
        assert_equal {ABSTAIN {}} [list $action $id]
        assert_equal 4096 [llength $audits]
        assert_morethan $accepts 1000
        assert_equal 4096 [lc_get f:busy pending]
        assert_equal $accepts [lc_get f:busy accepted]

        for {set k 0} {$k < 50} {incr k} {
            assert_equal {ABSTAIN {}} [r gpredict f:busy SCORE 1]
        }
        assert_equal $accepts [lc_get f:busy accepted]
        assert_equal 4096 [lc_get f:busy audited]
        assert_equal 4096 [lc_get f:busy pending]

        # Some answers arrive: the gate answers again.
        for {set k 0} {$k < 100} {incr k} {
            assert_equal 1 [r glearn f:busy [lindex $audits $k] CORRECT 1]
        }
        assert_equal 3996 [lc_get f:busy pending]
        set again 0
        for {set k 0} {$k < 50} {incr k} {
            lassign [r gpredict f:busy SCORE 1] action id
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            if {$action eq "ACCEPT"} {incr again}
        }
        assert_morethan $again 0
        assert_equal [expr {$accepts + $again}] [lc_get f:busy accepted]
        lc_assert_counters f:busy
    }

    test {gate lifecycle: one lost answer does not fill a gate whose other requests are all answered} {
        # SPEC section 3: a gate holds at most 4096 requests waiting for
        # feedback, and lost feedback costs a little coverage instead of
        # shutting the gate. Here one request never gets its answer and
        # every other request is answered at once: at any moment one
        # request is waiting for feedback, not 4096.
        lc_seed 20
        r gcreate f:one ALPHA 0.2 DELTA 0.2 SCORE
        lassign [r gpredict f:one SCORE 0.9] action lost
        assert {$lost ne {}}
        for {set k 1} {$k <= 4500} {incr k} {
            lassign [r gpredict f:one SCORE 0.9] action id
            if {$action eq "ABSTAIN" && $id eq {}} {
                fail "request $k: the gate is full (pending [lc_get f:one pending]) although one request is waiting for its answer"
            }
            if {$id ne {}} {assert_equal 1 [r glearn f:one $id CORRECT 1]}
        }
        assert_lessthan_equal [lc_get f:one pending] 1
        lc_assert_counters f:one
    }
}

# ===================== GPAUSE, GRESUME, GRESET, drift =====================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a paused gate does not answer but keeps checking itself, GRESUME lets it answer} {
        lc_seed 21
        r gcreate p:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate p:s
        set before [r ginfo p:s]

        assert_equal OK [r gpause p:s]
        set i [r ginfo p:s]
        assert_equal PAUSED [dict get $i state]
        assert_equal 1 [dict get $i generation]
        assert_equal [dict get $before thresholds] [dict get $i thresholds]

        # Shadow mode: nothing is answered, but the requests drawn as spot
        # checks still want their answer, and their answers still count.
        set ids 0
        for {set k 0} {$k < 60} {incr k} {
            lassign [r gpredict p:s SCORE 0.9] action id
            assert {$action ne "ACCEPT"}
            if {$id ne {}} {
                incr ids
                assert_equal 1 [r glearn p:s $id CORRECT 1]
            }
        }
        assert_morethan $ids 0
        set i [r ginfo p:s]
        assert_equal PAUSED [dict get $i state]
        assert_equal [dict get $before accepted] [dict get $i accepted]
        assert_equal [expr {[dict get $before requests] + 60}] [dict get $i requests]
        assert_equal [expr {[dict get $before audit-samples] + $ids}] [dict get $i audit-samples]
        assert_equal 0 [dict get $i audit-errors]
        assert_equal 0 [dict get $i pending]
        # Pausing twice changes nothing.
        assert_equal OK [r gpause p:s]
        assert_equal PAUSED [lc_get p:s state]

        assert_equal OK [r gresume p:s]
        assert_equal ACTIVE [lc_get p:s state]
        set accepts 0
        for {set k 0} {$k < 60} {incr k} {
            set action [lc_score p:s 0.9 1]
            assert {$action eq "ACCEPT" || $action eq "AUDIT"}
            if {$action eq "ACCEPT"} {incr accepts}
        }
        assert_morethan $accepts 0
        assert_equal OK [r gresume p:s]
        assert_equal ACTIVE [lc_get p:s state]
        lc_assert_counters p:s
    }

    test {gate lifecycle: a paused calibrating gate proves itself and GRESUME makes it ACTIVE at once} {
        lc_seed 22
        r gcreate p:shadow ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal OK [r gpause p:shadow]
        set i [r ginfo p:shadow]
        assert_equal PAUSED [dict get $i state]
        assert_equal 1 [dict get $i candidate]
        assert_equal 0 [dict get $i generation]

        # Feedback is accepted as before, and the proof is the proof of a
        # gate that was never paused.
        set need [lc_proof_needed 0.2 0.2 1]
        for {set n 1} {$n <= $need} {incr n} {
            assert_equal {} [lc_threshold p:shadow] "after [expr {$n - 1}] answers"
            lassign [r gpredict p:shadow SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            assert_equal 1 [r glearn p:shadow $id CORRECT 1]
        }
        set i [r ginfo p:shadow]
        assert_equal PAUSED [dict get $i state]
        assert_equal 1 [dict get $i generation]
        assert {[lindex [dict get $i thresholds] 0] ne {}}
        assert_equal $need [dict get $i calibration-samples]

        # Proven, but still paused: nothing is answered.
        for {set k 0} {$k < 40} {incr k} {
            assert {[lc_score p:shadow 0.9 1] ne "ACCEPT"}
        }
        assert_equal 0 [lc_get p:shadow accepted]
        assert_equal PAUSED [lc_get p:shadow state]

        assert_equal OK [r gresume p:shadow]
        assert_equal ACTIVE [lc_get p:shadow state]
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
        assert_equal OK [r gpause p:c]
        assert_equal PAUSED [lc_get p:c state]
        for {set k 1} {$k <= 20} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict p:c TEXT [lc_message $label]] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [r glearn p:c $id LABEL $label]
            assert_equal $k [lc_get p:c learned]
        }
        # Its first snapshot was taken on time, pause or not.
        set i [r ginfo p:c]
        assert_equal PAUSED [dict get $i state]
        assert_equal 1 [dict get $i candidate]
        assert_equal OK [r gresume p:c]
        assert_equal CALIBRATING [lc_get p:c state]
    }

    test {gate lifecycle: GRESET takes a score gate back to the start, under a new snapshot number} {
        lc_seed 24
        r gcreate r:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        assert_equal [lc_proof_needed 0.2 0.2 1] [lc_calibrate r:s]
        # A spot check that is still waiting for its answer.
        set old {}
        for {set k 0} {$k < 100 && $old eq {}} {incr k} {
            lassign [r gpredict r:s SCORE 0.9] action old
        }
        assert {$old ne {}}
        set before [r ginfo r:s]
        assert_equal ACTIVE [dict get $before state]
        assert_equal 1 [dict get $before generation]
        assert_equal 1 [dict get $before pending]

        assert_equal OK [r greset r:s]
        set i [r ginfo r:s]
        assert_equal CALIBRATING [dict get $i state]
        assert_equal 0 [dict get $i generation]
        assert_equal {} [lindex [dict get $i thresholds] 0]
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal 0 [dict get $i $field] $field
        }
        assert {[dict get $i coverage] == 0}
        # The configuration is kept.
        foreach field {kind alpha delta audit classes alphas} {
            assert_equal [dict get $before $field] [dict get $i $field] $field
        }
        # A snapshot number is never reused.
        assert_equal 2 [dict get $i candidate]

        # Ids from before the reset are refused, and teach nothing.
        assert_equal 0 [r glearn r:s $old CORRECT 1]
        assert_equal 0 [lc_get r:s calibration-samples]

        # GRESET hands no budget back: snapshot 2 has a smaller share of
        # DELTA than snapshot 1 had, so its proof takes more answers.
        set second [lc_calibrate r:s]
        assert_equal [lc_proof_needed 0.2 0.2 2] $second
        assert_morethan $second [lc_proof_needed 0.2 0.2 1]
        assert_equal 2 [lc_get r:s generation]

        assert_equal OK [r greset r:s]
        assert_equal 3 [lc_get r:s candidate]
        assert_equal OK [r greset r:s]
        assert_equal 4 [lc_get r:s candidate]
        assert_equal [lc_proof_needed 0.2 0.2 4] [lc_calibrate r:s]
        assert_equal 4 [lc_get r:s generation]

        # The numbers and the budget belong to the key: a new key starts
        # from 1 again.
        r del r:s
        r gcreate r:s ALPHA 0.2 DELTA 0.2 SCORE
        assert_equal 1 [lc_get r:s candidate]
        assert_equal 0 [r glearn r:s $old CORRECT 1]
        assert_equal 0 [lc_get r:s calibration-samples]
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
        assert_equal CALIBRATING [dict get $before state]
        assert_equal 1 [dict get $before candidate]
        assert_equal 120 [dict get $before learned]
        assert_equal 1 [dict get $before pending]

        assert_equal OK [r greset r:c]
        set i [r ginfo r:c]
        assert_equal COLD [dict get $i state]
        assert_equal 0 [dict get $i generation]
        assert_equal 0 [dict get $i candidate]
        assert_equal {{} {}} [dict get $i thresholds]
        foreach field {learned requests accepted audited abstained audit-samples
                       audit-errors calibration-samples pending missing revocations} {
            assert_equal 0 [dict get $i $field] $field
        }
        foreach field {kind alpha delta audit classes alphas} {
            assert_equal [dict get $before $field] [dict get $i $field] $field
        }
        assert_equal 0 [r glearn r:c $old LABEL ignore]
        assert_equal 0 [lc_get r:c learned]

        # It learns from nothing again, and its next snapshot gets a new number.
        lc_history r:c {ignore respond} 99
        assert_equal COLD [lc_get r:c state]
        lc_history r:c {ignore respond} 1
        set i [r ginfo r:c]
        assert_equal CALIBRATING [dict get $i state]
        assert_equal 2 [dict get $i candidate]
        assert_equal 100 [dict get $i learned]
    }

    test {gate lifecycle: GRESET does not resume a paused gate} {
        # SPEC.md does not say what GRESET does to a pause. Answering was
        # switched off by an operator, and nothing a gate forgets switches
        # it back on.
        lc_seed 26
        r gcreate r:p ALPHA 0.2 DELTA 0.2 SCORE
        lc_calibrate r:p
        r gpause r:p
        assert_equal OK [r greset r:p]
        set i [r ginfo r:p]
        assert_equal PAUSED [dict get $i state]
        assert_equal 0 [dict get $i generation]
        assert_equal 2 [dict get $i candidate]
        r gresume r:p
        assert_equal CALIBRATING [lc_get r:p state]
    }

    test {gate lifecycle: what happens to one gate leaves another gate alone} {
        lc_seed 29
        r gcreate x:a ALPHA 0.2 DELTA 0.2 SCORE
        r gcreate x:b ALPHA 0.1 DELTA 0.05 AUDIT 0.5 SCORE
        lassign [r gpredict x:b SCORE 0.7] action id
        set b [r ginfo x:b]
        assert_equal 1 [dict get $b pending]

        lc_calibrate x:a
        for {set k 0} {$k < 50} {incr k} {
            lc_score x:a 0.9 1
        }
        assert_equal $b [r ginfo x:b]
        r gpause x:a
        assert_equal $b [r ginfo x:b]
        r greset x:a
        assert_equal $b [r ginfo x:b]
        r del x:a
        assert_equal $b [r ginfo x:b]
        assert_equal 1 [r glearn x:b $id CORRECT 1]
        assert_equal 1 [lc_get x:b calibration-samples]
    }

    test {gate lifecycle: a score gate whose spot checks start to fail stops answering and calibrates again} {
        lc_seed 27
        r gcreate d:s ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE
        lc_calibrate d:s
        for {set k 0} {$k < 40} {incr k} {
            lc_score d:s 0.9 1
        }
        assert_equal 0 [lc_get d:s audit-errors]

        # One wrong spot check is what an error rate of 0.2 allows.
        for {set k 0} {$k < 100} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "AUDIT"} {
                assert_equal 1 [r glearn d:s $id CORRECT 0]
                break
            }
        }
        for {set k 0} {$k < 40} {incr k} {
            lc_score d:s 0.9 1
        }
        set i [r ginfo d:s]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 1 [dict get $i audit-errors]
        assert_equal 0 [dict get $i revocations]

        # Now the cheap result is wrong every time.
        set wrong_audits 0
        set wrong_accepts 0
        for {set k 0} {$k < 300} {incr k} {
            lassign [r gpredict d:s SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr wrong_accepts
            } elseif {$action eq "AUDIT"} {
                incr wrong_audits
                assert_equal 1 [r glearn d:s $id CORRECT 0]
            } else {
                break
            }
        }
        # A gross failure is caught after a handful of spot checks, not
        # after one.
        assert_equal ABSTAIN $action
        assert_range $wrong_audits 2 10
        assert_lessthan $wrong_accepts 60
        set i [r ginfo d:s]
        assert_equal CALIBRATING [dict get $i state]
        assert_equal 1 [dict get $i revocations]
        assert_equal 0 [dict get $i generation]
        assert_equal 2 [dict get $i candidate]
        assert_equal {} [lindex [dict get $i thresholds] 0]
        assert {[dict get $i coverage] == 0}

        # It starts over by itself: that ABSTAIN was the first request of
        # the new test. The cheap result is right again.
        assert {$id ne {}}
        assert_equal 1 [r glearn d:s $id CORRECT 1]
        assert_equal [expr {[lc_proof_needed 0.2 0.2 2] - 1}] [lc_calibrate d:s]
        set i [r ginfo d:s]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 2 [dict get $i generation]
        assert_equal 1 [dict get $i revocations]
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
                assert_equal 1 [r glearn d:lost $id CORRECT 0]
            } else {
                break
            }
        }
        assert_equal ABSTAIN $action \
            "still answering after $wrong_audits failed spot checks in a row and $wrong_accepts wrong answers"
        assert_lessthan_equal $wrong_audits 40
        assert_equal 1 [lc_get d:lost revocations]
        assert {[lc_get d:lost state] ne "ACTIVE"}
    }
}

# ============================ Classifier gates ============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: a classifier is COLD until it has learned gate.snapshot-examples examples} {
        lc_seed 31
        lc_srand 31
        r config set gate.snapshot-examples 20
        assert_equal OK [r gcreate c:cold ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond]
        set i [r ginfo c:cold]
        assert_equal classifier [dict get $i kind]
        assert_equal COLD [dict get $i state]
        assert_equal {ignore respond} [dict get $i classes]
        assert_equal 2 [llength [dict get $i alphas]]
        foreach alpha [dict get $i alphas] {assert {$alpha == 0.2}}
        assert_equal {{} {}} [dict get $i thresholds]
        assert_equal 0 [dict get $i generation]
        assert_equal 0 [dict get $i candidate]
        assert_equal 0 [dict get $i learned]

        # Ten examples from history: GADD replies with the number learned.
        for {set k 1} {$k <= 10} {incr k} {
            set label [lc_pick {ignore respond}]
            assert_equal $k [r gadd c:cold TEXT [lc_message $label] LABEL $label]
        }
        # Nine live requests: the gate knows nothing, asks, and learns.
        for {set k 11} {$k <= 19} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:cold TEXT [lc_message $label]] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            assert_equal 1 [r glearn c:cold $id LABEL $label]
            set i [r ginfo c:cold]
            assert_equal $k [dict get $i learned]
            assert_equal COLD [dict get $i state]
            assert_equal 0 [dict get $i candidate]
            assert_equal 0 [dict get $i calibration-samples]
        }
        # The twentieth example: the first snapshot is taken and tested.
        set label [lc_pick {ignore respond}]
        lassign [r gpredict c:cold TEXT [lc_message $label]] action id
        assert_equal 1 [r glearn c:cold $id LABEL $label]
        set i [r ginfo c:cold]
        assert_equal 20 [dict get $i learned]
        assert_equal CALIBRATING [dict get $i state]
        assert_equal 1 [dict get $i candidate]
        assert_equal 0 [dict get $i generation]
        assert_equal {{} {}} [dict get $i thresholds]

        # While it is tested every request goes to the teacher, and every
        # teacher answer counts as proof.
        for {set k 1} {$k <= 6} {incr k} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:cold TEXT [lc_message $label]] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
            assert_equal 1 [r glearn c:cold $id LABEL $label]
            assert_equal $k [lc_get c:cold calibration-samples]
        }
        assert_equal 26 [lc_get c:cold learned]
        lc_assert_counters c:cold
    }

    test {gate lifecycle: GADD makes the model better and never counts as proof} {
        lc_seed 32
        lc_srand 32
        r config set gate.snapshot-examples 20
        r gcreate c:add ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        lc_history c:add {ignore respond} 20
        assert_equal CALIBRATING [lc_get c:add state]
        lc_history c:add {ignore respond} 1000
        set i [r ginfo c:add]
        assert_equal 1020 [dict get $i learned]
        assert_equal CALIBRATING [dict get $i state]
        assert_equal 1 [dict get $i candidate]
        assert_equal 0 [dict get $i generation]
        assert_equal 0 [dict get $i calibration-samples]
        assert_equal 0 [dict get $i requests]
        assert_equal {{} {}} [dict get $i thresholds]
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
                assert_equal $label $decision
            }
            if {$n % 5 == 0} {
                set state [lc_get c:live state]
                if {[lindex $states end] ne $state} {lappend states $state}
                if {$n % 50 == 0 && [lc_all_proven c:live]} break
            }
        }
        # COLD until 20 examples, then CALIBRATING, then ACTIVE: never back.
        assert_equal {COLD CALIBRATING ACTIVE} $states
        assert {[lc_all_proven c:live]}
        set i [r ginfo c:live]
        assert_equal ACTIVE [dict get $i state]
        assert_morethan [dict get $i generation] 0
        foreach threshold [dict get $i thresholds] {
            # SPEC section 5: two labels, thresholds from 0.5 to 0.999.
            assert {$threshold >= 0.5 && $threshold <= 0.999}
        }
        assert_morethan $by_gate 0
        assert_equal $by_gate [dict get $i accepted]
        assert {$wrong <= 0.2 * $by_gate}
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [dict get $i missing]
        assert_equal 0 [dict get $i revocations]
        assert_equal $asked [dict get $i requests]
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
        assert_equal 0 $wrong
        lc_assert_counters c:new
    }

    test {gate lifecycle: an ALWAYSDEFER label is never returned by ACCEPT} {
        lc_seed 35
        lc_srand 35
        set labels {ignore respond escalate}
        lc_trained_gate c:defer $labels 300 2000 ALWAYSDEFER 1 escalate
        set i [r ginfo c:defer]
        assert_equal ACTIVE [dict get $i state]
        assert_equal $labels [dict get $i classes]
        lassign [dict get $i alphas] a_ignore a_respond a_escalate
        assert {$a_ignore == 0.2 && $a_respond == 0.2 && $a_escalate == 0}
        lassign [dict get $i thresholds] t_ignore t_respond t_escalate
        # SPEC section 5: three labels, thresholds from 1/3 to 0.999.
        assert {$t_ignore >= 0.333 && $t_ignore <= 0.999}
        assert {$t_respond >= 0.333 && $t_respond <= 0.999}
        assert_equal {} $t_escalate

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
                assert_equal 1 [r glearn c:defer $arg LABEL $label]
            }
        }
        assert_equal 0 $acc_by(escalate)
        assert_morethan $acc_by(ignore) [expr {$req_by(ignore) / 2}]
        assert_morethan $acc_by(respond) [expr {$req_by(respond) / 2}]
        assert {$wrong <= 0.2 * ($acc_by(ignore) + $acc_by(respond))}
        assert_equal {} [lindex [lc_get c:defer thresholds] 2]
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
                assert_equal 0 $first_at(respond)
                assert_equal ACTIVE [lc_get c:strict state]
            }
        }
        assert_morethan $first_at(ignore) 0
        assert_morethan $first_at(respond) $first_at(ignore)
        assert_morethan $req_then(respond) $req_then(ignore)
        # The best a bet at 0.05 can do is grow by 1 + 0.9 * 0.05 / 0.95 on
        # a correct answer, and no share of the budget is larger than DELTA:
        # fewer than log(1 / 0.2) / log(1.0474) = 35 answers prove nothing.
        assert_morethan_equal $req_then(respond) 35
        lc_assert_counters c:strict
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
            assert_equal $label [lsort -unique $answers] "mentioned $mentioned"
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
                assert_equal ABSTAIN $action
                lappend ids $id
                lappend labels $label
            }
            # The model keeps learning from every answer, in whatever order
            # they come. The proof uses the guesses recorded when the
            # requests were asked, in the order they were asked.
            for {set j 0} {$j < 150} {incr j} {
                set k [expr {$key eq "c:fwd" ? $j : 149 - $j}]
                assert_equal 1 [r glearn $key [lindex $ids $k] LABEL [lindex $labels $k]]
            }
        }
        set fwd [r ginfo c:fwd]
        set rev [r ginfo c:rev]
        assert_equal ACTIVE [dict get $fwd state]
        assert_equal 150 [dict get $fwd calibration-samples]
        assert_equal 550 [dict get $fwd learned]
        foreach field {state thresholds generation candidate calibration-samples learned
                       pending missing requests abstained} {
            assert_equal [dict get $fwd $field] [dict get $rev $field] $field
        }
    }

    test {gate lifecycle: a gate never becomes ACTIVE when the labels do not depend on the input} {
        lc_seed 39
        lc_srand 39
        r config set gate.snapshot-examples 100
        r gcreate c:coin ALPHA 0.2 DELTA 0.2 CLASSES 2 ignore respond
        for {set n 1} {$n <= 3000} {incr n} {
            # The teacher tosses a coin, whatever the message says.
            set text [lc_message [lc_pick {ignore respond}]]
            set label [lc_pick {ignore respond}]
            lassign [r gpredict c:coin TEXT $text] action id
            if {$action ne "ABSTAIN"} {fail "request $n: $action"}
            if {$id ne {}} {r glearn c:coin $id LABEL $label}
        }
        set i [r ginfo c:coin]
        assert {[dict get $i state] ne "ACTIVE"}
        assert_equal 0 [dict get $i generation]
        assert_equal {{} {}} [dict get $i thresholds]
        assert_equal 0 [dict get $i accepted]
        assert_equal 0 [dict get $i audited]
        assert_equal 3000 [dict get $i abstained]
        # A snapshot that proves nothing is dropped and the next one is
        # tried, under a new number.
        assert_morethan [dict get $i candidate] 1
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
        assert_equal ACTIVE [dict get $i state]
        assert_equal $accepts [dict get $i accepted]
        # The gate is useful, and it disagrees with the teacher about as
        # often as the teacher disagrees with itself: far less than ALPHA.
        assert_morethan $accepts 2000
        assert_morethan $wrong 0
        assert {$wrong < 0.2 * $accepts}
        # Its own spot checks tell the same story, and never raised an alarm.
        assert_morethan [dict get $i audit-samples] 0
        assert_morethan [dict get $i audit-errors] 0
        assert {[dict get $i audit-errors] < 0.2 * [dict get $i audit-samples]}
        assert_equal 0 [dict get $i revocations]
        assert_equal 0 [dict get $i pending]
        lc_assert_counters c:noisy
    }

    test {gate lifecycle: when the teacher changes its mind a classifier notices, forgets and learns again} {
        lc_seed 41
        lc_srand 41
        lc_trained_gate d:c {ignore respond} 300 1500 AUDIT 0.3
        assert_equal ACTIVE [lc_get d:c state]
        set first [lc_get d:c generation]

        # From now on the teacher answers the other label.
        set wrong 0
        for {set n 1} {$n <= 400} {incr n} {
            set label [lc_pick {ignore respond}]
            lassign [r gpredict d:c TEXT [lc_message $label]] action arg
            if {$action eq "ACCEPT"} {
                if {$arg ne [lc_other $label]} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn d:c $arg LABEL [lc_other $label]
            }
            if {[lc_get d:c revocations] > 0} break
        }
        # The gate stopped answering at once and forgot its model.
        set i [r ginfo d:c]
        assert_equal 1 [dict get $i revocations]
        assert_equal COLD [dict get $i state]
        assert_equal 0 [dict get $i generation]
        assert_equal 0 [dict get $i candidate]
        assert_equal {{} {}} [dict get $i thresholds]
        assert_equal 0 [dict get $i learned]
        assert_lessthan $wrong 100

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
        assert_equal 200 $accepts
        assert {$wrong <= 0.2 * $accepts}
        set i [r ginfo d:c]
        assert_equal ACTIVE [dict get $i state]
        assert_morethan [dict get $i generation] $first
        assert_equal 1 [dict get $i revocations]
        lc_assert_counters d:c
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
                assert_equal ACTIVE [dict get $i state] "request $n"
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
        assert_equal ACTIVE [dict get $i state]
        assert_equal 0 [dict get $i revocations]
        assert {$wrong <= 0.2 * $accepts}
        lc_assert_counters c:next
    }

    test {gate lifecycle: a gate proves its labels when most requests carry an ALWAYSDEFER label} {
        # 96 requests in 100 are for a label the gate must never answer.
        # The two labels it may answer are as learnable as ever, and by the
        # end of this run each of them has had some 120 requests: a proof
        # needs about 30. Requests of a label that is never tested prove
        # nothing, so they cannot be the "generous amount of proof" after
        # which a snapshot is dropped (SPEC section 3).
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
        assert_equal ACTIVE [dict get $i state] "after $n requests, candidate [dict get $i candidate]"
        assert {[lc_all_proven c:rare]}
        assert {$wrong <= 0.2 * $accepts}
    }

    test {gate lifecycle: a gate with 32 evenly used labels becomes ACTIVE on a learnable problem} {
        # SPEC section 2 allows 2 to 64 labels. Here every label has two
        # words of its own and one request in 32, and the model is taught
        # 30 examples of each before its first snapshot. A label needs some
        # 45 correct answers for its proof: about 1400 requests in all.
        lc_seed 43
        lc_srand 43
        set labels {}
        for {set k 0} {$k < 32} {incr k} {lappend labels topic$k}
        r config set gate.snapshot-examples 960
        r gcreate c:many ALPHA 0.2 DELTA 0.2 CLASSES 32 {*}$labels
        for {set k 0} {$k < 960} {incr k} {
            set topic [expr {$k % 32}]
            r gadd c:many TEXT "[lc_pick $::lc_fillers] alpha${topic}word beta${topic}term [lc_pick $::lc_fillers]" \
                LABEL topic$topic
        }
        assert_equal CALIBRATING [lc_get c:many state]
        set accepts 0
        set wrong 0
        for {set n 1} {$n <= 4000} {incr n} {
            set topic [lc_rand 32]
            set text "[lc_pick $::lc_fillers] alpha${topic}word beta${topic}term [lc_pick $::lc_fillers]"
            lassign [r gpredict c:many TEXT $text] action arg
            if {$action eq "ACCEPT"} {
                incr accepts
                if {$arg ne "topic$topic"} {incr wrong}
            } elseif {$arg ne {}} {
                r glearn c:many $arg LABEL topic$topic
            }
            if {$accepts >= 100} break
        }
        set i [r ginfo c:many]
        assert_equal ACTIVE [dict get $i state] "after $n requests, candidate [dict get $i candidate]"
        assert_equal 100 $accepts
        assert {$wrong <= 0.2 * $accepts}
    }
}

# ============================= The client loop ============================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {

    test {gate lifecycle: with the default settings a classifier wants 1000 examples and then its proof} {
        # Nothing was configured on this server.
        assert_equal {gate.snapshot-examples 1000} [r config get gate.snapshot-examples]
        assert_equal {gate.feedback-timeout 300000} [r config get gate.feedback-timeout]
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
                assert_equal $n [dict get $i learned]
                assert_equal [expr {$n == 999 ? "COLD" : "CALIBRATING"}] [dict get $i state]
                assert_equal [expr {$n == 999 ? 0 : 1}] [dict get $i candidate]
            }
        }
        set i [r ginfo c:default]
        assert_equal ACTIVE [dict get $i state]
        assert_equal 1 [dict get $i generation]
        assert {[lc_all_proven c:default]}
        # At an error rate of 0.05 a correct answer grows a bet by at most
        # 1 + 0.9 * 0.05 / 0.95, and no share of the budget is larger than
        # DELTA = 0.01: fewer than log(100) / log(1.0474) = 100 answers
        # prove nothing.
        assert_morethan $first_by_gate 1100
        assert_morethan $by_gate 1000
        assert_equal $by_gate [dict get $i accepted]
        assert {$wrong <= 0.05 * $by_gate}
        assert_equal 4000 [dict get $i requests]
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [dict get $i revocations]
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
            assert_equal OK [r gcreate loop:$proto ALPHA 0.2 DELTA 0.2 CLASSES 2 {*}$labels]
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
            assert_equal 23 [dict size $i]
            assert_equal ACTIVE [dict get $i state]
            assert_equal 2000 [dict get $i requests]
            # The teacher was called for every ABSTAIN and every AUDIT and
            # for nothing else, and the gate spared most of the calls.
            assert_equal $::lc_teacher_calls [expr {[dict get $i audited] + [dict get $i abstained]}]
            assert_equal [expr {2000 - $::lc_teacher_calls}] [dict get $i accepted]
            assert_lessthan $::lc_teacher_calls 1000
            # Every id was used, at once: nothing waits, nothing was lost,
            # and every teacher answer taught the model.
            assert_equal 0 [dict get $i pending]
            assert_equal 0 [dict get $i missing]
            assert_equal [expr {200 + $::lc_teacher_calls}] [dict get $i learned]
            assert_equal 0 [dict get $i revocations]
            # What the caller got is what the teacher would have said.
            assert {$wrong <= 0.2 * [dict get $i accepted]}
            lc_assert_counters loop:$proto
            r hello 2
        }
    }

    test {gate lifecycle: the client loop of a score gate} {
        r hello 2
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
                assert_equal {} $arg
                if {!$cheap_is_right} {incr wrong}
                continue
            }
            incr teacher_calls
            if {$arg ne {}} {
                r glearn loop:score $arg CORRECT $cheap_is_right
            }
        }
        set i [r ginfo loop:score]
        assert_equal ACTIVE [dict get $i state]
        assert_equal [expr {2000 - $teacher_calls}] [dict get $i accepted]
        assert_morethan [dict get $i accepted] 200
        assert_lessthan $teacher_calls 2000
        # The cheap result is wrong 70 times in 100 below 0.6: the gate
        # found a threshold that keeps most of that out.
        set thr [lindex [dict get $i thresholds] 0]
        assert {$thr > 0.3 && $thr < 0.998}
        assert {$wrong <= 0.2 * [dict get $i accepted]}
        assert_equal 0 [dict get $i pending]
        assert_equal 0 [dict get $i missing]
        lc_assert_counters loop:score
    }
}
