# Redis Gate: primaries, replicas and failover.
#
# The contract under test (SPEC.md, section 3: "Redis", "Late and lost
# answers", "No answers at all", "Replacing the answering snapshot",
# "Getting worse"; section 5; CHANGES.md):
#
#   * GPREDICT runs on the primary. Replicas and the AOF receive what actually
#     happened (the random draw, the clock, the recorded guess), never the
#     command, so they never re-roll dice. A replica therefore holds, for
#     every gate, exactly the state of its primary: DEBUG DIGEST-VALUE is the
#     same on both once the replica has caught up.
#   * An ACCEPT writes nothing.
#   * A gate does its bookkeeping when it is written to. Proof that moves on
#     without a late answer after a quarter of the feedback timeout, a request
#     given up when the feedback timeout is over, a gate that stops because 32
#     requests in a row ended without an answer or because it lost too many
#     of its spot checks: each of these happens inside whatever write command
#     comes next, at the clock of the primary, and reaches the replicas as
#     what happened: with the operation of that command or, when the command
#     replicates nothing by itself (an ACCEPT), as an operation of its own
#     (TICK), and only then.
#   * What a gate knows of its spot checks (the number each got when it was
#     asked, the disagreements and the lost answers of the blocks its drift
#     detector has open), how a newer snapshot compares with the answering
#     one, and what a snapshot left to the teacher is state like any other:
#     a replica holds it as its primary does, however it came by the gate
#     (the stream, a full synchronization, its AOF, a file), so that the
#     answer that stops the gate, or lets a snapshot take over, does so on
#     every node.
#   * After a failover Redis cannot show that nothing was lost, so a gate
#     keeps its model, drops its proof and its waiting requests, and proves
#     itself again by itself.
#
# Two nodes that agree may both be wrong: a replica runs the code of its
# primary. Wherever the outcome follows from SPEC.md whatever the dice were,
# the tests work it out by themselves (the right answers a threshold needs,
# the disagreements and lost answers that stop a gate, the proof after which
# a snapshot is dropped) and hold every node to it.
#
# The tests use real randomness (gate.test-seed 0) wherever the point is that
# realized values are replicated, and assert only what holds for any draw.
# gate.test-seed is used where exact outcomes are asserted.
#
# Tests in which time matters let real time pass (feedback timeouts of 100 to
# 4000 ms). What they assert either holds however long each step took, or is
# asserted only after the test has checked its own clock: a step that was
# meant to happen before a deadline and did not (a stalled machine) makes the
# test start over with fresh gates, a few times at most.
#
# Every test ends by checking that the replicas are alive, that they sent no
# error to their primary, and that none of them met an operation that "did
# not fit its gate" (logged, and counted as gate_apply_mismatches in INFO);
# the workloads check that after every phase. (Two tests send such
# operations on purpose, to servers of their own, and count them.)
#
# Run from the repository root:
#   ./runtest --single ../modules/gate/tests/integration/replication
# (a part of it: --tags gate:chain, gate:accept, gate:time, gate:whole,
# gate:middle, gate:failover, gate:clock, gate:writable, gate:drift or
# gate:edge)

set gatemodule [file normalize modules/gate/gate.so]

# ------------------------------------------------------------------ helpers

# DEBUG DIGEST-VALUE of a key that does not exist.
set ::gr_nodigest [string repeat 0 40]

# The request counters are kept by the primary only: an ACCEPT is not
# replicated, so a replica cannot know them.
set ::gr_primary_only {requests accepted audited abstained coverage}

# GINFO as a dict.
proc gr_info {client key} {
    set info [dict create]
    foreach {field value} [$client ginfo $key] {
        dict set info $field $value
    }
    return $info
}

proc gr_field {client key field} {
    dict get [gr_info $client $key] $field
}

# GINFO without the fields only a primary keeps.
proc gr_replicated_info {client key} {
    set info [gr_info $client $key]
    foreach field $::gr_primary_only {
        dict unset info $field
    }
    return $info
}

# GINFO for a failure message: never throws.
proc gr_describe {client key} {
    if {[catch {gr_info $client $key} info]} {
        return "(GINFO failed: $info)"
    }
    return $info
}

# 1 if the gate answers for no label at all.
proc gr_not_answering {info} {
    if {[dict get $info generation] != 0} {return 0}
    foreach threshold [dict get $info thresholds] {
        if {$threshold ne {}} {return 0}
    }
    return 1
}

# Every replica has applied the whole stream of its primary. A few quick
# looks come first: wait_for_ofs_sync sleeps 100 ms whenever it has to look
# twice, and a gate does not stand still while a test sleeps.
proc gr_sync {primary replicas} {
    for {set try 0} {$try < 150} {incr try} {
        set want [status $primary master_repl_offset]
        set behind 0
        foreach replica $replicas {
            if {[status $replica master_repl_offset] ne $want} {
                set behind 1
                break
            }
        }
        if {!$behind} break
        after 2
    }
    foreach replica $replicas {
        wait_for_ofs_sync $primary $replica
    }
}

# Every replica has applied the whole stream of its primary and holds, for
# each of 'keys', the gate the primary holds: same DEBUG DIGEST-VALUE.
proc gr_assert_same_digests {primary replicas keys {phase ""}} {
    gr_sync $primary $replicas
    set want [$primary debug digest-value {*}$keys]
    foreach key $keys digest $want {
        if {$digest eq $::gr_nodigest} {
            fail "$phase: there is no key $key on the primary"
        }
    }
    set n 0
    foreach replica $replicas {
        incr n
        set got [$replica debug digest-value {*}$keys]
        foreach key $keys w $want g $got {
            if {$w eq $g} continue
            set note ""
            if {![catch {gr_replicated_info $primary $key} p] &&
                ![catch {gr_replicated_info $replica $key} r] && $p eq $r} {
                set note "\n(GINFO shows the same on both: they differ in something it does not show, such as the time of a request or of a snapshot)"
            }
            fail "$phase: DEBUG DIGEST-VALUE $key on replica #$n differs from its primary\
                  \nprimary: [gr_describe $primary $key]\
                  \nreplica: [gr_describe $replica $key]$note"
        }
    }
}

# As above, and GINFO on every replica shows what GINFO shows on the
# primary, request counters aside.
proc gr_assert_same {primary replicas keys {phase ""}} {
    gr_assert_same_digests $primary $replicas $keys $phase
    foreach key $keys {
        set want [gr_replicated_info $primary $key]
        set n 0
        foreach replica $replicas {
            incr n
            assert_equal $want [gr_replicated_info $replica $key] \
                "$phase: GINFO $key on replica #$n (second) against its primary (first)"
        }
    }
}

# The whole dataset of every replica is the dataset of the primary: no key
# more, no key less, nothing different.
proc gr_assert_same_dataset {primary replicas {phase ""}} {
    gr_sync $primary $replicas
    set want [$primary debug digest]
    set n 0
    foreach replica $replicas {
        incr n
        assert_equal $want [$replica debug digest] "$phase: DEBUG DIGEST of replica #$n (second) against its primary (first)"
    }
}

# What GINFO shows of a gate from the moment its primary knows that the
# gate will drop its proof to the moment it has: everything but the request
# counters and the bytes.
proc gr_recovery_view {client key} {
    dict remove [gr_replicated_info $client $key] memory
}

# The stats of a command in INFO COMMANDSTATS, whatever the case it is
# reported in, or an empty string if the command was never called.
proc gr_cmdstat {client cmd} {
    set cmd [string map {. \\.} $cmd]
    if {[regexp -nocase -line "^cmdstat_$cmd:(.*?)\r?$" [$client info commandstats] -> stats]} {
        return $stats
    }
    return ""
}

# How many times a command was executed (not merely rejected).
proc gr_calls {client cmd} {
    if {[regexp {calls=(\d+)} [gr_cmdstat $client $cmd] -> calls]} {
        return $calls
    }
    return 0
}

# How many replicated operations a server has applied that did not fit
# their gates, since it started (CHANGES.md, section 8).
proc gr_mismatches {client} {
    if {![regexp {gate_apply_mismatches:(\d+)} [$client info gate] -> mismatches]} {
        fail "INFO does not show gate_apply_mismatches"
    }
    return $mismatches
}

proc gr_log_tail {log {lines 25}} {
    if {[catch {exec tail -n $lines $log} tail]} {
        return "(cannot read $log)"
    }
    return $tail
}

# The servers every test must leave alive and well: levels as 'srv' takes
# them. A replica that could not apply what its primary sent either dies
# (propagation-error-behavior panic) or logs and counts that an operation
# did not fit its gate.
set ::gr_watch {}
array set ::gr_misfits {}

proc gr_watch {args} {
    set ::gr_watch $args
}

proc gr_check_watched {} {
    foreach level $::gr_watch {
        set log [srv $level stdout]
        if {[catch {[srv $level client] ping} pong] || $pong ne "PONG"} {
            fail "the server at level $level does not answer PING ($pong); its log ends with:\n[gr_log_tail $log]"
        }
        set errors [status [srv $level client] unexpected_error_replies]
        if {$errors != 0} {
            fail "the server at level $level sent $errors error replies to its primary or to the AOF loader; its log ends with:\n[gr_log_tail $log]"
        }
        set misfits [count_message_lines $log "did not fit its gate"]
        set before 0
        if {[info exists ::gr_misfits($log)]} {
            set before $::gr_misfits($log)
        }
        set ::gr_misfits($log) $misfits
        if {$misfits > $before} {
            fail "the server at level $level logged that a replicated operation did not fit its gate"
        }
        # The log names the first few of them only. INFO counts them all,
        # since the server started (CHANGES.md, section 8).
        set mismatches [gr_mismatches [srv $level client]]
        if {$mismatches != 0} {
            fail "the server at level $level has applied $mismatches replicated operations that did not fit their gates (gate_apply_mismatches)"
        }
    }
}

# Run the body of a test. An error that is not a failed assertion (a dead
# server, an unexpected error reply) fails this test instead of aborting the
# whole suite, and every test ends by checking the watched servers. (A body
# may end early with 'return'.)
proc gr_body {body} {
    if {[catch {uplevel 1 $body} err] == 1} {
        if {[string match "assertion:*" $err]} {
            error $err
        }
        set note ""
        foreach level $::gr_watch {
            if {[catch {[srv $level client] ping}]} {
                append note "\nthe server at level $level does not answer PING; its log ends with:\n[gr_log_tail [srv $level stdout]]"
            }
        }
        fail "unexpected error: $err$note"
    }
    gr_check_watched
}

proc gr_test {name body} {
    uplevel 1 [list test $name [list gr_body $body]]
}

# A private, repeatable source of randomness for the workloads: the shape of
# a workload is the same in every run, only the dice of the server differ.
set ::gr_seed 20261128

proc gr_rand {n} {
    set ::gr_seed [expr {($::gr_seed * 1103515245 + 12345) & 0x7fffffff}]
    expr {($::gr_seed >> 8) % $n}
}

# Texts a classifier can tell apart: three words of the label's vocabulary.
set ::gr_words {
    spam    {winner prize money free click offer cheap pills casino bonus}
    ham     {meeting agenda report lunch tomorrow project review notes thanks team}
    billing {invoice refund charge payment receipt price discount card renewal bill}
    tech    {crash error login password install update timeout driver reboot kernel}
    other   {hello weather holiday music football movie birthday coffee garden travel}
    spam2   {lottery jackpot viagra unsubscribe deal urgent wire transfer crypto giveaway}
    ham2    {schedule deadline budget draft slides client contract feedback minutes roadmap}
}

proc gr_text {label n} {
    set words [dict get $::gr_words $label]
    set len [llength $words]
    return "[lindex $words [expr {$n % $len}]] [lindex $words [expr {($n * 7 + 3) % $len}]] [lindex $words [expr {($n * 3 + 1) % $len}]]"
}

# Long texts: 60 words of 63 letters out of a pool of 400. Each gives a
# classifier thousands of clues to keep while the request waits for its
# feedback. (The whole command stays under 4096 bytes: the test client
# sends a longer one in two writes, which costs 40 ms of TCP delay each.)
set ::gr_long_words {}

proc gr_long_text {n} {
    if {[llength $::gr_long_words] == 0} {
        for {set w 0} {$w < 400} {incr w} {
            set word ""
            for {set c 0} {$c < 63} {incr c} {
                append word [string index abcdefghijklmnopqrstuvwxyz [gr_rand 26]]
            }
            lappend ::gr_long_words $word
        }
    }
    set text ""
    for {set w 0} {$w < 60} {incr w} {
        append text [lindex $::gr_long_words [expr {($n * 67 + $w * 31 + $n * $w) % 400}]] " "
    }
    return $text
}

# One request to a score gate, answered at once by a teacher that agrees
# with the cheap result when the score is at least 'good'. Returns the
# action.
proc gr_score_request {client key score {good 0.7}} {
    lassign [$client gpredict $key SCORE $score] action id
    if {$action ne "ACCEPT" && $id ne {}} {
        assert_equal 1 [$client glearn $key $id CORRECT [expr {$score >= $good}]] "GLEARN $key $id"
    }
    return $action
}

# One request to a classifier gate, answered at once by a teacher that says
# 'label'. Returns the action.
proc gr_text_request {client key text label} {
    lassign [$client gpredict $key TEXT $text] action id
    if {$action eq "ACCEPT"} {
        return $action
    }
    if {$id ne {}} {
        assert_equal 1 [$client glearn $key $id LABEL $label] "GLEARN $key $id"
    }
    return $action
}

# Requests with these scores, in a cycle: the teacher agrees from 0.7 up.
set ::gr_scores {0.95 0.6 0.9 0.99 0.85 0.65 0.8 0.75 0.97 0.72}

# Send requests to a score gate until it answers (at most 'max' requests).
proc gr_score_until_active {client key {max 600} {good 0.7}} {
    for {set n 0} {$n < $max} {incr n} {
        if {[gr_field $client $key state] eq "ACTIVE"} {
            return $n
        }
        gr_score_request $client $key [lindex $::gr_scores [expr {$n % [llength $::gr_scores]}]] $good
    }
    fail "score gate $key is not ACTIVE after $max answered requests: [gr_describe $client $key]"
}

# Send requests to a classifier gate until it answers (at most 'max').
proc gr_text_until_active {client key labels {max 4000}} {
    for {set n 0} {$n < $max} {incr n} {
        if {[gr_field $client $key state] eq "ACTIVE"} {
            return $n
        }
        set label [lindex $labels [expr {$n % [llength $labels]}]]
        gr_text_request $client $key [gr_text $label $n] $label
    }
    fail "classifier gate $key is not ACTIVE after $max answered requests: [gr_describe $client $key]"
}

# A mixed workload. 'gates' is a list of {key kind labels lossy}: kind is
# score or text, labels are the classes of a classifier, and a lossy gate
# (one with a short feedback timeout) never gets the feedback of some of its
# requests. Feedback is sent out of order, some of it twice, some of it for
# ids no gate ever issued.
set ::gr_pending {}  ;# feedback not sent yet: {key id LABEL|CORRECT answer}
set ::gr_used {}     ;# feedback already sent once
array set ::gr_seen {ACCEPT 0 AUDIT 0 ABSTAIN 0 NOFEEDBACK 0 USED 0 UNUSED 0}

set ::gr_bogus_ids {
    nonsense 0123456789abcdef-7 ffffffffffffffff-18446744073709551615
    0000000000000000-0 FFFFFFFFFFFFFFFF-1 {} 0123456789abcdef-9223372036854775808
}

proc gr_workload_request {client gate} {
    lassign $gate key kind labels lossy
    if {$kind eq "score"} {
        set score [expr {[gr_rand 101] / 100.0}]
        set right [expr {$score >= 0.5}]
        if {[gr_rand 100] < 3} {
            set right [expr {!$right}]
        }
        set reply [$client gpredict $key SCORE $score]
        set answer [list CORRECT $right]
    } else {
        set label [lindex $labels [gr_rand [llength $labels]]]
        set text [gr_text $label [gr_rand 1000]]
        switch [gr_rand 8] {
            0 {
                set reply [$client gpredict $key TEXT $text FEATURES 2 urgent [expr {[gr_rand 21] / 10.0 - 1}] depth 0.25]
            }
            1 {
                # A long text with a link, a question and bytes that are not text.
                set text "$text? see https://example.com/[gr_rand 50] [string repeat "$text " 40]\x00\xff\r\n"
                set reply [$client gpredict $key TEXT $text]
            }
            default {
                set reply [$client gpredict $key TEXT $text]
            }
        }
        if {[gr_rand 100] < 3} {
            set label [lindex $labels [gr_rand [llength $labels]]]
        }
        set answer [list LABEL $label]
    }
    lassign $reply action id
    if {$action eq "ACCEPT"} {
        incr ::gr_seen(ACCEPT)
    } elseif {$id eq {}} {
        incr ::gr_seen(NOFEEDBACK)
    } else {
        incr ::gr_seen($action)
        if {!$lossy || [gr_rand 100] >= 4} {
            lappend ::gr_pending [list $key $id {*}$answer]
        }
    }
}

proc gr_workload {client gates nops} {
    for {set op 0} {$op < $nops} {incr op} {
        set gate [lindex $gates [gr_rand [llength $gates]]]
        lassign $gate key kind labels lossy
        set dice [gr_rand 100]
        set waiting [llength $::gr_pending]
        if {$waiting > 40 || ($waiting > 0 && $dice >= 45 && $dice < 80)} {
            # The feedback of a request picked at random: out of order.
            set i [gr_rand $waiting]
            set feedback [lindex $::gr_pending $i]
            set ::gr_pending [lreplace $::gr_pending $i $i]
            set used [$client glearn {*}$feedback]
            if {$used == 1} {
                incr ::gr_seen(USED)
            } elseif {$used == 0} {
                incr ::gr_seen(UNUSED)
            } else {
                fail "GLEARN $feedback replied '$used'"
            }
            lappend ::gr_used $feedback
            if {[llength $::gr_used] > 60} {
                set ::gr_used [lrange $::gr_used 30 end]
            }
        } elseif {$dice < 80} {
            gr_workload_request $client $gate
        } elseif {$dice < 88} {
            if {$kind eq "text"} {
                set label [lindex $labels [gr_rand [llength $labels]]]
                if {[gr_rand 3] == 0} {
                    $client gadd $key TEXT [gr_text $label [gr_rand 1000]] \
                        FEATURES 3 urgent 1 depth [expr {[gr_rand 9] / 4.0 - 1}] {a name} -0.5 LABEL $label
                } else {
                    $client gadd $key TEXT [gr_text $label [gr_rand 1000]] LABEL $label
                }
            } else {
                gr_workload_request $client $gate
            }
        } elseif {$dice < 92} {
            # The same feedback a second time.
            if {[llength $::gr_used] > 0} {
                set feedback [lindex $::gr_used [gr_rand [llength $::gr_used]]]
                assert_equal 0 [$client glearn {*}$feedback] "GLEARN $feedback, sent a second time"
            }
        } elseif {$dice < 95} {
            # An id this gate never issued.
            set id [lindex $::gr_bogus_ids [gr_rand [llength $::gr_bogus_ids]]]
            if {$kind eq "score"} {
                assert_equal 0 [$client glearn $key $id CORRECT 1] "GLEARN $key '$id'"
            } else {
                assert_equal 0 [$client glearn $key $id LABEL [lindex $labels 0]] "GLEARN $key '$id'"
            }
        } elseif {$dice < 97} {
            $client gpause $key
        } else {
            $client gresume $key
        }
    }
}

# The feedback the workload still owes, sent in request order: nothing it
# asked keeps waiting.
proc gr_workload_drain {client} {
    foreach feedback $::gr_pending {
        $client glearn {*}$feedback
    }
    set ::gr_pending {}
    set ::gr_used {}
}

# Forget what the workload knows about some keys (they were reset, deleted
# or recovered: their ids are void).
proc gr_workload_forget {keys} {
    foreach var {::gr_pending ::gr_used} {
        set kept {}
        foreach feedback [set $var] {
            if {[lsearch -exact $keys [lindex $feedback 0]] < 0} {
                lappend kept $feedback
            }
        }
        set $var $kept
    }
}

# Read what a (fake) replica received until 'marker' shows up.
proc gr_stream_until {repl marker} {
    set commands {}
    while 1 {
        set command [read_from_replication_stream $repl]
        if {$command eq ""} {
            fail "the replication stream ended before '$marker'; it carried: $commands"
        }
        if {$command eq $marker} {
            return $commands
        }
        lappend commands $command
    }
}

# Every command of a stream is the internal one, on one of 'keys', or part
# of the plumbing (SELECT, MULTI, EXEC). Returns how many internal commands.
proc gr_assert_stream_is_realized {commands keys} {
    set applied 0
    foreach command $commands {
        set name [lindex $command 0]
        if {$name eq "select" || $name eq "multi" || $name eq "exec"} continue
        if {$name ne "_gate.apply"} {
            fail "the replication stream carries '$name': [string range $command 0 200]"
        }
        if {[lsearch -exact $keys [lindex $command 1]] < 0} {
            fail "the replication stream touches the key '[lindex $command 1]': [string range $command 0 200]"
        }
        incr applied
    }
    return $applied
}

set ::gr_public_writes {gcreate gpredict glearn gadd gpause gresume greset}


# The operations a stream carried, in order, as {OPERATION key clock}: the
# commands must all be the internal one, on one of 'keys' (SELECT, MULTI and
# EXEC aside). The clock of a gate never runs backwards.
proc gr_stream_operations {commands keys} {
    set operations {}
    array set clock {}
    foreach command $commands {
        set name [lindex $command 0]
        if {$name eq "select" || $name eq "multi" || $name eq "exec"} continue
        if {$name ne "_gate.apply"} {
            fail "the replication stream carries '$name': [string range $command 0 200]"
        }
        lassign $command name key operation now
        if {[lsearch -exact $keys $key] < 0} {
            fail "the replication stream touches the key '$key': [string range $command 0 200]"
        }
        if {![string is wideinteger -strict $now]} {
            fail "an operation without a clock: [string range $command 0 200]"
        }
        if {[info exists clock($key)] && $now < $clock($key)} {
            fail "the clock of $key runs backwards in the stream ($clock($key), then $now): [string range $command 0 200]"
        }
        set clock($key) $now
        lappend operations [list $operation $key $now]
    }
    return $operations
}

# The end of a phase of a workload: every replica holds what its primary
# holds, is alive, and has not met an operation that did not fit its gate.
proc gr_phase {primary replicas keys phase} {
    gr_assert_same $primary $replicas $keys $phase
    gr_check_watched
}

# ----------------------------------------------------------------- the clock
#
# A gate counts time on its primary, when it is written to: from the wall
# clock at its first use, by the time that has passed since (CHANGES.md,
# section 8). A test knows the age of a request within the time its own
# commands took: the request was made between the moments the test read its
# clock before and after the GPREDICT.

# Sleep until everything that was made before the moment 'since' (a
# [clock milliseconds]) is at least 'age' ms old, for any clock that is not
# off by more than a few ms.
proc gr_wait_age {since age} {
    set wait [expr {$since + $age + 3 - [clock milliseconds]}]
    if {$wait > 0} {
        after $wait
    }
}

# 1 if what was made after the moment 'since' is certainly still younger than
# 'age' ms. When this is 0 the test took too long (a stalled machine) and
# cannot tell which side of a deadline its last command fell on.
proc gr_in_time {since age} {
    expr {[clock milliseconds] - $since < $age - 5}
}

# SPEC.md section 5: the wealth of a threshold starts at 1 and is multiplied
# by 1 + c alpha / (1 - alpha) for a right guess and by 1 - c for a wrong
# one; the average over c = 0.1, 0.25, 0.5, 0.9 is what counts, and the
# threshold is proven when it reaches 32 x labels x g (g + 1) / DELTA for
# snapshot g. Returns how many right answers that takes when 'wrong' answers
# were wrong.
proc gr_right_answers {alpha delta labels generation {wrong 0}} {
    set bar [expr {32.0 * $labels * $generation * ($generation + 1) / $delta}]
    for {set n 1} {$n < 100000} {incr n} {
        set wealth 0.0
        foreach c {0.1 0.25 0.5 0.9} {
            set wealth [expr {$wealth + pow(1 - $c, $wrong) * pow(1 + $c * $alpha / (1 - $alpha), $n) / 4}]
        }
        if {$wealth >= $bar} {
            return $n
        }
    }
    fail "no number of right answers proves a threshold (alpha $alpha, delta $delta)"
}

# 'count' requests to a score gate that nothing has proven yet, each answered
# at once: the teacher agrees. (Every such request is proof and gets an id.)
proc gr_right_requests {client key count {score 0.9}} {
    for {set n 0} {$n < $count} {incr n} {
        set asked [clock milliseconds]
        lassign [$client gpredict $key SCORE $score] action id
        assert_equal ABSTAIN $action "GPREDICT $key, not answering yet"
        if {[$client glearn $key $id CORRECT 1] != 1} {
            fail "GLEARN $key $id was not used, [expr {[clock milliseconds] - $asked}] ms after its request: [gr_describe $client $key]"
        }
    }
}

# A script that makes ARGV[1] requests to the score gate KEYS[1] and answers
# each of them at once (the teacher agrees). Returns how many answers were
# used. Thousands of them take a moment this way.
set ::gr_answered_requests {
    local used = 0
    for i = 1, tonumber(ARGV[1]) do
        local reply = redis.call('GPREDICT', KEYS[1], 'SCORE', '0.9')
        used = used + redis.call('GLEARN', KEYS[1], reply[2], 'CORRECT', '1')
    end
    return used
}

# ------------------------------------------------ a teacher that takes time
#
# The workloads above answer at once or not at all, and no time passes. In
# the workloads below the teacher answers at once, a little later (and out
# of order), after proof has moved on without the answer (a quarter of the
# feedback timeout), after the request was given up (the feedback timeout),
# or never; it can also fall silent, or start to disagree with everything.
# Meanwhile time passes, in sleeps of a few ms.
#
# 'gates' is a list of {key kind labels timeout}: kind is score or text,
# labels are the classes of a classifier, timeout is the feedback timeout
# the gate was created with (ms).

# How the teacher of a gate behaves: good (the default), wrong or silent.
set ::gr_teacher [dict create]
# Answers the teacher owes: {due key id LABEL|CORRECT answer void}. 'due' is
# a [clock milliseconds]; a void answer is for a request the gate has
# forgotten (it was reset or it recovered) and must not be used.
set ::gr_owed {}
# What becomes of an answer, in percent; the rest never comes.
set ::gr_fates {now 55 soon 15 late 15 toolate 8}
# 1: only requests the teacher and the gate agree on (scores from 0.8 up).
set ::gr_confident 0
array set ::gr_timed {
    ACCEPT 0 AUDIT 0 ABSTAIN 0 NOFEEDBACK 0
    now 0 soon 0 late 0 toolate 0 never 0 silent 0
    used 0 unused 0 refused 0
}

proc gr_timed_start {} {
    set ::gr_teacher [dict create]
    set ::gr_owed {}
    set ::gr_fates {now 55 soon 15 late 15 toolate 8}
    set ::gr_confident 0
    foreach name [array names ::gr_timed] {
        set ::gr_timed($name) 0
    }
}

# The teacher of every gate is right, and answers as usual: at once, late,
# too late or never. (Where a test begins, whatever the one before it left.)
proc gr_timed_usual {} {
    set ::gr_teacher [dict create]
    set ::gr_fates {now 55 soon 15 late 15 toolate 8}
    set ::gr_confident 0
}

proc gr_teacher {key} {
    if {[dict exists $::gr_teacher $key]} {
        return [dict get $::gr_teacher $key]
    }
    return good
}

proc gr_timed_answer {client key id word answer {void 0}} {
    set used [$client glearn $key $id $word $answer]
    if {$void} {
        assert_equal 0 $used "GLEARN $key $id, an id from before the gate started over"
        incr ::gr_timed(refused)
    } elseif {$used == 1} {
        incr ::gr_timed(used)
    } elseif {$used == 0} {
        incr ::gr_timed(unused)
    } else {
        fail "GLEARN $key $id $word $answer replied '$used'"
    }
    return $used
}

# One request, and what becomes of its answer.
proc gr_timed_request {client gate} {
    lassign $gate key kind labels timeout
    set teacher [gr_teacher $key]
    if {$kind eq "score"} {
        if {$::gr_confident} {
            set score [expr {(80 + [gr_rand 21]) / 100.0}]
        } else {
            set score [expr {[gr_rand 101] / 100.0}]
        }
        # The teacher agrees with the cheap result from 0.5 up.
        set right [expr {$teacher eq "wrong" ? 0 : $score >= 0.5}]
        set reply [$client gpredict $key SCORE $score]
        set answer [list CORRECT $right]
    } else {
        set n [gr_rand [llength $labels]]
        set text [gr_text [lindex $labels $n] [gr_rand 1000]]
        if {[gr_rand 10] == 0} {
            set reply [$client gpredict $key TEXT $text FEATURES 2 urgent [expr {[gr_rand 21] / 10.0 - 1}] depth 0.25]
        } else {
            set reply [$client gpredict $key TEXT $text]
        }
        if {$teacher eq "wrong"} {
            # It calls everything by the name of the next label.
            set n [expr {($n + 1) % [llength $labels]}]
        }
        set answer [list LABEL [lindex $labels $n]]
    }
    lassign $reply action id
    if {$action eq "ACCEPT"} {
        incr ::gr_timed(ACCEPT)
        return $action
    }
    if {$id eq {}} {
        incr ::gr_timed(NOFEEDBACK)
        return $action
    }
    incr ::gr_timed($action)
    if {$teacher eq "silent"} {
        incr ::gr_timed(silent)
        return $action
    }
    set dice [gr_rand 100]
    set fate never
    foreach {name share} $::gr_fates {
        if {$dice < $share} {
            set fate $name
            break
        }
        set dice [expr {$dice - $share}]
    }
    incr ::gr_timed($fate)
    set grace [expr {$timeout / 4}]
    set now [clock milliseconds]
    switch $fate {
        now {
            gr_timed_answer $client $key $id {*}$answer
        }
        soon {
            lappend ::gr_owed [list [expr {$now + [gr_rand [expr {$grace / 2 + 1}]]}] $key $id {*}$answer 0]
        }
        late {
            lappend ::gr_owed [list [expr {$now + $grace + 2 + [gr_rand [expr {$timeout / 2 + 1}]]}] $key $id {*}$answer 0]
        }
        toolate {
            lappend ::gr_owed [list [expr {$now + $timeout + 2 + [gr_rand 40]}] $key $id {*}$answer 0]
        }
    }
    return $action
}

# Requests to a gate until one gets an id. Its answer is owed from now on,
# so the request waits. Returns the id.
proc gr_timed_owe {client gate} {
    lassign $gate key kind labels timeout
    for {set n 0} {$n < 400} {incr n} {
        if {$kind eq "score"} {
            lassign [$client gpredict $key SCORE 0.2] action id
            set answer {CORRECT 0}
        } else {
            lassign [$client gpredict $key TEXT [gr_text [lindex $labels 0] $n]] action id
            set answer [list LABEL [lindex $labels 0]]
        }
        # (What comes with an ACCEPT of a classifier is a label.)
        if {$action ne "ACCEPT" && $id ne {}} {
            lappend ::gr_owed [list [clock milliseconds] $key $id {*}$answer 0]
            return $id
        }
    }
    fail "gate $key handed out no id in 400 requests: [gr_describe $client $key]"
}

# Send the answers that are due (all of them if 'all'), in no order.
proc gr_timed_deliver {client {all 0}} {
    set now [clock milliseconds]
    set due {}
    set kept {}
    foreach owed $::gr_owed {
        if {$all || [lindex $owed 0] <= $now} {
            lappend due $owed
        } else {
            lappend kept $owed
        }
    }
    set ::gr_owed $kept
    while {[llength $due] > 0} {
        set i [gr_rand [llength $due]]
        gr_timed_answer $client {*}[lrange [lindex $due $i] 1 end]
        set due [lreplace $due $i $i]
    }
}

# The gates 'keys' were reset or have recovered: what the teacher owes for
# them is for requests they have forgotten. It is sent all the same.
proc gr_timed_void {keys} {
    set owed {}
    foreach answer $::gr_owed {
        if {[lsearch -exact $keys [lindex $answer 1]] >= 0} {
            lset answer 5 1
        }
        lappend owed $answer
    }
    set ::gr_owed $owed
}

proc gr_timed_workload {client gates nops} {
    for {set op 0} {$op < $nops} {incr op} {
        set gate [lindex $gates [gr_rand [llength $gates]]]
        lassign $gate key kind labels timeout
        set dice [gr_rand 100]
        if {$dice < 62} {
            gr_timed_request $client $gate
        } elseif {$dice < 74} {
            gr_timed_deliver $client
        } elseif {$dice < 82} {
            # Time passes.
            after [expr {1 + [gr_rand 14]}]
        } elseif {$dice < 88} {
            if {$kind eq "text"} {
                set label [lindex $labels [gr_rand [llength $labels]]]
                $client gadd $key TEXT [gr_text $label [gr_rand 1000]] LABEL $label
            } else {
                gr_timed_request $client $gate
            }
        } elseif {$dice < 90} {
            $client gpause $key
        } elseif {$dice < 93} {
            $client gresume $key
        } elseif {$dice < 96} {
            # An id this gate never issued.
            set id [lindex $::gr_bogus_ids [gr_rand [llength $::gr_bogus_ids]]]
            if {$kind eq "score"} {
                assert_equal 0 [$client glearn $key $id CORRECT 1] "GLEARN $key '$id'"
            } else {
                assert_equal 0 [$client glearn $key $id LABEL [lindex $labels 0]] "GLEARN $key '$id'"
            }
        } else {
            $client ginfo $key
        }
    }
}

# Requests, answered at once by a teacher that is right, until every gate
# answers (at most 'max' requests).
proc gr_timed_until_active {client gates {max 12000}} {
    set fates $::gr_fates
    set ::gr_fates {now 100}
    foreach gate $gates {
        $client gresume [lindex $gate 0]
    }
    for {set n 0} {$n < $max} {incr n} {
        if {$n % 40 == 0} {
            set active 1
            foreach gate $gates {
                if {[gr_field $client [lindex $gate 0] state] ne "ACTIVE"} {
                    set active 0
                    break
                }
            }
            if {$active} break
        }
        gr_timed_request $client [lindex $gates [expr {$n % [llength $gates]}]]
    }
    set ::gr_fates $fates
    foreach gate $gates {
        set key [lindex $gate 0]
        if {[gr_field $client $key state] ne "ACTIVE"} {
            fail "gate $key is not ACTIVE after $max answered requests to [llength $gates] gates: [gr_describe $client $key]"
        }
    }
    return $n
}

# The bytes of "_GATE.APPLY key TICK clock" in the stream, for a clock of 13
# digits (milliseconds, until the year 2286).
proc gr_tick_bytes {key} {
    expr {58 + [string length $key]}
}

# 'count' requests to an answering score gate whose teacher agrees from 0.7
# up, half of them sure, half of them hopeless. A request that gets an id (a
# spot check) is answered at once, and both operations are appended to the
# list named 'expectedvar'. Returns how many requests got no id (accepted,
# or not answered and not wanted as proof) and after how many of those
# master_repl_offset had moved.
proc gr_quiet_run {client key count expectedvar} {
    upvar 1 $expectedvar expected
    set quiet 0
    set moved 0
    for {set n 0} {$n < $count} {incr n} {
        set score [lindex {0.95 0.2 0.99 0.1} [expr {$n % 4}]]
        set offset [status $client master_repl_offset]
        lassign [$client gpredict $key SCORE $score] action id
        if {$id eq {}} {
            incr quiet
            if {[status $client master_repl_offset] != $offset} {
                incr moved
            }
        } else {
            lappend expected PREDICT LEARN
            assert_equal 1 [$client glearn $key $id CORRECT [expr {$score >= 0.7}]] "GLEARN $key $id"
        }
    }
    return [list $quiet $moved]
}

# One request to such a gate at a moment when something is due. Without an
# id it must send a TICK, with an id (a spot check, answered at once) the
# step travels with the request. Returns 1 if the request got no id, and the
# bytes the request itself added to the stream.
proc gr_step_request {client key score expectedvar} {
    upvar 1 $expectedvar expected
    set offset [status $client master_repl_offset]
    lassign [$client gpredict $key SCORE $score] action id
    set bytes [expr {[status $client master_repl_offset] - $offset}]
    if {$id eq {}} {
        lappend expected TICK
        return [list 1 $bytes]
    }
    lappend expected PREDICT LEARN
    assert_equal 1 [$client glearn $key $id CORRECT [expr {$score >= 0.7}]] "GLEARN $key $id"
    return [list 0 $bytes]
}

# ------------------------------------------ what every node shows of a gate

# GINFO 'key' on one node shows these fields with these values.
proc gr_expect_fields {node key expected {what ""}} {
    set info [gr_info $node $key]
    set wrong {}
    dict for {field value} $expected {
        if {[dict get $info $field] ne $value} {
            lappend wrong "$field is '[dict get $info $field]', expected '$value'"
        }
    }
    if {[llength $wrong] > 0} {
        fail "$what: GINFO $key: [join $wrong {; }]\n[gr_describe $node $key]"
    }
}

# Once the replicas have caught up, GINFO 'key' shows these fields with these
# values on every node.
proc gr_expect {primary replicas key expected {what ""}} {
    gr_sync $primary $replicas
    gr_expect_fields $primary $key $expected "$what, on the primary"
    set n 0
    foreach replica $replicas {
        gr_expect_fields $replica $key $expected "$what, on replica #[incr n]"
    }
}

# The number in an id ("<epoch>-<number>"): requests are numbered in the
# order they were asked.
proc gr_id_number {id} {
    lindex [split $id -] 1
}

# A list in another order: the same one in every run.
proc gr_shuffle {list} {
    for {set i [expr {[llength $list] - 1}]} {$i > 0} {incr i -1} {
        set j [gr_rand [expr {$i + 1}]]
        set held [lindex $list $i]
        lset list $i [lindex $list $j]
        lset list $j $held
    }
    return $list
}

# 'count' requests to a score gate that answers and checks every request it
# could answer (AUDIT 1): each of them is a spot check. Returns their ids;
# nothing is answered.
proc gr_spot_checks {client key count {score 0.9}} {
    set ids {}
    for {set n 0} {$n < $count} {incr n} {
        lassign [$client gpredict $key SCORE $score] action id
        assert_equal AUDIT $action "GPREDICT $key SCORE $score, to a gate that checks everything it could answer"
        lappend ids $id
    }
    return $ids
}

# 'count' requests to a score gate that get an id each and no answer.
proc gr_unanswered {client key count {score 0.9}} {
    set ids {}
    for {set n 0} {$n < $count} {incr n} {
        lassign [$client gpredict $key SCORE $score] action id
        if {$action eq "ACCEPT" || $id eq {}} {
            fail "GPREDICT $key SCORE $score replied '$action' without an id: [gr_describe $client $key]"
        }
        lappend ids $id
    }
    return $ids
}

# ---------------------------------------------- the drift detector, by hand
#
# SPEC.md, section 5: "likelihood ratios for 2x and 4x the error rate (at
# most halfway to 1), averaged, over blocks of 64, 512 and 4,096 spot checks
# in the order they were asked; a spot check counts as right until its
# answer says otherwise; alarm at 1,000, and at 10^9 with lost answers
# counted as disagreements". A disagreement or a lost answer whose block has
# ended counts only in the longer blocks that are still open (gate_stats.h,
# CHANGES.md 9f).
#
# A replica runs the code of its primary, so that the two agree says nothing
# about whether either is right. The tests therefore keep a detector of their
# own, made from the words above, and hold every node to it.

set ::gr_drift_blocks {64 512 4096}

# The statistic of a block, as a natural logarithm: 'asked' spot checks were
# asked in it so far and 'against' of them count against the gate.
proc gr_drift_stat {alpha asked against} {
    set terms {}
    foreach times {2 4} {
        set p [expr {min($times * $alpha, (1 + $alpha) / 2.0)}]
        lappend terms [expr {$against * log($p / $alpha) + ($asked - $against) * log((1 - $p) / (1 - $alpha))}]
    }
    lassign $terms a b
    set top [expr {max($a, $b)}]
    expr {$top + log((exp($a - $top) + exp($b - $top)) / 2)}
}

# 1 if that statistic has reached 'level' (1000, or 1e9 with lost answers).
proc gr_drift_reached {alpha asked against level} {
    set stat [gr_drift_stat $alpha $asked $against]
    set bar [expr {log($level)}]
    if {abs($stat - $bar) < 1e-9} {
        fail "a block of $asked spot checks with $against against the gate (alpha $alpha) sits on the alarm level $level: this test cannot tell on which side"
    }
    expr {$stat >= $bar}
}

# A detector: the spot checks asked and, for each block length, the
# disagreements and the lost answers of the block now open.
proc gr_detector {alpha} {
    dict create alpha $alpha asked 0 wrong {0 0 0} lost {0 0 0}
}

# A spot check is asked. Returns its number.
proc gr_detector_ask {var} {
    upvar 1 $var d
    set asked [dict get $d asked]
    set scale 0
    foreach length $::gr_drift_blocks {
        if {$asked % $length == 0} {
            # The first spot check of a block opens it.
            foreach what {wrong lost} {
                dict set d $what [lreplace [dict get $d $what] $scale $scale 0]
            }
        }
        incr scale
    }
    dict incr d asked
    return [dict get $d asked]
}

# The answer of spot check 'number' disagreed ('what' is wrong) or was given
# up ('what' is lost). Returns quiet, worse (the disagreements of a block
# have reached 1,000) or unchecked (only together with its lost answers:
# 10^9).
proc gr_detector_count {var number what} {
    upvar 1 $var d
    set alpha [dict get $d alpha]
    set asked [dict get $d asked]
    set verdict quiet
    set scale -1
    foreach length $::gr_drift_blocks {
        incr scale
        if {($number - 1) / $length != ($asked - 1) / $length} continue  ;# its block has ended
        set counts [dict get $d $what]
        lset counts $scale [expr {[lindex $counts $scale] + 1}]
        dict set d $what $counts
        set open [expr {($asked - 1) % $length + 1}]
        set wrong [lindex [dict get $d wrong] $scale]
        set lost [lindex [dict get $d lost] $scale]
        if {$what eq "wrong" && [gr_drift_reached $alpha $open $wrong 1000]} {
            set verdict worse
        }
        if {$verdict ne "worse" && [gr_drift_reached $alpha $open [expr {$wrong + $lost}] 1e9]} {
            set verdict unchecked
        }
    }
    return $verdict
}

# --------------------------------- a score gate the test knows all about
#
# A score gate that was proven with right answers only answers every score
# (its lowest threshold, 0, is proven first). While it answers, its replies
# tell what the dice were: ACCEPT is a request that was not drawn as a spot
# check, AUDIT one that was (while paused: ABSTAIN without and with an id).
# So a test can know everything GINFO shows of such a gate, whatever the
# dice, from SPEC.md alone:
#
#   * a spot check gets its number in the detector when it is asked, and is
#     reported there the moment its answer arrives: audit-samples counts the
#     answers, audit-errors the disagreements;
#   * proof is counted in request order (calibration-samples): an answer
#     waits for the answers of older requests;
#   * the gate stops when the detector says so. If it got worse, a new
#     snapshot is tested at once. If it can no longer be checked (lost
#     answers), or 32 requests in a row ended without an answer while no
#     answer arrived at all, it takes no snapshot until an answer arrives.
#     Either way it forgets the requests that waited.
#
# The model is a dict: generation (0: it does not answer), top (the last
# snapshot number handed out), waiting (id -> spot check number), queue (ids
# in the order asked whose proof is not counted yet), done (answered ones
# among them), and what GINFO counts.

array set ::gr_exact {worse 0 unchecked 0 blind 0 late 0 multi 0 script 0 unknown 0 refused 0 paused 0 reset 0}

# The model of the score gate 'key', created just now: it tests its first
# snapshot.
proc gr_exact_new {key alpha {delta 0.2}} {
    dict create key $key alpha $alpha delta $delta \
        generation 0 top 1 blind 0 paused 0 \
        samples 0 audits 0 errors 0 missing 0 revocations 0 \
        detector [gr_detector $alpha] waiting {} queue {} done {} \
        topseq 0 mark 1 run 0
}

# GINFO shows what the model says.
proc gr_exact_check {client var {where ""}} {
    upvar 1 $var m
    set generation [dict get $m generation]
    set blind [dict get $m blind]
    if {[dict get $m paused]} {
        set state PAUSED
    } elseif {$generation != 0} {
        set state ACTIVE
    } elseif {$blind} {
        set state COLD
    } else {
        set state CALIBRATING
    }
    gr_expect_fields $client [dict get $m key] [dict create \
        state $state generation $generation \
        candidate [expr {$generation != 0 || $blind ? 0 : [dict get $m top]}] \
        revocations [dict get $m revocations] missing [dict get $m missing] \
        pending [dict size [dict get $m waiting]] \
        audit-samples [dict get $m audits] audit-errors [dict get $m errors] \
        calibration-samples [dict get $m samples]] $where
}

# Make the gate answer: right answers, as many as SPEC.md section 5 says its
# snapshot needs, and not one less.
proc gr_exact_prove {client var} {
    upvar 1 $var m
    set key [dict get $m key]
    if {[dict get $m generation] != 0} {
        fail "gr_exact_prove: $key answers already"
    }
    if {[dict get $m paused]} {
        assert_equal OK [$client gresume $key]
        dict set m paused 0
    }
    if {[dict get $m blind]} {
        # It stopped because it could not be checked, and takes no snapshot
        # until an answer arrives. Its requests are recorded meanwhile, so
        # that there is an id to answer.
        lassign [$client gpredict $key SCORE 0.9] action id
        assert_equal ABSTAIN $action "GPREDICT $key, stopped for want of answers"
        assert_equal 1 [$client glearn $key $id CORRECT 1] "GLEARN $key $id, the first answer since the gate stopped"
        dict set m blind 0
        dict incr m top
    }
    gr_exact_check $client m "before the right answers"
    set top [dict get $m top]
    set need [gr_right_answers [dict get $m alpha] [dict get $m delta] 1 $top]
    set id {}
    for {set n 1} {$n <= $need} {incr n} {
        if {$n == $need} {
            assert_equal CALIBRATING [gr_field $client $key state] "state of $key after [expr {$need - 1}] right answers (snapshot $top needs $need)"
        }
        lassign [$client gpredict $key SCORE 0.9] action id
        assert_equal ABSTAIN $action "GPREDICT $key, not answering yet"
        assert_equal 1 [$client glearn $key $id CORRECT 1] "GLEARN $key $id"
    }
    dict set m generation $top
    dict set m samples $need
    dict set m audits 0
    dict set m errors 0
    dict set m detector [gr_detector [dict get $m alpha]]
    dict set m waiting {}
    dict set m queue {}
    dict set m done {}
    dict set m topseq [gr_id_number $id]
    dict set m mark [expr {[dict get $m topseq] + 1}]
    dict set m run 0
    gr_exact_check $client m "after the $need right answers snapshot $top needs"
    assert_equal 0 [lindex [gr_field $client $key thresholds] 0] "the threshold of $key, proven with right answers only"
}

# One request to the gate while it answers. Returns its id if it was drawn
# as a spot check, else an empty string.
proc gr_exact_ask {client var score} {
    upvar 1 $var m
    set key [dict get $m key]
    if {[dict get $m generation] == 0} {
        fail "gr_exact_ask: $key does not answer"
    }
    set paused [dict get $m paused]
    lassign [$client gpredict $key SCORE $score] action id
    if {$id eq {}} {
        assert_equal [expr {$paused ? "ABSTAIN" : "ACCEPT"}] $action "GPREDICT $key SCORE $score, without an id"
        return {}
    }
    assert_equal [expr {$paused ? "ABSTAIN" : "AUDIT"}] $action "GPREDICT $key SCORE $score, with an id"
    set d [dict get $m detector]
    dict set m waiting $id [gr_detector_ask d]
    dict set m detector $d
    dict lappend m queue $id
    dict set m topseq [gr_id_number $id]
    return $id
}

# The gate stops answering.
proc gr_exact_stop {var verdict} {
    upvar 1 $var m
    incr ::gr_exact($verdict)
    dict incr m revocations
    dict set m generation 0
    dict set m samples 0
    dict set m audits 0
    dict set m errors 0
    # The requests that waited were proof for a snapshot that is gone.
    dict set m waiting {}
    dict set m queue {}
    dict set m done {}
    dict set m run 0
    if {$verdict eq "worse"} {
        dict incr m top
    } else {
        dict set m blind 1
    }
}

# GRESET: the gate forgets everything learned and proven, the requests that
# wait and the counters. It keeps the pause, and hands no snapshot number
# out again: the next snapshot is tested at once.
proc gr_exact_reset {var} {
    upvar 1 $var m
    foreach field {generation samples audits errors missing revocations blind run} {
        dict set m $field 0
    }
    foreach field {waiting queue done} {
        dict set m $field {}
    }
    dict incr m top
    incr ::gr_exact(reset)
}

# The gate is in the hands of a primary that cannot know what it has missed
# (a failover, a reload): at its first write it drops its proof and the
# requests that wait, keeps its counters, and is tested again at once under
# a number that was never used.
proc gr_exact_recover {var} {
    upvar 1 $var m
    foreach field {generation samples audits errors blind run} {
        dict set m $field 0
    }
    foreach field {waiting queue done} {
        dict set m $field {}
    }
    dict incr m top
}

# What the answer of request 'id' does. Returns what GLEARN must reply (1 if
# the answer is used) and the verdict of the detector.
proc gr_exact_settle {var id correct} {
    upvar 1 $var m
    if {![dict exists $m waiting $id]} {
        return {0 quiet}
    }
    set number [dict get $m waiting $id]
    dict unset m waiting $id
    # An answer has arrived: every request asked so far has seen one.
    dict set m mark [expr {[dict get $m topseq] + 1}]
    dict set m run 0
    dict incr m audits
    set verdict quiet
    if {!$correct} {
        dict incr m errors
        set d [dict get $m detector]
        set verdict [gr_detector_count d $number wrong]
        dict set m detector $d
    }
    if {$verdict ne "quiet"} {
        gr_exact_stop m $verdict
        return [list 1 $verdict]
    }
    dict set m done $id 1
    set queue [dict get $m queue]
    while {[llength $queue] > 0 && [dict exists $m done [lindex $queue 0]]} {
        dict unset m done [lindex $queue 0]
        set queue [lrange $queue 1 end]
        dict incr m samples
    }
    dict set m queue $queue
    return {1 quiet}
}

# Send the answer of request 'id'. Returns the verdict of the detector.
proc gr_exact_answer {client var id correct} {
    upvar 1 $var m
    lassign [gr_exact_settle m $id $correct] used verdict
    assert_equal $used [$client glearn [dict get $m key] $id CORRECT $correct] "GLEARN [dict get $m key] $id CORRECT $correct"
    return $verdict
}

set ::gr_answers_script {
    local replies = {}
    for i = 1, #ARGV, 2 do
        replies[#replies + 1] = redis.call('GLEARN', KEYS[1], ARGV[i], 'CORRECT', ARGV[i + 1])
    end
    return replies
}

# Send several answers ({id correct ...}) at once: in one transaction ('how'
# is multi) or in one script. Returns the verdicts. With 'late' set, proof
# may have moved on without some of these answers: a gate that stops in the
# middle of them forgets the requests that still waited for their turn, but
# not those its proof had moved on without (they keep a valid id until their
# time is up), so what it replies to the rest is not known here.
proc gr_exact_batch {client var answers how {late 0}} {
    upvar 1 $var m
    set key [dict get $m key]
    if {$how eq "script"} {
        set replies [$client eval $::gr_answers_script 1 $key {*}$answers]
    } else {
        $client multi
        foreach {id correct} $answers {
            $client glearn $key $id CORRECT $correct
        }
        set replies [$client exec]
    }
    set expected {}
    set verdicts {}
    foreach {id correct} $answers {
        if {$late && [dict get $m generation] == 0} {
            set used [lindex $replies [llength $expected]]
            assert {$used in {0 1}}
            lappend expected $used
            continue
        }
        lassign [gr_exact_settle m $id $correct] used verdict
        lappend expected $used
        lappend verdicts $verdict
    }
    assert_equal $expected $replies "the replies to [llength $expected] answers for $key, sent in one $how"
    return $verdicts
}

# Every request that still waits has waited longer than the feedback timeout
# of the gate, and no write found out yet. This one does (GRESUME): the
# requests are given up, oldest first. Each is missing, and a lost spot
# check is reported to the detector; one that has seen no answer arrive
# since it was asked also counts towards the 32 in a row that stop a gate.
# Returns quiet, unchecked or blind.
proc gr_exact_give_up {client var} {
    upvar 1 $var m
    assert_equal OK [$client gresume [dict get $m key]]
    dict set m paused 0
    set verdict quiet
    foreach id [dict get $m queue] {
        dict incr m samples
        if {[dict exists $m done $id]} continue  ;# answered: counted in its turn
        set number [dict get $m waiting $id]
        dict unset m waiting $id
        dict incr m missing
        if {[gr_id_number $id] >= [dict get $m mark]} {
            dict incr m run
            if {[dict get $m run] >= 32} {
                set verdict blind
                break
            }
        }
        set d [dict get $m detector]
        set verdict [gr_detector_count d $number lost]
        dict set m detector $d
        if {$verdict ne "quiet"} break
    }
    if {$verdict ne "quiet"} {
        # What still waited is forgotten, not missed.
        gr_exact_stop m $verdict
    } else {
        dict set m queue {}
        dict set m done {}
    }
    return $verdict
}

# One phase of a workload on such a gate, 'nops' steps. The teacher
# disagrees with 'wrong' percent of the spot checks. It answers some at
# once and holds the others back ('heldvar': {id correct ...}), to send them
# later one by one, or several in one transaction or one script, in no
# order. When the gate stops, what the teacher still holds is for requests
# the gate has forgotten; then the gate is proven again.
proc gr_exact_workload {client var heldvar nops wrong} {
    upvar 1 $var m $heldvar held
    set key [dict get $m key]
    for {set op 0} {$op < $nops} {incr op} {
        if {[dict get $m generation] == 0} {
            gr_exact_check $client m "after the gate stopped"
            foreach {id correct} $held {
                assert_equal 0 [$client glearn $key $id CORRECT $correct] "GLEARN $key $id, for a request the gate has forgotten"
                incr ::gr_exact(refused)
            }
            set held {}
            gr_exact_prove $client m
            continue
        }
        set dice [gr_rand 100]
        set waiting [expr {[llength $held] / 2}]
        if {$dice < 58} {
            set id [gr_exact_ask $client m [expr {[gr_rand 101] / 100.0}]]
            if {$id eq {}} continue
            set correct [expr {[gr_rand 100] >= $wrong}]
            if {[gr_rand 100] < 45} {
                gr_exact_answer $client m $id $correct
            } else {
                lappend held $id $correct
            }
        } elseif {$dice < 72} {
            # One of the answers the teacher held back, any of them. (It
            # keeps a few to itself for a while.)
            if {$waiting == 0 || ($waiting < 8 && [gr_rand 3] != 0)} continue
            set i [expr {2 * [gr_rand $waiting]}]
            lassign [lrange $held $i [expr {$i + 1}]] id correct
            set held [lreplace $held $i [expr {$i + 1}]]
            gr_exact_answer $client m $id $correct
            incr ::gr_exact(late)
        } elseif {$dice < 84} {
            # Several of them at once, in no order.
            if {$waiting < 8} continue
            set answers {}
            for {set n [expr {2 + [gr_rand 6]}]} {$n > 0 && [llength $held] > 0} {incr n -1} {
                set i [expr {2 * [gr_rand [expr {[llength $held] / 2}]]}]
                lappend answers {*}[lrange $held $i [expr {$i + 1}]]
                set held [lreplace $held $i [expr {$i + 1}]]
            }
            set how [lindex {multi script} [gr_rand 2]]
            gr_exact_batch $client m $answers $how
            incr ::gr_exact($how)
        } elseif {$dice < 89} {
            # An answer for a request that never was, or was another gate's.
            set id [lindex $::gr_bogus_ids [gr_rand [llength $::gr_bogus_ids]]]
            assert_equal 0 [$client glearn $key $id CORRECT 1] "GLEARN $key '$id'"
            incr ::gr_exact(unknown)
        } elseif {$dice < 93} {
            if {[dict get $m paused]} {
                assert_equal OK [$client gresume $key]
                dict set m paused 0
            } else {
                assert_equal OK [$client gpause $key]
                dict set m paused 1
                incr ::gr_exact(paused)
            }
        } elseif {$dice < 94 && [gr_rand 4] == 0} {
            assert_equal OK [$client greset $key]
            gr_exact_reset m
        } else {
            gr_exact_check $client m "in the middle of the workload"
        }
    }
}

# ------------------------------------------------- a snapshot that takes over
#
# SPEC.md, section 3: "A newer snapshot takes over only when it has proven
# thresholds of its own and answers more of the same spot-checked requests
# than the current one. A classifier snapshot that has not taken over after
# a generous amount of proof is dropped and the next one is tried."
# Section 5: dropped "after 10 times the proof a flawless threshold needs,
# per label, without taking over; requests it left to the teacher although
# their label is one the gate may answer count like proof".
#
# The gates below check every request they could answer (AUDIT 1), so what
# happens to them does not depend on the dice.

# Requests are written in the vocabulary of 'words' and their teacher says
# 'label': {words label}.
set ::gr_vocabularies {{spam spam} {ham ham} {spam2 spam} {ham2 ham}}

# A classifier whose first snapshot knows two of the four vocabularies its
# requests are written in. It proves what it is sure of and starts to
# answer; the model has meanwhile learned all four from the answers, and
# its second snapshot is tested at once. Returns when that is so: snapshot 1
# answers, snapshot 2 is tested.
proc gr_takeover_begin {client key {audit 1}} {
    $client config set gate.snapshot-examples 40
    assert_equal OK [$client gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT $audit CLASSES 2 spam ham]
    $client config set gate.snapshot-examples 20
    for {set n 0} {$n < 20} {incr n} {
        foreach label {spam ham} {
            $client gadd $key TEXT [gr_text $label $n] LABEL $label
        }
    }
    for {set n 0} {$n < 2000 && [gr_field $client $key generation] == 0} {incr n} {
        lassign [lindex $::gr_vocabularies [expr {$n % 4}]] words label
        gr_text_request $client $key [gr_text $words $n] $label
    }
    set info [gr_info $client $key]
    assert_equal ACTIVE [dict get $info state] "state of $key after $n requests"
    assert_equal 1 [dict get $info generation]
    assert_equal 2 [dict get $info candidate]
    # The first snapshot does not answer everything: it leaves to the
    # teacher what it can only guess.
    if {[dict get $info thresholds] eq {0.5 0.5}} {
        fail "the first snapshot of $key answers everything: this test needs one that does not"
    }
}

# 'count' requests in the vocabularies the first snapshot knows, numbered
# from 'from', each answered at once. Returns the actions.
proc gr_takeover_known {client key from count} {
    set actions {}
    for {set n $from} {$n < $from + $count} {incr n} {
        lassign [lindex $::gr_vocabularies [expr {$n % 2}]] words label
        lappend actions [gr_text_request $client $key [gr_text $words $n] $label]
    }
    return $actions
}

# Requests to a classifier of the labels billing and other, numbered from
# 'from', until 'count' of them were answered 'billing'. Every fifth is one
# the teacher calls 'other'. Returns the number of the next request.
proc gr_defer_requests {client key from count} {
    set n $from
    while {$count > 0} {
        if {$n % 5 == 4} {
            set label other
        } else {
            set label billing
            incr count -1
        }
        lassign [$client gpredict $key TEXT [gr_text $label $n]] action id
        if {$action ne "ACCEPT"} {
            assert_equal 1 [$client glearn $key $id LABEL $label] "GLEARN $key $id"
        }
        incr n
    }
    return $n
}

# ------------------------------------- one story for every way to get a gate
#
# Gates in the middle of something that only comes out right if a node holds
# all of their state, and that comes out the same whatever the dice:
#
#   worse   40 spot checks asked in the first block; 10 agreed and 3
#           disagreed. Then the last 8 of them disagree, and 8 of the others
#           in no order: nothing, there is no restart inside a block and 19
#           of 40 make 901. The 20th disagreement stops the gate (3,325).
#   block   72 spot checks asked: the first block of 64 has ended. Its
#           spot checks 1 to 6 disagree, then 65 to 71: nothing, the first
#           six count in the longer blocks only and 7 of the 8 in the open
#           block make 594.75. The 72nd makes 8 of 8: 3,408.5.
#   lost    12 spot checks in a row given up. Then 8 more: the 20th lost
#           spot check of the block stops the gate (19 make 0.58 x 10^9, 20
#           make 1.74 x 10^9), which keeps what it learned and waits for an
#           answer.
#   run     20 requests in a row given up while no answer arrived. Then 12
#           more: the 32nd stops the gate.
#   mark    20 requests without an answer, one that is answered, 12 without:
#           all given up at once, and the gate goes on. An answer arrived
#           after the first 20 were asked, so they are lost answers, not an
#           outage.
#   clear   31 requests in a row given up; then an answer arrives, which
#           ends the run. 31 more are given up: the gate goes on.
#   keep    a newer snapshot is compared with the answering one, which
#           answers every request of the comparison. The newer one cannot
#           answer more: it is dropped after 880 pieces of proof, not one
#           earlier (10 x 44 x 2 labels: SPEC.md section 5). Then the
#           teacher calls 'spam' whatever the answering snapshot takes for
#           'ham'. Each label has a detector of its own, and 439 spot
#           checks of 'ham' were asked so far: 55 of them have opened a
#           block of 64, in which the 9 disagreements that still fit stop
#           nothing. The next block is stopped by its 7th: the 16th
#           disagreement in all. The classifier forgets what it learned.
#   swap    the same comparison for 100 requests, then a request the
#           answering snapshot leaves to the teacher: the newer one takes
#           over with it.
#   defer   a snapshot that only knows the label the gate never answers
#           with. 200 requests it left to the teacher although their label
#           is one the gate may answer; with the 350th (10 x 35 x 1 label)
#           it is dropped. The next snapshot answers after the 41 right
#           answers the second snapshot of a key needs, and answers every
#           request of that label. So the third cannot answer more, and is
#           dropped after 440 pieces of proof (10 x 44). By then the model
#           has been taught that every text is of the label the gate never
#           answers with: the fourth snapshot leaves everything to the
#           teacher, proves nothing, and is dropped after 470 requests that
#           were spot checks of the answering snapshot (10 x 47).
#
# gr_story_begin takes the gates to the middle, gr_story_end from there to
# the end: on a primary whose replicas were there all along, reloaded
# meanwhile, attached in between, or promoted.

proc gr_story_keys {p} {
    set keys {}
    foreach name {worse block lost run mark clear keep swap defer} {
        lappend keys $p:$name
    }
    return $keys
}

proc gr_story_begin {client p} {
    set timeout 300
    set need [gr_right_answers 0.2 0.2 1 1]
    set story [dict create p $p timeout $timeout]

    $client config set gate.feedback-timeout 600000
    assert_equal OK [$client gcreate $p:worse ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
    assert_equal OK [$client gcreate $p:block ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
    $client config set gate.feedback-timeout $timeout
    assert_equal OK [$client gcreate $p:lost ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
    assert_equal OK [$client gcreate $p:run ALPHA 0.2 DELTA 0.2 SCORE]
    assert_equal OK [$client gcreate $p:mark ALPHA 0.2 DELTA 0.2 SCORE]
    assert_equal OK [$client gcreate $p:clear ALPHA 0.2 DELTA 0.2 SCORE]
    $client config set gate.feedback-timeout 600000

    gr_right_requests $client $p:worse $need
    set spots [gr_spot_checks $client $p:worse 40]
    foreach id [lrange $spots 0 9] {
        assert_equal 1 [$client glearn $p:worse $id CORRECT 1]
    }
    foreach id [lrange $spots 10 12] {
        assert_equal 1 [$client glearn $p:worse $id CORRECT 0]
    }
    dict set story worse [lrange $spots 13 end]

    gr_right_requests $client $p:block $need
    set spots [gr_spot_checks $client $p:block 72]
    foreach id [lrange $spots 0 5] {
        assert_equal 1 [$client glearn $p:block $id CORRECT 0]
    }
    dict set story block [lrange $spots 64 end]

    # The requests that will have been given up by the time this returns.
    gr_right_requests $client $p:lost $need
    gr_spot_checks $client $p:lost 12
    gr_unanswered $client $p:run 20
    set ids [gr_unanswered $client $p:mark 21]
    assert_equal 1 [$client glearn $p:mark [lindex $ids end] CORRECT 1]
    gr_unanswered $client $p:mark 12
    gr_unanswered $client $p:clear 31
    dict set story asked [clock milliseconds]

    foreach name {keep swap} {
        gr_takeover_begin $client $p:$name
        set actions [gr_takeover_known $client $p:$name 0 60]
        # What the story needs of the first snapshot (nothing here depends
        # on the dice, so it is so in every run or in none): it had no
        # threshold for 'ham' yet when the second of these requests was
        # asked, and the answer gave it one, the lowest there is. Every
        # other request was a spot check: it answers them all.
        assert_equal ABSTAIN [lindex $actions 1] "the second request of the comparison"
        assert_equal 1 [lsearch -all -not $actions AUDIT] "requests of the comparison that the answering snapshot of $p:$name left to the teacher"
        set thresholds [gr_field $client $p:$name thresholds]
        assert {[lindex $thresholds 0] > 0.5}
        assert_equal 0.5 [lindex $thresholds 1]
    }

    assert_equal OK [$client gcreate $p:defer ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 billing other ALWAYSDEFER 1 other]
    for {set n 0} {$n < 20} {incr n} {
        $client gadd $p:defer TEXT [gr_text other $n] LABEL other
    }
    dict set story defer [gr_defer_requests $client $p:defer 0 200]

    gr_wait_age [dict get $story asked] $timeout
    assert_equal OK [$client gresume $p:lost]
    assert_equal OK [$client gresume $p:run]
    assert_equal OK [$client gresume $p:clear]
    gr_right_requests $client $p:clear 1
    return $story
}

# What every node must show of the gates of a story in the middle.
proc gr_story_middle {primary replicas story} {
    set p [dict get $story p]
    gr_expect $primary $replicas $p:worse {state ACTIVE generation 1 audit-samples 13 audit-errors 3 pending 27 calibration-samples 48} "$p in the middle"
    gr_expect $primary $replicas $p:block {state ACTIVE generation 1 audit-samples 6 audit-errors 6 pending 66} "$p in the middle"
    gr_expect $primary $replicas $p:lost {state ACTIVE generation 1 missing 12 pending 0 revocations 0} "$p in the middle"
    gr_expect $primary $replicas $p:run {state CALIBRATING candidate 1 missing 20 pending 0 calibration-samples 20} "$p in the middle"
    gr_expect $primary $replicas $p:clear {state CALIBRATING candidate 1 missing 31 pending 0 calibration-samples 32} "$p in the middle"
    foreach name {keep swap} {
        gr_expect $primary $replicas $p:$name {state ACTIVE generation 1 candidate 2 revocations 0} "$p in the middle"
    }
    gr_expect $primary $replicas $p:defer [dict create state CALIBRATING generation 0 candidate 1 calibration-samples 0 \
        learned [expr {20 + [dict get $story defer]}]] "$p in the middle"
    gr_phase $primary $replicas [gr_story_keys $p] "$p in the middle"
}

proc gr_story_end {primary replicas story} {
    set p [dict get $story p]
    set timeout [dict get $story timeout]

    # The requests that will be given up at the end.
    gr_spot_checks $primary $p:lost 8
    gr_unanswered $primary $p:run 12
    gr_unanswered $primary $p:clear 31
    set asked [clock milliseconds]

    # worse: spot checks 14 to 40 wait.
    set waiting [dict get $story worse]
    assert_equal 27 [llength $waiting]
    assert {![gr_drift_reached 0.2 40 19 1000] && [gr_drift_reached 0.2 40 20 1000]}
    foreach id [lrange $waiting end-7 end] {
        assert_equal 1 [$primary glearn $p:worse $id CORRECT 0]
    }
    gr_expect $primary $replicas $p:worse {state ACTIVE generation 1 audit-samples 21 audit-errors 11 pending 19 revocations 0} \
        "8 disagreements in a row at the end of 40 spot checks"
    set order [gr_shuffle [lrange $waiting 0 end-8]]
    foreach id [lrange $order 0 7] {
        assert_equal 1 [$primary glearn $p:worse $id CORRECT 0]
    }
    gr_expect $primary $replicas $p:worse {state ACTIVE generation 1 audit-samples 29 audit-errors 19 pending 11 revocations 0} \
        "19 disagreements among the 40 spot checks of a block"
    assert_equal 1 [$primary glearn $p:worse [lindex $order 8] CORRECT 0]
    gr_expect $primary $replicas $p:worse {state CALIBRATING generation 0 candidate 2 audit-samples 0 audit-errors 0 pending 0 revocations 1} \
        "the 20th disagreement among the 40 spot checks of a block"
    foreach id [lrange $order 9 end] {
        assert_equal 0 [$primary glearn $p:worse $id CORRECT 1] "GLEARN $p:worse $id, a spot check of a snapshot that was withdrawn"
    }

    # block: spot checks 65 to 72 are those of the open block.
    set open [dict get $story block]
    assert_equal 8 [llength $open]
    assert {![gr_drift_reached 0.2 8 7 1000] && [gr_drift_reached 0.2 8 8 1000]}
    foreach id [lrange $open 0 6] {
        assert_equal 1 [$primary glearn $p:block $id CORRECT 0]
    }
    gr_expect $primary $replicas $p:block {state ACTIVE generation 1 audit-samples 13 audit-errors 13 pending 59 revocations 0} \
        "6 disagreements in a block that has ended and 7 among the 8 spot checks of the open one"
    assert_equal 1 [$primary glearn $p:block [lindex $open 7] CORRECT 0]
    gr_expect $primary $replicas $p:block {state CALIBRATING generation 0 candidate 2 pending 0 revocations 1} \
        "the 8th disagreement among the 8 spot checks of the open block"

    # mark: asked in the first half, given up only now.
    gr_wait_age [dict get $story asked] $timeout
    assert_equal OK [$primary gresume $p:mark]
    gr_expect $primary $replicas $p:mark {state CALIBRATING generation 0 candidate 1 missing 32 pending 0 calibration-samples 33 revocations 0} \
        "20 lost answers, an answer, 12 lost answers, all given up at once"

    # swap
    set actions [gr_takeover_known $primary $p:swap 60 40]
    assert_equal {} [lsearch -all -not $actions AUDIT] "requests of the comparison that the answering snapshot of $p:swap left to the teacher"
    gr_expect $primary $replicas $p:swap {state ACTIVE generation 1 candidate 2 revocations 0} \
        "100 requests, all but one answered by the snapshot that answers"
    assert {[lindex [gr_field $primary $p:swap thresholds] 0] > 0.5}
    lassign [$primary gpredict $p:swap TEXT [gr_text spam2 0]] action id
    assert_equal ABSTAIN $action "a request in a vocabulary the answering snapshot of $p:swap does not know"
    assert_equal 1 [$primary glearn $p:swap $id LABEL spam]
    gr_expect $primary $replicas $p:swap {state ACTIVE generation 2 candidate 3 thresholds {0.5 0.5} revocations 0} \
        "the first request the answering snapshot left to the teacher"

    # keep
    set budget [expr {[gr_right_answers 0.2 0.2 2 2] * 10 * 2}]
    set actions [gr_takeover_known $primary $p:keep 60 [expr {$budget - 60 - 1}]]
    assert_equal {} [lsearch -all -not $actions AUDIT] "requests of the comparison that the answering snapshot of $p:keep left to the teacher"
    gr_expect $primary $replicas $p:keep {state ACTIVE generation 1 candidate 2 revocations 0} \
        "[expr {$budget - 1}] pieces of proof for a snapshot that cannot answer more"
    gr_takeover_known $primary $p:keep [expr {$budget - 1}] 1
    # Every request but one was a spot check, and the answering snapshot
    # agreed with the teacher on each: it took every text for what it was.
    gr_expect $primary $replicas $p:keep [dict create state ACTIVE generation 1 candidate 3 revocations 0 \
        audit-samples [expr {$budget - 1}] audit-errors 0] "$budget pieces of proof for a snapshot that cannot answer more"
    # So its detector for 'ham' has numbered every request of that label
    # but the one that was no spot check. From now on the teacher calls
    # them 'spam'.
    set detector [gr_detector 0.2]
    for {set n 0} {$n < $budget / 2 - 1} {incr n} {
        gr_detector_ask detector
    }
    set verdict quiet
    set wrong 0
    while {$verdict eq "quiet" && $wrong < 100} {
        incr wrong
        lassign [$primary gpredict $p:keep TEXT [gr_text ham [expr {$budget + $wrong}]]] action id
        assert_equal AUDIT $action "GPREDICT $p:keep, a text the answering snapshot knows"
        set verdict [gr_detector_count detector [gr_detector_ask detector] wrong]
        if {$verdict ne "quiet"} {
            gr_expect $primary $replicas $p:keep [dict create state ACTIVE generation 1 revocations 0 audit-errors [expr {$wrong - 1}]] \
                "[expr {$wrong - 1}] disagreements about 'ham'"
        }
        assert_equal 1 [$primary glearn $p:keep $id LABEL spam]
    }
    assert_equal worse $verdict
    assert_equal 16 $wrong "disagreements about 'ham' that it takes to stop the gate"
    # It got worse: a classifier forgets its model as well.
    gr_expect $primary $replicas $p:keep {state COLD generation 0 candidate 0 learned 0 revocations 1} \
        "the 16th disagreement about 'ham'"

    # defer
    set budget [expr {[gr_right_answers 0.2 0.2 1 1] * 10}]
    set next [gr_defer_requests $primary $p:defer [dict get $story defer] [expr {$budget - 200 - 1}]]
    gr_expect $primary $replicas $p:defer {state CALIBRATING generation 0 candidate 1 calibration-samples 0} \
        "[expr {$budget - 1}] requests a snapshot left to the teacher although the gate may answer their label"
    set next [gr_defer_requests $primary $p:defer $next 1]
    gr_expect $primary $replicas $p:defer {state CALIBRATING generation 0 candidate 2 calibration-samples 0} \
        "$budget requests a snapshot left to the teacher although the gate may answer their label"
    set again [gr_right_answers 0.2 0.2 1 2]
    set next [gr_defer_requests $primary $p:defer $next [expr {$again - 1}]]
    gr_expect $primary $replicas $p:defer [dict create state CALIBRATING generation 0 candidate 2 calibration-samples [expr {$again - 1}]] \
        "[expr {$again - 1}] right answers for the second snapshot"
    set next [gr_defer_requests $primary $p:defer $next 1]
    gr_expect $primary $replicas $p:defer {state ACTIVE generation 2 candidate 3 thresholds {0.5 {}}} \
        "$again right answers for the second snapshot"
    # The third snapshot is compared with the second, which answers every
    # request of the label the gate may answer.
    set budget [expr {[gr_right_answers 0.2 0.2 1 3] * 10}]
    set next [gr_defer_requests $primary $p:defer $next [expr {$budget - 1}]]
    gr_expect $primary $replicas $p:defer {state ACTIVE generation 2 candidate 3 revocations 0} \
        "[expr {$budget - 1}] pieces of proof for the third snapshot"
    # Meanwhile the model is taught, from history, that all of it is
    # 'other': the snapshot taken next knows nothing else.
    for {set n 0} {$n < 300} {incr n} {
        $primary gadd $p:defer TEXT [gr_text billing $n] LABEL other
    }
    set next [gr_defer_requests $primary $p:defer $next 1]
    gr_expect $primary $replicas $p:defer {state ACTIVE generation 2 candidate 4 calibration-samples 0 revocations 0} \
        "$budget pieces of proof for the third snapshot"
    set budget [expr {[gr_right_answers 0.2 0.2 1 4] * 10}]
    set next [gr_defer_requests $primary $p:defer $next [expr {$budget - 1}]]
    # Not one of them was proof for it.
    gr_expect $primary $replicas $p:defer {state ACTIVE generation 2 candidate 4 calibration-samples 0 revocations 0} \
        "[expr {$budget - 1}] spot checks the fourth snapshot left to the teacher"
    gr_defer_requests $primary $p:defer $next 1
    gr_expect $primary $replicas $p:defer {state ACTIVE generation 2 candidate 5 revocations 0} \
        "$budget spot checks the fourth snapshot left to the teacher"

    # lost, run and clear
    gr_wait_age $asked $timeout
    assert_equal OK [$primary gresume $p:lost]
    assert_equal OK [$primary gresume $p:run]
    assert_equal OK [$primary gresume $p:clear]
    gr_expect $primary $replicas $p:clear {state CALIBRATING generation 0 candidate 1 missing 62 pending 0 calibration-samples 63 revocations 0} \
        "31 requests in a row without an answer, an answer, and 31 more"
    gr_expect $primary $replicas $p:lost {state COLD generation 0 candidate 0 missing 20 pending 0 revocations 1} \
        "the 20th lost spot check of a block"
    gr_expect $primary $replicas $p:run {state COLD generation 0 candidate 0 missing 32 pending 0 revocations 0} \
        "the 32nd request in a row without an answer"
    foreach key [list $p:lost $p:run] {
        lassign [$primary gpredict $key SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key, the first answer since the gate stopped"
        gr_expect $primary $replicas $key {state CALIBRATING generation 0 candidate 2} "an answer for a gate that had stopped"
    }
    gr_phase $primary $replicas [gr_story_keys $p] "$p at the end"
}
# ---------------------------------------------- a replica attached at birth
#
# Three servers in a chain: primary -> replica -> sub-replica. Both replicas
# are attached before any gate exists, so everything they know about a gate
# came through the replication stream. The replica keeps an AOF.

start_server [list tags {"modules external:skip gate:chain"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule appendonly yes auto-aof-rewrite-percentage 0]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    gr_watch -1 -2

    gr_test {Gate replication: replicas attach before any gate exists} {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 20
        $primary config set gate.feedback-timeout 600000
        # What a gate is created with travels in the stream. A replica that
        # read its own settings, rolled its own dice or looked at its own
        # clock would not end up with the gate of its primary.
        $replica config set gate.snapshot-examples 3
        $replica config set gate.feedback-timeout 1
        $replica config set gate.test-seed 7

        $replica replicaof $primary_host $primary_port
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $replica
        wait_for_sync $subreplica
        waitForBgrewriteaof $replica
        assert_equal 0 [$primary dbsize]
        assert_equal 0 [$replica dbsize]
        assert_equal 0 [$subreplica dbsize]
    }

    gr_test {Gate replication: GCREATE gives every replica the gate of the primary} {
        assert_equal OK [$primary gcreate sc:plain ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate sc:audit ALPHA 0.2 SCORE AUDIT 1 DELTA 0.2]
        assert_equal OK [$primary gcreate cl:two ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        # A classifier that takes its first snapshot late, once it has
        # learned from history.
        $primary config set gate.snapshot-examples 150
        assert_equal OK [$primary gcreate cl:three AUDIT 0.2 CLASSES 3 billing tech other \
            ALWAYSDEFER 1 other CLASSALPHA 1 billing 0.1 DELTA 0.2 ALPHA 0.2]
        $primary config set gate.snapshot-examples 20
        # The largest configuration: 64 labels, among them labels that spell
        # option names, labels with spaces and labels that are not ASCII.
        # (The two last ones are UTF-8, byte by byte: this file is ASCII.)
        set labels [list ALPHA SCORE CLASSES LABEL TEXT {two words} "caf\xc3\xa9" "\xe6\x97\xa5\xe6\x9c\xac"]
        lappend labels [string repeat x 64]
        for {set n [llength $labels]} {$n < 64} {incr n} {
            lappend labels "label-$n"
        }
        set classalpha {}
        foreach label [lrange $labels 0 39] {
            lappend classalpha $label 0.01
        }
        assert_equal OK [$primary gcreate cl:wide ALPHA 0.05 CLASSES 64 {*}$labels \
            CLASSALPHA 40 {*}$classalpha ALWAYSDEFER 24 {*}[lrange $labels 40 end]]
        set gates {sc:plain sc:audit cl:two cl:three cl:wide}

        gr_assert_same $primary $replicas $gates "after GCREATE"
        foreach node $replicas {
            foreach key $gates {
                assert_equal gate-type [$node type $key]
            }
            assert_equal score [gr_field $node sc:plain kind]
            assert_equal 1 [gr_field $node sc:audit audit]
            assert_equal {spam ham} [gr_field $node cl:two classes]
            assert_equal {billing tech other} [gr_field $node cl:three classes]
            assert_equal {0.1 0.2 0} [gr_field $node cl:three alphas]
            assert_equal 64 [llength [gr_field $node cl:wide classes]]
        }

        # The widest gate learns a little, takes its first snapshot and
        # has requests waiting: megabytes of weights on every node.
        for {set n 0} {$n < 26} {incr n} {
            $primary gadd cl:wide TEXT "sample number $n of [lindex $labels $n]" LABEL [lindex $labels $n]
        }
        for {set n 0} {$n < 3} {incr n} {
            lassign [$primary gpredict cl:wide TEXT "sample number $n"] action id
            assert_equal ABSTAIN $action
            if {$n > 0} {
                assert_equal 1 [$primary glearn cl:wide $id LABEL [lindex $labels $n]]
            }
        }
        gr_assert_same $primary $replicas cl:wide "the widest gate"
        foreach node $replicas {
            assert_equal 28 [gr_field $node cl:wide learned]
            assert_equal 1 [gr_field $node cl:wide candidate]
        }

        # A GCREATE that is refused creates nothing anywhere.
        assert_error {BUSYKEY*} {$primary gcreate sc:plain ALPHA 0.5 SCORE}
        assert_error {ERR*} {$primary gcreate no:gate ALPHA 2 SCORE}
        wait_for_ofs_sync $primary $replica
        wait_for_ofs_sync $primary $subreplica
        foreach node $nodes {
            assert_equal 0 [$node exists no:gate]
        }
        gr_assert_same $primary $replicas $gates "after a refused GCREATE"
    }

    gr_test {Gate replication: a score gate becomes ACTIVE on the replicas as on the primary} {
        for {set n 0} {$n < 10} {incr n} {
            gr_score_request $primary sc:plain [lindex $::gr_scores $n]
        }
        gr_assert_same $primary $replicas sc:plain "while calibrating"
        foreach node $replicas {
            assert_equal CALIBRATING [gr_field $node sc:plain state]
            assert_equal 1 [gr_field $node sc:plain candidate]
        }

        gr_score_until_active $primary sc:plain
        gr_assert_same $primary $replicas sc:plain "once ACTIVE"
        foreach node $replicas {
            assert_equal ACTIVE [gr_field $node sc:plain state]
            assert_equal 1 [gr_field $node sc:plain generation]
            assert {[lindex [gr_field $node sc:plain thresholds] 0] ne {}}
        }

        # The gate answers now. What it accepts never reaches the replicas,
        # its spot checks and their answers do.
        set accepted 0
        for {set n 0} {$n < 300} {incr n} {
            set score [lindex $::gr_scores [expr {$n % [llength $::gr_scores]}]]
            if {[gr_score_request $primary sc:plain $score] eq "ACCEPT"} {
                incr accepted
            }
        }
        assert {$accepted > 0}
        gr_assert_same $primary $replicas sc:plain "while answering"
        assert_equal $accepted [gr_field $primary sc:plain accepted]
    }

    gr_test {Gate replication: a classifier gate becomes ACTIVE on the replicas as on the primary} {
        # History first. GADD is never proof, but it moves the model, and
        # the model of every node.
        set learned 0
        for {set n 0} {$n < 15} {incr n} {
            foreach label {spam ham} {
                assert_equal [incr learned] [$primary gadd cl:two TEXT [gr_text $label $n] LABEL $label]
            }
        }
        gr_assert_same $primary $replicas cl:two "after GADD"
        foreach node $replicas {
            assert_equal 30 [gr_field $node cl:two learned]
            assert_equal 0 [gr_field $node cl:two calibration-samples]
        }

        gr_text_until_active $primary cl:two {spam ham}
        gr_assert_same $primary $replicas cl:two "once ACTIVE"
        foreach node $replicas {
            assert_equal ACTIVE [gr_field $node cl:two state]
            assert {[gr_field $node cl:two generation] > 0}
        }

        set accepted 0
        for {set n 0} {$n < 300} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            if {[gr_text_request $primary cl:two [gr_text $label $n] $label] eq "ACCEPT"} {
                incr accepted
            }
        }
        assert {$accepted > 0}
        gr_assert_same $primary $replicas cl:two "while answering"
    }

    gr_test {Gate replication: a classifier with ALWAYSDEFER and CLASSALPHA, taught with GADD} {
        for {set n 0} {$n < 50} {incr n} {
            foreach label {billing tech other} {
                # Named features are part of an example.
                $primary gadd cl:three TEXT [gr_text $label $n] \
                    FEATURES 2 urgent [expr {$label eq "tech"}] depth [expr {($n % 5) / 5.0}] LABEL $label
            }
        }
        gr_assert_same $primary $replicas cl:three "after GADD"
        foreach node $replicas {
            assert_equal 150 [gr_field $node cl:three learned]
            # The first snapshot is taken after 150 examples, as the primary
            # was told when the gate was created.
            assert_equal 1 [gr_field $node cl:three candidate]
        }

        gr_text_until_active $primary cl:three {billing tech other}
        for {set n 0} {$n < 300} {incr n} {
            set label [lindex {billing tech other} [expr {$n % 3}]]
            gr_text_request $primary cl:three [gr_text $label $n] $label
        }
        gr_assert_same $primary $replicas cl:three "once ACTIVE"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node cl:three state]
            # Never answered: always deferred.
            assert_equal {} [lindex [gr_field $node cl:three thresholds] 2]
        }
    }

    gr_test {Gate replication: a newer snapshot takes over on the replicas as on the primary} {
        # Every request this gate could answer is a spot check (AUDIT 1),
        # so what follows does not depend on the dice.
        $primary config set gate.snapshot-examples 40
        assert_equal OK [$primary gcreate cl:swap ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham]
        $primary config set gate.snapshot-examples 20
        # The first snapshot is taken of a model that knows one of the two
        # vocabularies its requests are written in.
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd cl:swap TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_assert_same $primary $replicas cl:swap "the first snapshot is tested"
        foreach node $nodes {
            assert_equal CALIBRATING [gr_field $node cl:swap state]
            assert_equal 1 [gr_field $node cl:swap candidate]
        }
        # About the other vocabulary the first snapshot can only guess.
        # Every threshold is a test of its own: the snapshot proves the
        # thresholds its guesses fall short of, and answers for what it is
        # sure of.
        set vocabularies {{spam spam} {ham ham} {spam2 spam} {ham2 ham}}
        for {set n 0} {$n < 2000 && [gr_field $primary cl:swap generation] == 0} {incr n} {
            lassign [lindex $vocabularies [expr {$n % 4}]] words label
            gr_text_request $primary cl:swap [gr_text $words $n] $label
        }
        set generation [gr_field $primary cl:swap generation]
        assert {$generation > 0}
        gr_assert_same $primary $replicas cl:swap "the first snapshot answers"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node cl:swap state]
            assert_equal $generation [gr_field $node cl:swap generation]
            # Meanwhile the model has learned both vocabularies from the
            # answers: its next snapshot is tested at once.
            assert {[gr_field $node cl:swap candidate] > $generation}
        }

        # The next snapshot answers more of the same requests, and takes
        # over.
        for {set n 0} {$n < 4000 && [gr_field $primary cl:swap generation] == $generation} {incr n} {
            lassign [lindex $vocabularies [expr {$n % 4}]] words label
            assert {[gr_text_request $primary cl:swap [gr_text $words $n] $label] ne "ACCEPT"}
        }
        gr_assert_same $primary $replicas cl:swap "the next snapshot took over"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node cl:swap state]
            assert {[gr_field $node cl:swap generation] > $generation}
            # It took over: the first one was not withdrawn.
            assert_equal 0 [gr_field $node cl:swap revocations]
        }
    }

    gr_test {Gate replication: feedback out of order, repeated and unknown} {
        assert_equal OK [$primary gcreate sc:order ALPHA 0.2 DELTA 0.2 SCORE]
        set ids {}
        for {set n 0} {$n < 60} {incr n} {
            lassign [$primary gpredict sc:order SCORE 0.9] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        gr_assert_same $primary $replicas sc:order "60 requests waiting"

        # Newest first, and not the oldest: every answer is used, but proof
        # is applied in request order, so all of them wait for the oldest.
        foreach id [lreverse [lrange $ids 1 end]] {
            assert_equal 1 [$primary glearn sc:order $id CORRECT 1]
        }
        gr_assert_same $primary $replicas sc:order "59 answers waiting for an older one"
        foreach node $nodes {
            assert_equal 0 [gr_field $node sc:order calibration-samples]
            assert_equal CALIBRATING [gr_field $node sc:order state]
        }

        # The oldest answer lets all of them through, in order. Half way
        # the gate has its proof and starts answering; the rest was asked
        # of the same snapshot and is proof for it all the same.
        assert_equal 1 [$primary glearn sc:order [lindex $ids 0] CORRECT 1]
        gr_assert_same $primary $replicas sc:order "all answered"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node sc:order state]
            assert_equal 1 [gr_field $node sc:order generation]
            assert_equal 60 [gr_field $node sc:order calibration-samples]
            assert_equal 0 [gr_field $node sc:order pending]
        }

        # A gate may have a time to live.
        assert_equal 1 [$primary expire sc:order 100000]

        # Retrying is safe, and so is an id that means nothing here.
        foreach id $ids {
            assert_equal 0 [$primary glearn sc:order $id CORRECT 0]
        }
        foreach id $::gr_bogus_ids {
            assert_equal 0 [$primary glearn sc:order $id CORRECT 1]
        }
        lassign [$primary gpredict sc:plain SCORE 0.01] action other
        if {$other ne {}} {
            assert_equal 0 [$primary glearn sc:order $other CORRECT 1]
            assert_equal 1 [$primary glearn sc:plain $other CORRECT 0]
        }
        gr_assert_same $primary $replicas {sc:order sc:plain} "after retries"
        foreach node $nodes {
            assert_equal 60 [gr_field $node sc:order calibration-samples]
        }

        # A classifier that has no snapshot yet learns from its answers in
        # the order they arrive: the replicas must see them in that order.
        assert_equal OK [$primary gcreate cl:order ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        set asked {}
        for {set n 0} {$n < 12} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            lassign [$primary gpredict cl:order TEXT [gr_text $label $n]] action id
            assert_equal ABSTAIN $action
            lappend asked [list $id $label]
        }
        foreach n {7 2 11 0 5 9 1 10 3 8 6 4} {
            lassign [lindex $asked $n] id label
            assert_equal 1 [$primary glearn cl:order $id LABEL $label]
        }
        gr_assert_same $primary $replicas cl:order "classifier taught out of order"
        foreach node $nodes {
            assert_equal 12 [gr_field $node cl:order learned]
        }
    }

    gr_test {Gate replication: feedback that never comes is missed by every node at the same time} {
        $primary config set gate.feedback-timeout 150
        set timed {to:predict to:learn to:pause to:resume}
        foreach key $timed {
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 SCORE]
        }
        assert_equal OK [$primary gcreate to:add ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        assert_equal OK [$primary gcreate to:active ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        $primary config set gate.feedback-timeout 600000
        assert_equal OK [$primary gpause to:resume]

        # Five requests to each gate, never answered.
        set lost [dict create]
        foreach key $timed {
            for {set n 0} {$n < 5} {incr n} {
                lassign [$primary gpredict $key SCORE 0.9] action id
                assert_equal ABSTAIN $action
                dict lappend lost $key $id
            }
        }
        for {set n 0} {$n < 5} {incr n} {
            lassign [$primary gpredict to:add TEXT [gr_text spam $n]] action id
            assert_equal ABSTAIN $action
            dict lappend lost to:add $id
        }
        gr_assert_same $primary $replicas [concat $timed to:add] "requests waiting"

        # The replicas do not watch the clock. They learn that the requests
        # waited too long when the primary finds out: at its next write to
        # the gate, whatever that write is.
        after 200
        lassign [$primary gpredict to:predict SCORE 0.9] action fresh
        assert_equal 0 [$primary glearn to:learn [lindex [dict get $lost to:learn] 0] CORRECT 1]
        assert_equal OK [$primary gpause to:pause]
        assert_equal OK [$primary gresume to:resume]
        $primary gadd to:add TEXT [gr_text ham 1] LABEL ham
        gr_assert_same $primary $replicas [concat $timed to:add] "requests timed out"
        foreach node $nodes {
            foreach key $timed {
                # Each request was needed as proof and never got its GLEARN.
                assert_equal 5 [gr_field $node $key missing] "missing of $key"
            }
        }
        dict for {key ids} $lost {
            foreach id $ids {
                if {$key eq "to:add"} {
                    assert_equal 0 [$primary glearn $key $id LABEL spam] "GLEARN $key for a request that timed out"
                } else {
                    assert_equal 0 [$primary glearn $key $id CORRECT 1] "GLEARN $key for a request that timed out"
                }
            }
        }
        gr_assert_same $primary $replicas [concat $timed to:add] "late feedback"

        # An answering gate: most requests are accepted and replicate
        # nothing by themselves, yet the replicas must learn about the spot
        # checks that were never answered.
        for {set n 0} {$n < 600 && [gr_field $primary to:active state] ne "ACTIVE"} {incr n} {
            lassign [$primary gpredict to:active SCORE 0.9] action id
            $primary glearn to:active $id CORRECT 1
        }
        assert_equal ACTIVE [gr_field $primary to:active state]
        for {set round 1} {$round <= 3} {incr round} {
            for {set n 0} {$n < 300} {incr n} {
                lassign [$primary gpredict to:active SCORE 0.9] action id
                if {$action eq "AUDIT"} break
            }
            assert_equal AUDIT $action
            after 200
            # Accepted or audited, this request finds the overdue spot check.
            lassign [$primary gpredict to:active SCORE 0.9] action id
            if {$action eq "AUDIT"} {
                $primary glearn to:active $id CORRECT 1
            }
            gr_assert_same $primary $replicas to:active "round $round of overdue spot checks"
        }
        assert_equal OK [$primary gpause to:active]
        assert_equal OK [$primary gresume to:active]
        gr_assert_same $primary $replicas to:active "overdue spot checks"
        foreach node $nodes {
            # (More than three only if this test was stalled for longer
            # than the feedback timeout between a request and its answer.)
            assert {[gr_field $node to:active missing] >= 3}
        }
    }

    gr_test {Gate replication: a drift alarm stops the gate on the replicas too} {
        # sc:audit checks every request it could answer (AUDIT 1).
        gr_score_until_active $primary sc:audit
        gr_assert_same $primary $replicas sc:audit "before the alarm"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node sc:audit state]
            assert_equal 0 [gr_field $node sc:audit revocations]
        }
        # From now on the teacher disagrees with every answer.
        for {set n 0} {$n < 200 && [gr_field $primary sc:audit revocations] == 0} {incr n} {
            lassign [$primary gpredict sc:audit SCORE 0.95] action id
            assert {$action ne "ACCEPT"}
            if {$id ne {}} {
                assert_equal 1 [$primary glearn sc:audit $id CORRECT 0]
            }
        }
        gr_assert_same $primary $replicas sc:audit "after the alarm"
        foreach node $nodes {
            assert_equal 1 [gr_field $node sc:audit revocations]
            assert {[gr_not_answering [gr_info $node sc:audit]]}
        }

        # A classifier forgets its model as well.
        assert_equal ACTIVE [gr_field $primary cl:two state]
        for {set n 0} {$n < 5000 && [gr_field $primary cl:two revocations] == 0} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            set wrong [lindex {ham spam} [expr {$n % 2}]]
            lassign [$primary gpredict cl:two TEXT [gr_text $label $n]] action id
            if {$action eq "AUDIT"} {
                assert_equal 1 [$primary glearn cl:two $id LABEL $wrong]
            } elseif {$action eq "ABSTAIN" && $id ne {}} {
                assert_equal 1 [$primary glearn cl:two $id LABEL $label]
            }
        }
        gr_assert_same $primary $replicas cl:two "after the alarm of a classifier"
        foreach node $nodes {
            assert_equal 1 [gr_field $node cl:two revocations]
            assert {[gr_not_answering [gr_info $node cl:two]]}
            assert_equal 0 [gr_field $node cl:two learned]
        }
    }

    gr_test {Gate replication: GPAUSE, GRESUME and GRESET reach the replicas} {
        assert_equal ACTIVE [gr_field $primary sc:plain state]
        assert_equal OK [$primary gpause sc:plain]
        gr_assert_same $primary $replicas sc:plain "paused"
        foreach node $nodes {
            assert_equal PAUSED [gr_field $node sc:plain state]
        }
        # A paused gate answers nothing and keeps proving itself.
        for {set n 0} {$n < 60} {incr n} {
            set score [lindex $::gr_scores [expr {$n % [llength $::gr_scores]}]]
            assert {[gr_score_request $primary sc:plain $score] ne "ACCEPT"}
        }
        gr_assert_same $primary $replicas sc:plain "requests while paused"

        assert_equal OK [$primary gresume sc:plain]
        gr_assert_same $primary $replicas sc:plain "resumed"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node sc:plain state]
        }

        # GRESET: everything learned and proven is gone, and so are the
        # requests that were waiting.
        set waiting {}
        for {set n 0} {$n < 5} {incr n} {
            lassign [$primary gpredict cl:three TEXT [gr_text other $n]] action id
            if {$id ne {}} {
                lappend waiting cl:three $id LABEL other
            }
            lassign [$primary gpredict sc:order SCORE 0.9] action id
            if {$id ne {}} {
                lappend waiting sc:order $id CORRECT 1
            }
        }
        assert {[llength $waiting] > 0}
        set generation [gr_field $primary cl:three generation]
        assert_equal OK [$primary greset cl:three]
        assert_equal OK [$primary greset sc:order]
        assert_equal OK [$primary greset sc:plain]
        gr_assert_same $primary $replicas {cl:three sc:order sc:plain} "reset"
        foreach node $nodes {
            assert_equal 0 [gr_field $node cl:three learned]
            assert_equal 0 [gr_field $node cl:three pending]
            assert_equal 0 [gr_field $node sc:order pending]
            foreach key {cl:three sc:order sc:plain} {
                assert {[gr_not_answering [gr_info $node $key]]}
            }
            # The configuration stays.
            assert_equal {billing tech other} [gr_field $node cl:three classes]
            assert_equal {0.1 0.2 0} [gr_field $node cl:three alphas]
        }
        foreach {key id word answer} $waiting {
            assert_equal 0 [$primary glearn $key $id $word $answer] "GLEARN of a request made before GRESET"
        }
        gr_assert_same $primary $replicas {cl:three sc:order sc:plain} "feedback from before the reset"

        # A snapshot number is never reused, on any node.
        gr_score_until_active $primary sc:plain
        gr_assert_same $primary $replicas sc:plain "ACTIVE again after GRESET"
        foreach node $nodes {
            assert_equal 2 [gr_field $node sc:plain generation]
        }
    }

    gr_test {Gate replication: a mixed workload of several thousand commands} {
        assert_equal OK [$primary gcreate mix:s1 ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate mix:s2 ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        $primary config set gate.snapshot-examples 60
        assert_equal OK [$primary gcreate mix:c1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        assert_equal OK [$primary gcreate mix:c2 ALPHA 0.2 DELTA 0.2 CLASSES 3 billing tech other ALWAYSDEFER 1 other]
        $primary config set gate.feedback-timeout 100
        assert_equal OK [$primary gcreate mix:lossy:s ALPHA 0.2 DELTA 0.2 AUDIT 0.3 SCORE]
        assert_equal OK [$primary gcreate mix:lossy:c ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 600000
        $primary config set gate.snapshot-examples 20
        # The classifiers start from some history, so that their first
        # snapshot is worth testing.
        for {set n 0} {$n < 30} {incr n} {
            foreach label {spam ham} {
                $primary gadd mix:c1 TEXT [gr_text $label $n] LABEL $label
                $primary gadd mix:lossy:c TEXT [gr_text $label $n] LABEL $label
            }
            foreach label {billing tech} {
                $primary gadd mix:c2 TEXT [gr_text $label $n] LABEL $label
            }
        }
        set mix {
            {mix:s1 score {} 0}
            {mix:s2 score {} 0}
            {mix:c1 text {spam ham} 0}
            {mix:c2 text {billing tech other} 0}
            {mix:lossy:s score {} 1}
            {mix:lossy:c text {spam ham} 1}
            {sc:audit score {} 0}
            {cl:two text {spam ham} 0}
        }
        set mixkeys {}
        foreach gate $mix {
            lappend mixkeys [lindex $gate 0]
        }

        for {set round 1} {$round <= 10} {incr round} {
            gr_workload $primary $mix 800
            # Whatever was paused answers again in the next round.
            foreach key $mixkeys {
                $primary gresume $key
            }
            gr_assert_same $primary $replicas $mixkeys "mixed workload, round $round"
            switch $round {
                2 {
                    # Long enough for the lossy gates to miss their feedback.
                    after 150
                }
                4 {
                    # The replica starts over from what it would save.
                    assert_equal OK [$replica debug reload]
                    gr_assert_same $primary $replicas $mixkeys "replica reloaded"
                }
                7 {
                    assert_equal OK [$primary greset mix:s2]
                    assert_equal OK [$primary greset mix:c1]
                    gr_workload_forget {mix:s2 mix:c1}
                }
            }
        }
        # The workload went through every kind of reply, and through
        # feedback that came too late.
        foreach {what count} [array get ::gr_seen] {
            assert {$count > 0}
        }
        assert {[gr_field $primary mix:lossy:s missing] > 0}
        set allgates [concat $mixkeys $timed {sc:plain sc:order cl:three cl:order cl:swap cl:wide to:add to:active}]
        gr_assert_same $primary $replicas $allgates "after the mixed workload"
    }

    gr_test {Gate replication: RENAME, EXPIRE, DEL, MOVE, DUMP and RESTORE of a gate} {
        assert_equal OK [$primary gcreate tmp:a ALPHA 0.2 DELTA 0.2 SCORE]
        lassign [$primary gpredict tmp:a SCORE 0.9] action id
        assert_equal OK [$primary rename tmp:a tmp:b]
        # An id belongs to the gate, not to its name.
        assert_equal 1 [$primary glearn tmp:b $id CORRECT 1]
        assert_equal 1 [$primary expire tmp:b 3600]
        gr_assert_same $primary $replicas tmp:b "renamed gate"
        foreach node $replicas {
            assert_equal 0 [$node exists tmp:a]
            assert {[$node ttl tmp:b] > 0}
        }

        # A copy made with DUMP and RESTORE reaches the replicas as it is,
        # with a request that waits in the original.
        # (sc:audit checks everything it could answer and nothing can be
        # more confident than 1: this request always waits for feedback.)
        gr_workload_drain $primary
        lassign [$primary gpredict sc:audit SCORE 1] action id
        assert {$id ne {}}
        assert_equal OK [$primary restore tmp:copy 0 [$primary dump sc:audit]]
        gr_assert_same_digests $primary $replicas {tmp:copy sc:audit} "restored gate"
        # The primary cannot tell what the copy went through: at its first
        # write the copy drops its proof and its waiting requests, on every
        # node. The original is not concerned.
        assert_equal 0 [$primary glearn tmp:copy $id CORRECT 1]
        assert_equal 1 [$primary glearn sc:audit $id CORRECT 1]
        gr_assert_same $primary $replicas {tmp:copy sc:audit} "restored gate after its first write"
        foreach node $nodes {
            assert {[gr_not_answering [gr_info $node tmp:copy]]}
        }
        lassign [$primary gpredict tmp:copy SCORE 1] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn tmp:copy $id CORRECT 1]
        gr_assert_same $primary $replicas {tmp:copy sc:audit} "requests to the restored gate"

        assert_equal 2 [$primary del tmp:b tmp:copy]
        wait_for_ofs_sync $primary $replica
        wait_for_ofs_sync $primary $subreplica
        foreach node $replicas {
            assert_equal 0 [$node exists tmp:b tmp:copy]
        }

        # A gate created under the name of a deleted one is a new gate.
        assert_equal OK [$primary gcreate tmp:b ALPHA 0.1 CLASSES 2 yes no]
        gr_assert_same $primary $replicas tmp:b "gate created again"
        foreach node $replicas {
            assert_equal classifier [gr_field $node tmp:b kind]
        }
        assert_equal 1 [$primary del tmp:b]

        # A key name is any bytes.
        set odd "tmp:a name\x00with\r\nany bytes \xff"
        assert_equal OK [$primary gcreate $odd ALPHA 0.2 DELTA 0.2 SCORE]
        lassign [$primary gpredict $odd SCORE 0.9] action id
        assert_equal 1 [$primary glearn $odd $id CORRECT 1]
        gr_assert_same $primary $replicas [list $odd] "gate with an odd name"
        assert_equal 1 [$primary del $odd]

        # A gate moved to another database keeps its waiting requests
        # there. A gate created here under its name is another gate.
        set elsewhere {}
        foreach level {0 -1 -2} {
            set client [redis_client $level]
            $client select 10
            lappend elsewhere $client
        }
        assert_equal OK [$primary gcreate tmp:db ALPHA 0.2 DELTA 0.2 SCORE]
        lassign [$primary gpredict tmp:db SCORE 0.9] action id
        assert_equal 1 [$primary move tmp:db 10]
        assert_equal OK [$primary gcreate tmp:db ALPHA 0.1 CLASSES 2 yes no]
        assert_equal 0 [$primary glearn tmp:db $id LABEL yes]
        assert_equal 1 [[lindex $elsewhere 0] glearn tmp:db $id CORRECT 1]
        gr_assert_same $primary $replicas tmp:db "gate created under the name of one that moved away"
        gr_assert_same [lindex $elsewhere 0] [lrange $elsewhere 1 end] tmp:db "gate moved to another database"
        foreach node $replicas client [lrange $elsewhere 1 end] {
            assert_equal classifier [gr_field $node tmp:db kind]
            assert_equal score [gr_field $client tmp:db kind]
            assert_equal 1 [gr_field $client tmp:db calibration-samples]
        }
        assert_equal 1 [$primary del tmp:db]
        assert_equal 1 [[lindex $elsewhere 0] del tmp:db]
        foreach client $elsewhere {
            $client close
        }
    }

    gr_test {Gate replication: the stream carries what happened, never the commands} {
        set repl [attach_to_replication_stream]
        set changes 0

        assert_equal OK [$primary gcreate st:score ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate st:text ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        assert_equal 1 [$primary gadd st:text TEXT [gr_text spam 1] LABEL spam]
        lassign [$primary gpredict st:score SCORE 0.9] action id
        assert_equal 1 [$primary glearn st:score $id CORRECT 1]
        set numbered [dict create st:score [lindex [split $id -] 1]]
        lassign [$primary gpredict st:text TEXT [gr_text ham 1] FEATURES 1 depth 0.5] action id
        assert_equal 1 [$primary glearn st:text $id LABEL ham]
        dict set numbered st:text [lindex [split $id -] 1]
        assert_equal OK [$primary gpause st:score]
        assert_equal OK [$primary gresume st:score]
        incr changes 9

        # A transaction.
        $primary multi
        $primary gpredict st:score SCORE 0.8
        $primary gadd st:text TEXT [gr_text ham 2] LABEL ham
        set replies [$primary exec]
        assert_equal ABSTAIN [lindex $replies 0 0]
        assert_equal 1 [$primary glearn st:score [lindex $replies 0 1] CORRECT 1]
        incr changes 3

        # A script.
        assert_equal 1 [$primary eval {
            local reply = redis.call('GPREDICT', KEYS[1], 'SCORE', ARGV[1])
            return redis.call('GLEARN', KEYS[1], reply[2], 'CORRECT', '1')
        } 1 st:score 0.7]
        incr changes 2

        assert_equal OK [$primary greset st:text]
        incr changes

        # Reads and refused commands change nothing, so nothing is sent.
        $primary ginfo st:score
        assert_error {ERR*} {$primary gpredict st:score SCORE 7}
        assert_error {ERR*} {$primary gadd st:score TEXT x LABEL y}
        assert_error {ERR*} {$primary glearn st:text $id LABEL nolabel}
        assert_error {ERR*} {$primary gpredict st:text SCORE 0.5}
        assert_equal 0 [$primary glearn st:score nonsense CORRECT 1]

        $primary set gr:marker 1
        set commands [gr_stream_until $repl {set gr:marker 1}]
        close_replication_stream $repl
        assert_equal 1 [$primary del gr:marker]

        # Nothing but the internal command, and every command that changed
        # a gate reached the stream.
        set applied [gr_assert_stream_is_realized $commands {st:score st:text}]
        assert {$applied >= $changes}
        # A recorded request travels under the number of its id (CHANGES.md,
        # section 8: "... c_score seq [TEXT ...]"), a classifier's request
        # with its text and a score gate's without anything.
        set carried [dict create]
        foreach command $commands {
            if {[lindex $command 2] ne "PREDICT"} continue
            set key [lindex $command 1]
            if {![dict exists $carried $key]} {
                dict set carried $key [lindex $command 11]
            }
            if {$key eq "st:text"} {
                assert_equal TEXT [lindex $command 12]
            } else {
                assert_equal 12 [llength $command]
            }
        }
        assert_equal $numbered $carried "the numbers of the first requests (first) and what their operations carry (second)"
        gr_assert_same $primary $replicas {st:score st:text} "after the watched commands"
    }

    gr_test {Gate replication: of a long TEXT only the part that is read travels} {
        # SPEC.md: a TEXT has at most 1 MiB and its first 4,096 bytes are
        # read. That part is what the replicas need, and all they are sent.
        assert_equal OK [$primary gcreate st:long ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        set repl [attach_to_replication_stream]
        # Words and characters of several bytes (UTF-8, byte by byte), and
        # bytes that are no text at all: wherever the cut falls, it falls
        # inside something.
        set unit "caf\xc3\xa9 winner prize \x00\xff\r\n \xe6\x97\xa5\xe6\x9c\xac? "
        set texts {}
        foreach length {4095 4096 4097 4099 70000} {
            lappend texts [string range [string repeat $unit [expr {$length / [string length $unit] + 1}]] 0 [expr {$length - 1}]]
        }
        # The longest text a gate takes.
        lappend texts [string range [string repeat $unit 40000] 0 1048575]
        set asked {}
        set sent {}
        set n 0
        foreach text $texts {
            if {[incr n] % 2} {
                lassign [$primary gpredict st:long TEXT $text] action id
            } else {
                lassign [$primary gpredict st:long TEXT $text FEATURES 2 depth 0.5 urgent -1] action id
            }
            assert_equal ABSTAIN $action
            lappend asked $id
            lappend sent $text
            assert_equal $n [$primary gadd st:long TEXT $text FEATURES 1 depth 0.25 LABEL spam]
            lappend sent $text
        }
        # One byte more is refused, and nothing is sent.
        assert_error {ERR*} {$primary gpredict st:long TEXT "[lindex $texts end]x"}
        assert_error {ERR*} {$primary gadd st:long TEXT "[lindex $texts end]x" LABEL spam}
        # The model learns from the clues each node kept with the request.
        foreach id $asked {
            assert_equal 1 [$primary glearn st:long $id LABEL ham]
        }
        $primary set gr:marker 1
        set commands [gr_stream_until $repl {set gr:marker 1}]
        close_replication_stream $repl
        assert_equal 1 [$primary del gr:marker]

        set carried {}
        foreach command $commands {
            set at [lsearch -exact $command TEXT]
            if {$at < 0} continue
            assert {[lindex $command 2] in {PREDICT ADD}}
            lappend carried [lindex $command [expr {$at + 1}]]
            # What follows the text is sent as it came.
            if {[lindex $command 2] eq "ADD"} {
                assert_equal {FEATURES 1 depth 0.25 LABEL spam} [lrange $command [expr {$at + 2}] end]
            }
        }
        assert_equal [llength $sent] [llength $carried]
        foreach text $sent part $carried {
            assert {[string length $part] <= 4096}
            if {$part ne [string range $text 0 4095]} {
                fail "a TEXT of [string length $text] bytes travelled as [string length $part] bytes that are not its beginning"
            }
        }
        gr_assert_stream_is_realized $commands st:long
        gr_assert_same $primary $replicas st:long "after the long texts"
        foreach node $nodes {
            assert_equal [expr {2 * [llength $texts]}] [gr_field $node st:long learned]
            assert_equal 0 [gr_field $node st:long pending]
        }
    }

    gr_test {Gate replication: replicas execute the internal command only} {
        wait_for_ofs_sync $primary $replica
        wait_for_ofs_sync $primary $subreplica
        foreach node $replicas {
            assert {[gr_calls $node _gate.apply] > 1000}
            foreach command [concat $::gr_public_writes eval] {
                assert_equal 0 [gr_calls $node $command] "calls of $command on a replica"
            }
        }
        foreach command $::gr_public_writes {
            assert {[gr_calls $primary $command] > 0}
        }
        assert_equal 0 [gr_calls $primary _gate.apply]
    }

    gr_test {Gate replication: a read-only replica refuses every write and answers GINFO} {
        set gates {sc:audit cl:two mix:s1 mix:c2}
        gr_assert_same $primary $replicas $gates "before the refused commands"
        # A request that waits for its feedback on the primary.
        lassign [$primary gpredict sc:audit SCORE 1] action id
        assert {$id ne {}}

        foreach node $replicas {
            assert_error {READONLY*} {$node gcreate new:gate ALPHA 0.1 SCORE}
            assert_error {READONLY*} {$node gpredict sc:audit SCORE 0.9}
            assert_error {READONLY*} {$node gpredict cl:two TEXT [gr_text spam 1]}
            assert_error {READONLY*} {$node glearn sc:audit $id CORRECT 1}
            assert_error {READONLY*} {$node gadd cl:two TEXT [gr_text spam 1] LABEL spam}
            assert_error {READONLY*} {$node gpause sc:audit}
            assert_error {READONLY*} {$node gresume sc:audit}
            assert_error {READONLY*} {$node greset sc:audit}
            assert_equal 0 [$node exists new:gate]
        }
        # What the replicas refused did not happen: the request still waits.
        gr_assert_same $primary $replicas $gates "after the refused commands"
        assert_equal 1 [$primary glearn sc:audit $id CORRECT 1]

        # The stream command is not for clients, on any node.
        foreach node $nodes {
            assert_error {*unknown command*} {$node _gate.apply sc:audit PAUSE 0}
        }
        gr_assert_same $primary $replicas $gates "after the internal command was refused"

        # GINFO on a replica is GINFO on its primary, in RESP3 as well.
        set resp3 {}
        foreach level {0 -1 -2} {
            set client [redis_client $level]
            $client hello 3
            lappend resp3 $client
        }
        foreach key $gates {
            set want [dict remove [[lindex $resp3 0] ginfo $key] {*}$::gr_primary_only]
            assert_equal [dict get [gr_info $primary $key] state] [dict get $want state]
            foreach client [lrange $resp3 1 end] {
                assert_equal $want [dict remove [$client ginfo $key] {*}$::gr_primary_only] "GINFO $key in RESP3"
            }
        }
        foreach client $resp3 {
            $client close
        }
    }

    gr_test {Gate replication: the AOF of a replica holds what the replica holds} {
        set gates [concat $allgates {st:score st:text st:long}]
        gr_assert_same_digests $primary $replicas $gates "before loading the AOF"
        # The AOF of the replica started empty: everything in it came from
        # the stream, and replaying it must give the same gates.
        assert {[status $replica aof_base_size] < 1024}
        assert {[status $replica aof_current_size] > 100000}
        assert_equal OK [$replica debug loadaof]
        gr_assert_same $primary $replicas $gates "after DEBUG LOADAOF on the replica"

        # A rewritten AOF (as commands, then with an RDB preamble), with
        # more operations appended after each rewrite.
        foreach preamble {no yes} {
            $replica config set aof-use-rdb-preamble $preamble
            $replica bgrewriteaof
            waitForBgrewriteaof $replica
            gr_workload $primary $mix 400
            gr_assert_same_digests $primary $replicas $gates "workload after the AOF rewrite (preamble $preamble)"
            assert_equal OK [$replica debug loadaof]
            gr_assert_same_digests $primary $replicas $gates "after DEBUG LOADAOF of a rewritten AOF (preamble $preamble)"
            # The reloaded replica keeps following its primary.
            gr_workload $primary $mix 200
            gr_assert_same_digests $primary $replicas $gates "workload after DEBUG LOADAOF (preamble $preamble)"
        }
    }

    gr_test {Gate replication: a primary that reloaded its gates tells its replicas what it no longer trusts} {
        # A gate that answers for each command that can be the first one
        # a gate hears after the reload.
        set first {rl:predict rl:learn rl:pause rl:resume rl:reset}
        foreach key $first {
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
            gr_score_until_active $primary $key
        }
        $primary config set gate.snapshot-examples 40
        assert_equal OK [$primary gcreate rl:add ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        $primary config set gate.snapshot-examples 20
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd rl:add TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary rl:add {spam ham}
        lappend first rl:add
        assert_equal OK [$primary gpause rl:resume]
        # A spot check that waits for its feedback.
        for {set n 0} {$n < 500} {incr n} {
            lassign [$primary gpredict rl:learn SCORE 0.99] action id
            if {$action eq "AUDIT"} break
        }
        assert_equal AUDIT $action
        gr_assert_same $primary $replicas $first "before the primary reloads"
        foreach node $nodes {
            foreach key $first {
                assert {![gr_not_answering [gr_info $node $key]]}
            }
        }
        set learned [gr_field $primary rl:add learned]

        assert_equal OK [$primary debug reload]
        # Nothing was written: the replicas hold what the primary reloaded.
        gr_assert_same_digests $primary $replicas [concat $gates $first] "primary reloaded"

        # Whatever the first write to a gate is, it makes the gate drop its
        # proof and its waiting requests: on the primary, and so on its
        # replicas.
        lassign [$primary gpredict rl:predict SCORE 0.99] action fresh
        assert_equal ABSTAIN $action
        assert_equal 0 [$primary glearn rl:learn $id CORRECT 1]
        assert_equal OK [$primary gpause rl:pause]
        assert_equal OK [$primary gresume rl:resume]
        assert_equal OK [$primary greset rl:reset]
        assert_equal [expr {$learned + 1}] [$primary gadd rl:add TEXT [gr_text spam 1] LABEL spam]
        gr_assert_same $primary $replicas $first "after the first writes of the reloaded primary"
        foreach node $nodes {
            foreach key $first {
                assert {[gr_not_answering [gr_info $node $key]]}
            }
            assert_equal PAUSED [gr_field $node rl:pause state]
            assert {[gr_field $node rl:resume state] ni {PAUSED ACTIVE}}
            assert_equal [expr {$learned + 1}] [gr_field $node rl:add learned]
        }
        assert_equal 1 [$primary glearn rl:predict $fresh CORRECT 1]

        # Every other gate follows at its own first write.
        gr_workload $primary $mix 600
        gr_assert_same_digests $primary $replicas [concat $gates $first] "workload after the primary reloaded"
    }

    gr_test {Gate replication: a promoted replica takes its own replica along} {
        assert_equal OK [$primary gresume rl:pause]
        foreach key {rl:predict rl:learn rl:pause} {
            gr_score_until_active $primary $key
        }
        gr_assert_same $primary $replicas $first "before the promotion"
        # Nothing else differs either.
        gr_assert_same_dataset $primary $replicas "before the promotion"

        # The replica in the middle of the chain becomes a primary. Its own
        # replica stays with it.
        assert_equal OK [$replica replicaof no one]
        gr_watch -2
        foreach key $first {
            assert {[gr_not_answering [gr_info $replica $key]]}
        }
        foreach key {rl:predict rl:learn rl:pause rl:resume rl:reset} {
            lassign [$replica gpredict $key SCORE 0.99] action id
            assert_equal ABSTAIN $action "the first GPREDICT $key on the promoted replica"
            if {$id ne {}} {
                assert_equal 1 [$replica glearn $key $id CORRECT 1]
            }
        }
        $replica gadd rl:add TEXT [gr_text ham 1] LABEL ham
        gr_assert_same $replica $subreplica $first "behind the promoted replica"
        foreach key $first {
            assert {[gr_not_answering [gr_info $subreplica $key]]}
        }

        gr_score_until_active $replica rl:predict
        gr_text_until_active $replica rl:add {spam ham}
        gr_assert_same $replica $subreplica $first "gates proven again behind the promoted replica"
        assert_equal ACTIVE [gr_field $subreplica rl:predict state]
        assert_equal ACTIVE [gr_field $subreplica rl:add state]
        gr_assert_same_dataset $replica $subreplica "behind the promoted replica"

        # The AOF of the promoted replica holds what it applied as a
        # replica and what it did as a primary.
        assert_equal OK [$replica debug loadaof]
        gr_assert_same_dataset $replica $subreplica "after the promoted replica loaded its AOF"
    }
}
}
}

# ------------------------------------------------- an ACCEPT writes nothing
#
# The dice are seeded here (gate.test-seed): the same requests are accepted
# and the same are spot-checked in every run.

start_server [list tags {"modules external:skip gate:accept"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    gr_watch -1

    gr_test {Gate replication: an ACCEPT of a score gate replicates nothing} {
        $primary config set gate.test-seed 20261128
        $primary config set gate.snapshot-examples 60
        $primary config set gate.feedback-timeout 600000
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        # Nothing but gates may move the offset while it is watched.
        $primary config set repl-ping-replica-period 3600

        assert_equal OK [$primary gcreate ac:score ALPHA 0.2 DELTA 0.2 SCORE]
        gr_score_until_active $primary ac:score
        set threshold [lindex [gr_field $primary ac:score thresholds] 0]
        assert {$threshold ne {} && $threshold < 0.9}
        gr_assert_same $primary $replica ac:score "before the accepted requests"

        set accepted 0
        set audited 0
        for {set n 0} {$n < 400} {incr n} {
            set offset [status $primary master_repl_offset]
            set unsaved [status $primary rdb_changes_since_last_save]
            lassign [$primary gpredict ac:score SCORE 0.99] action id
            if {$action eq "ACCEPT"} {
                incr accepted
                assert_equal {} $id
                assert_equal $offset [status $primary master_repl_offset] "master_repl_offset after an ACCEPT"
                assert_equal $unsaved [status $primary rdb_changes_since_last_save] "unsaved changes after an ACCEPT"
            } else {
                # A spot check: this one the replica must know about.
                assert_equal AUDIT $action
                incr audited
                assert {[status $primary master_repl_offset] > $offset}
                assert_equal 1 [$primary glearn ac:score $id CORRECT 1]
            }
        }
        assert {$accepted > 300}
        assert {$audited > 0}
        gr_assert_same $primary $replica ac:score "after the accepted requests"
        # The primary counted what it accepted. The replica never heard.
        assert_equal $accepted [gr_field $primary ac:score accepted]
        assert_equal 0 [gr_field $replica ac:score accepted]
    }

    gr_test {Gate replication: an ACCEPT of a classifier gate replicates nothing} {
        assert_equal OK [$primary gcreate ac:text ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        for {set n 0} {$n < 30} {incr n} {
            foreach label {spam ham} {
                $primary gadd ac:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary ac:text {spam ham}
        gr_assert_same $primary $replica ac:text "before the accepted requests"

        set accepted 0
        for {set n 0} {$n < 400} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            set offset [status $primary master_repl_offset]
            lassign [$primary gpredict ac:text TEXT [gr_text $label $n]] action answer
            if {$action eq "ACCEPT"} {
                incr accepted
                assert {$answer eq "spam" || $answer eq "ham"}
                assert_equal $offset [status $primary master_repl_offset] "master_repl_offset after an ACCEPT"
            } elseif {$answer ne {}} {
                assert {[status $primary master_repl_offset] > $offset}
                assert_equal 1 [$primary glearn ac:text $answer LABEL $label]
            }
        }
        assert {$accepted > 200}
        gr_assert_same $primary $replica ac:text "after the accepted requests"
        assert_equal $accepted [gr_field $primary ac:text accepted]
        assert_equal 0 [gr_field $replica ac:text accepted]
    }

    gr_test {Gate replication: an ACCEPT that finds overdue feedback tells the replica} {
        $primary config set gate.feedback-timeout 150
        assert_equal OK [$primary gcreate ac:late ALPHA 0.2 DELTA 0.2 AUDIT 0.1 SCORE]
        $primary config set gate.feedback-timeout 600000
        for {set n 0} {$n < 600 && [gr_field $primary ac:late state] ne "ACTIVE"} {incr n} {
            lassign [$primary gpredict ac:late SCORE 0.9] action id
            $primary glearn ac:late $id CORRECT 1
        }
        assert_equal ACTIVE [gr_field $primary ac:late state]

        set accepted 0
        for {set round 1} {$round <= 4} {incr round} {
            # A spot check that is never answered.
            for {set n 0} {$n < 500} {incr n} {
                lassign [$primary gpredict ac:late SCORE 0.9] action id
                if {$action eq "AUDIT"} break
            }
            assert_equal AUDIT $action
            after 200
            # The next request is accepted, most of the time, and by itself
            # an ACCEPT replicates nothing. But the gate has just found out
            # that a spot check was never answered, and what it made of that
            # is something the replica must have too.
            lassign [$primary gpredict ac:late SCORE 0.9] action id
            if {$action eq "ACCEPT"} {
                incr accepted
            } else {
                $primary glearn ac:late $id CORRECT 1
            }
            gr_assert_same $primary $replica ac:late "round $round of overdue spot checks"
        }
        assert {$accepted > 0}
        assert_equal OK [$primary gpause ac:late]
        assert_equal OK [$primary gresume ac:late]
        gr_assert_same $primary $replica ac:late "after the overdue spot checks"
        # (More than four only if this test was stalled for longer than
        # the feedback timeout between a request and its answer.)
        assert {[gr_field $primary ac:late missing] >= 4}
        assert_equal [gr_field $primary ac:late missing] [gr_field $replica ac:late missing]
        $primary config set repl-ping-replica-period 10
        gr_assert_same_dataset $primary $replica "at the end"
    }

    gr_test {Gate replication: a request that replicates nothing sends a TICK when it finds something due, and only then} {
        # The dice start over: this test meets the same draws in every run.
        $primary config set gate.test-seed 0
        $primary config set gate.test-seed 20261128
        set timeout 800
        set grace 200
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            set key ac:tick:$attempt
            $primary config set gate.feedback-timeout $timeout
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 0.05 SCORE]
            $primary config set gate.feedback-timeout 600000
            # The teacher agrees from 0.7 up, and that is about where the
            # gate ends up answering from: it has requests it accepts and
            # requests it neither answers nor wants an answer for.
            gr_score_until_active $primary $key
            set threshold [lindex [gr_field $primary $key thresholds] 0]
            assert {$threshold > 0.25 && $threshold < 0.95}
            assert_equal 0 [gr_field $primary $key pending]
            # A reader of the stream. (No PING moves the offset while it is
            # attached.)
            set repl [attach_to_replication_stream]
            # The operations the stream must carry from here on, in order.
            set expected {}
            set ticks 0

            # Two spot checks. The first never gets its answer. The second
            # gets it at once, and waits for the first.
            set t0 [clock milliseconds]
            set lost {}
            for {set n 0} {$n < 3000} {incr n} {
                lassign [$primary gpredict $key SCORE 0.95] action id
                if {$action eq "ACCEPT"} continue
                assert_equal AUDIT $action
                lappend expected PREDICT
                if {$lost eq {}} {
                    set lost $id
                    set t1 [clock milliseconds]
                    continue
                }
                assert_equal 1 [$primary glearn $key $id CORRECT 1]
                lappend expected LEARN
                break
            }
            assert {$n < 3000}
            set samples [gr_field $primary $key calibration-samples]
            # Nothing is due: requests without an id replicate nothing.
            lassign [gr_quiet_run $primary $key 16 expected] quiet moved
            set early [gr_info $primary $key]
            if {![gr_in_time $t0 $grace]} {
                close_replication_stream $repl
                continue
            }
            assert {$quiet > 8}
            assert_equal 0 $moved "requests without an id that moved master_repl_offset while nothing was due"
            assert_equal $samples [dict get $early calibration-samples]

            # A quarter of the feedback timeout after the first spot check,
            # proof moves on without its answer: inside the next request,
            # which is accepted and sends a TICK of its own. (If the dice
            # make it a spot check, the step travels with that.)
            gr_wait_age $t1 $grace
            set step1 [llength $expected]
            lassign [gr_step_request $primary $key 0.95 expected] quiet1 bytes1
            incr ticks $quiet1
            # The replica is told at once, not with the next operation.
            gr_sync $primary $replica
            set told [gr_info $replica $key]
            lassign [gr_quiet_run $primary $key 16 expected] quiet moved
            set between [gr_info $primary $key]
            if {![gr_in_time $t0 $timeout]} {
                close_replication_stream $repl
                continue
            }
            if {$quiet1} {
                assert_equal [gr_tick_bytes $key] $bytes1 "bytes an accepted request added to the stream when proof had to move on"
            }
            assert_equal 0 $moved "requests without an id that moved master_repl_offset while nothing was due"
            # The spot check without an answer counts as a wrong guess for
            # now, the answered ones behind it were let through, and it
            # still waits.
            assert {[dict get $between calibration-samples] >= $samples + 2}
            assert_equal 1 [dict get $between pending]
            assert_equal 0 [dict get $between missing]
            assert {[dict get $told calibration-samples] >= $samples + 2}
            assert_equal 1 [dict get $told pending]

            # When the feedback timeout is over it is given up: inside the
            # next request again.
            gr_wait_age $t1 $timeout
            set step2 [llength $expected]
            lassign [gr_step_request $primary $key 0.2 expected] quiet2 bytes2
            incr ticks $quiet2
            if {$quiet2} {
                assert_equal [gr_tick_bytes $key] $bytes2 "bytes a request without an id added to the stream when a spot check had to be given up"
            }
            gr_sync $primary $replica
            assert_equal 1 [gr_field $replica $key missing] "missing on the replica, right after the request that found the spot check overdue"
            assert_equal 0 [gr_field $replica $key pending]
            lassign [gr_quiet_run $primary $key 16 expected] quiet moved
            assert_equal 0 $moved "requests without an id that moved master_repl_offset while nothing was due"
            # Time alone does nothing: no request waits any more.
            after 60
            lassign [gr_quiet_run $primary $key 8 expected] quiet moved
            assert_equal 0 $moved "requests without an id that moved master_repl_offset while nothing waited"

            $primary set gr:marker 1
            set commands [gr_stream_until $repl {set gr:marker 1}]
            close_replication_stream $repl
            assert_equal 1 [$primary del gr:marker]
            set done 1
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
        set operations [gr_stream_operations $commands [list $key]]
        set names {}
        foreach operation $operations {
            lappend names [lindex $operation 0]
        }
        assert_equal $expected $names "the operations in the stream"
        assert {$ticks > 0}
        # Each step happened at the clock of the request that found it.
        set asked [lindex $operations 0 2]
        set moved_on [lindex $operations $step1 2]
        set given_up [lindex $operations $step2 2]
        assert {$moved_on - $asked >= $grace && $moved_on - $asked < $timeout}
        assert {$given_up - $asked >= $timeout}
        gr_assert_same $primary $replica $key "after the spot check was given up"
        foreach node [list $primary $replica] {
            assert_equal 1 [gr_field $node $key missing]
            assert_equal 0 [gr_field $node $key pending]
        }
        assert_equal 0 [$primary glearn $key $lost CORRECT 1]
        gr_assert_same $primary $replica $key "an answer that came too late"
    }

    gr_test {Gate replication: an accepted request of a classifier gives up a request that waited as an example only} {
        $primary config set repl-ping-replica-period 3600
        set timeout 400
        $primary config set gate.feedback-timeout $timeout
        assert_equal OK [$primary gcreate ac:defer ALPHA 0.2 DELTA 0.2 AUDIT 0.001 CLASSES 3 billing tech other ALWAYSDEFER 1 other]
        $primary config set gate.feedback-timeout 600000
        for {set n 0} {$n < 20} {incr n} {
            foreach label {billing tech other} {
                $primary gadd ac:defer TEXT [gr_text $label $n] LABEL $label
            }
        }
        # Until the gate answers for both labels it may answer with.
        for {set n 0} {$n < 4000} {incr n} {
            set thresholds [gr_field $primary ac:defer thresholds]
            if {[lindex $thresholds 0] ne {} && [lindex $thresholds 1] ne {}} break
            set label [lindex {billing tech other} [expr {$n % 3}]]
            gr_text_request $primary ac:defer [gr_text $label $n] $label
        }
        assert {$n < 4000}
        assert_equal {} [lindex $thresholds 2]
        gr_assert_same $primary $replica ac:defer "before the request that is never answered"
        set missing [gr_field $primary ac:defer missing]
        set done 0
        for {set attempt 1} {$attempt <= 4 && !$done} {incr attempt} {
            # A request the gate takes for 'other', which it never answers
            # with: it is proof for nothing, and waits for its answer only
            # because the model could learn from it. The answer never comes.
            set t0 [clock milliseconds]
            lassign [$primary gpredict ac:defer TEXT [gr_text other [expr {100 + $attempt}]]] action lost
            set t1 [clock milliseconds]
            assert_equal ABSTAIN $action
            assert {$lost ne {}}
            # Accepted requests meanwhile.
            set accepted 0
            set moved 0
            for {set n 0} {$n < 30} {incr n} {
                set label [lindex {billing tech} [expr {$n % 2}]]
                set offset [status $primary master_repl_offset]
                lassign [$primary gpredict ac:defer TEXT [gr_text $label $n]] action answer
                if {$action eq "ACCEPT"} {
                    incr accepted
                    if {[status $primary master_repl_offset] != $offset} {
                        incr moved
                    }
                } elseif {$answer ne {}} {
                    $primary glearn ac:defer $answer LABEL $label
                }
            }
            set waiting [gr_field $primary ac:defer pending]
            if {![gr_in_time $t0 $timeout]} continue
            assert {$accepted > 20}
            assert_equal 0 $moved "accepted requests that moved master_repl_offset while nothing was due"
            assert {$waiting >= 1}
            gr_sync $primary $replica
            set applied [gr_calls $replica _gate.apply]

            # The feedback timeout is over. The next request is accepted,
            # and tells the replica that the gate forgot the other one.
            gr_wait_age $t1 $timeout
            set offset [status $primary master_repl_offset]
            lassign [$primary gpredict ac:defer TEXT [gr_text billing 7]] action answer
            set bytes [expr {[status $primary master_repl_offset] - $offset}]
            if {$action ne "ACCEPT"} {
                # A spot check, one request in a thousand: once more.
                if {$answer ne {}} {
                    $primary glearn ac:defer $answer LABEL billing
                }
                continue
            }
            assert_equal billing $answer
            assert_equal [gr_tick_bytes ac:defer] $bytes "bytes an accepted request added to the stream when a request had to be given up"
            gr_assert_same $primary $replica ac:defer "a request given up by an accepted one"
            assert_equal [expr {$applied + 1}] [gr_calls $replica _gate.apply]
            foreach node [list $primary $replica] {
                assert_equal 0 [gr_field $node ac:defer pending]
                # It was not proof: nothing is missing.
                assert_equal $missing [gr_field $node ac:defer missing]
            }
            assert_equal 0 [$primary glearn ac:defer $lost LABEL other]
            set done 1
        }
        if {!$done} {
            fail "four attempts, each either stalled or met by a spot check: this test could not show what it is about"
        }
        $primary config set repl-ping-replica-period 10
        gr_assert_same_dataset $primary $replica "at the end"
    }
}
}
# -------------------------------------------------------------- time passes
#
# The chain again: primary -> replica (with an AOF) -> sub-replica, attached
# before any gate exists. The gates have short feedback timeouts and real
# time passes between the commands: proof moves on without late answers,
# requests are given up, gates stop for want of answers. The replicas do not
# watch the clock. They are told what the primary found, when it found it,
# and must hold the same gates after every step.

start_server [list tags {"modules external:skip gate:time"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule appendonly yes auto-aof-rewrite-percentage 0]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    gr_watch -1 -2
    gr_timed_start

    gr_test {Gate replication over time: replicas attach before any gate exists} {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 20
        $replica replicaof $primary_host $primary_port
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $replica
        wait_for_sync $subreplica
        waitForBgrewriteaof $replica
        assert_equal 0 [$primary dbsize]
    }

    gr_test {Gate replication over time: proof moves on without a late answer inside the next write, whatever that write is} {
        set timeout 1000
        set grace 250
        # The right answers a score gate needs before it answers (SPEC.md
        # section 5): when no guess was wrong, and when one was.
        set flawless [gr_right_answers 0.2 0.2 1 1]
        set flawed [gr_right_answers 0.2 0.2 1 1 1]
        assert {$flawless > 4 && $flawed > $flawless}
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            # A gate for each write that can be the one to find that an
            # answer is late.
            set kpredict late:predict:$attempt
            set klearn late:learn:$attempt
            set kunknown late:unknown:$attempt
            set kpause late:pause:$attempt
            set kresume late:resume:$attempt
            set kadd late:add:$attempt
            set scored [list $kpredict $klearn $kunknown $kpause $kresume]
            set keys [concat $scored $kadd]
            $primary config set gate.feedback-timeout $timeout
            foreach key $scored {
                assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 SCORE]
            }
            assert_equal OK [$primary gcreate $kadd ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
            $primary config set gate.feedback-timeout 600000
            # A classifier needs a snapshot before a request can be proof.
            for {set n 0} {$n < 10} {incr n} {
                foreach label {spam ham} {
                    $primary gadd $kadd TEXT [gr_text $label $n] LABEL $label
                }
            }
            assert_equal 1 [gr_field $primary $kadd candidate]

            # Three requests to each gate. The second and the third are
            # answered at once, the first is not: proof is counted in
            # request order, so two answers wait for the oldest one.
            set unanswered [dict create]
            set t0 [clock milliseconds]
            foreach key $keys {
                set ids {}
                for {set n 0} {$n < 3} {incr n} {
                    if {$key eq $kadd} {
                        lassign [$primary gpredict $key TEXT [gr_text spam $n]] action id
                    } else {
                        lassign [$primary gpredict $key SCORE 0.9] action id
                    }
                    assert_equal ABSTAIN $action
                    lappend ids $id
                }
                dict set unanswered $key [lindex $ids 0]
                foreach id [lrange $ids 1 end] {
                    if {$key eq $kadd} {
                        assert_equal 1 [$primary glearn $key $id LABEL spam]
                    } else {
                        assert_equal 1 [$primary glearn $key $id CORRECT 1]
                    }
                }
            }
            set t1 [clock milliseconds]
            set early [dict create]
            foreach key $keys {
                dict set early $key [gr_info $primary $key]
            }
            if {![gr_in_time $t0 $grace]} continue
            foreach key $keys {
                assert_equal 0 [dict get $early $key calibration-samples] "proof of $key, counted while an older answer is awaited"
                assert_equal 1 [dict get $early $key pending]
            }

            # A quarter of the feedback timeout after the oldest request,
            # proof stops waiting for its answer. The gate finds out when it
            # is written to, and the replicas with it.
            gr_wait_age $t1 $grace
            lassign [$primary gpredict $kpredict SCORE 0.9] action fourth
            # The late answer itself can be that write.
            set late [$primary glearn $klearn [dict get $unanswered $klearn] CORRECT 1]
            set unknown [$primary glearn $kunknown 0123456789abcdef-7 CORRECT 1]
            assert_equal OK [$primary gpause $kpause]
            assert_equal OK [$primary gresume $kresume]
            $primary gadd $kadd TEXT [gr_text ham 11] LABEL ham
            if {![gr_in_time $t0 $timeout]} continue
            assert_equal ABSTAIN $action
            assert_equal 1 $late "GLEARN $klearn, a quarter of the feedback timeout after its request"
            assert_equal 0 $unknown
            gr_assert_same $primary $replicas $keys "proof moved on without the oldest answer"
            foreach node $nodes {
                foreach key $keys {
                    # The request without an answer is counted as a wrong
                    # guess for now, the two behind it as what they were.
                    assert_equal 3 [gr_field $node $key calibration-samples] "calibration-samples of $key"
                    assert_equal 0 [gr_field $node $key missing] "missing of $key"
                }
                # It still waits for its answer.
                foreach key [list $kunknown $kpause $kresume $kadd] {
                    assert_equal 1 [gr_field $node $key pending] "pending of $key"
                }
                assert_equal 2 [gr_field $node $kpredict pending]
                assert_equal 0 [gr_field $node $klearn pending]
                assert_equal PAUSED [gr_field $node $kpause state]
            }

            # For two more gates the answer arrives after all. It is used,
            # and the guess that was counted as wrong is set right.
            set late1 [$primary glearn $kpredict [dict get $unanswered $kpredict] CORRECT 1]
            set late2 [$primary glearn $kresume [dict get $unanswered $kresume] CORRECT 1]
            if {![gr_in_time $t0 $timeout]} continue
            assert_equal {1 1} [list $late1 $late2] "GLEARN for requests that proof had moved on without"
            assert_equal 1 [$primary glearn $kpredict $fourth CORRECT 1]
            gr_assert_same $primary $replicas $keys "late answers"
            # These three gates now hold right answers only (three of
            # them, and a fourth for one gate): they answer after exactly
            # as many as a gate needs that never met a wrong guess.
            foreach {key right} [list $kpredict 4 $klearn 3 $kresume 3] {
                gr_right_requests $primary $key [expr {$flawless - $right - 1}]
            }
            gr_assert_same $primary $replicas $keys "one right answer short"
            foreach node $nodes {
                foreach key [list $kpredict $klearn $kresume] {
                    assert_equal CALIBRATING [gr_field $node $key state] "$key after [expr {$flawless - 1}] right answers"
                    assert_equal 0 [gr_field $node $key pending]
                }
            }
            foreach key [list $kpredict $klearn $kresume] {
                gr_right_requests $primary $key 1
            }
            gr_assert_same $primary $replicas $keys "late answers, set right"
            foreach node $nodes {
                foreach key [list $kpredict $klearn $kresume] {
                    assert_equal ACTIVE [gr_field $node $key state] "$key after $flawless right answers"
                    assert_equal 1 [gr_field $node $key generation]
                }
            }

            # The other three never get that answer. When the feedback
            # timeout is over the request is given up: a wrong guess for
            # good, and its id is forgotten.
            gr_wait_age $t1 $timeout
            assert_equal 0 [$primary glearn $kunknown 0123456789abcdef-7 CORRECT 1]
            assert_equal OK [$primary gresume $kpause]
            $primary gadd $kadd TEXT [gr_text spam 12] LABEL spam
            gr_assert_same $primary $replicas $keys "the oldest answer never came"
            foreach node $nodes {
                foreach key [list $kunknown $kpause $kadd] {
                    assert_equal 1 [gr_field $node $key missing] "missing of $key"
                    assert_equal 0 [gr_field $node $key pending] "pending of $key"
                    assert_equal 3 [gr_field $node $key calibration-samples]
                }
            }
            assert_equal 0 [$primary glearn $kunknown [dict get $unanswered $kunknown] CORRECT 1]
            assert_equal 0 [$primary glearn $kpause [dict get $unanswered $kpause] CORRECT 1]
            assert_equal 0 [$primary glearn $kadd [dict get $unanswered $kadd] LABEL spam]
            # Two right answers and a wrong one: these gates need more
            # right answers than the others before they answer.
            foreach key [list $kunknown $kpause] {
                gr_right_requests $primary $key [expr {$flawed - 2 - 1}]
            }
            gr_assert_same $primary $replicas $keys "one right answer short, after a wrong one"
            foreach node $nodes {
                foreach key [list $kunknown $kpause] {
                    assert_equal CALIBRATING [gr_field $node $key state] "$key after a wrong guess and [expr {$flawed - 1}] right answers"
                }
            }
            foreach key [list $kunknown $kpause] {
                gr_right_requests $primary $key 1
            }
            gr_assert_same $primary $replicas $keys "an answer that never came"
            foreach node $nodes {
                foreach key [list $kunknown $kpause] {
                    assert_equal ACTIVE [gr_field $node $key state] "$key after a wrong guess and $flawed right answers"
                }
            }
            set done 1
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
    }

    gr_test {Gate replication over time: a gate that gets no answers, or loses those of its spot checks, stops on the replicas too, and starts again when answers come} {
        set flawless [gr_right_answers 0.2 0.2 1 1]
        set timeout 300
        $primary config set gate.feedback-timeout $timeout
        # Three gates that do not answer yet, two that answer and check
        # every request they could answer (AUDIT 1), and a classifier that
        # does the same.
        foreach key {out:calib out:short out:mixed} {
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 SCORE]
        }
        foreach key {out:spots out:fewer out:ended} {
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        }
        assert_equal OK [$primary gcreate out:text ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 600000
        set outage {out:calib out:short out:mixed out:spots out:fewer out:ended out:text}
        gr_right_requests $primary out:spots $flawless
        gr_right_requests $primary out:fewer $flawless
        gr_right_requests $primary out:ended $flawless
        for {set n 0} {$n < 10} {incr n} {
            foreach label {spam ham} {
                $primary gadd out:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary out:text {spam ham}
        gr_assert_same $primary $replicas $outage "before the answers stop"
        foreach node $nodes {
            foreach key {out:calib out:short out:mixed} {
                assert_equal CALIBRATING [gr_field $node $key state] "state of $key"
            }
            foreach key {out:spots out:fewer out:ended out:text} {
                assert_equal ACTIVE [gr_field $node $key state] "state of $key"
            }
        }
        set learned [gr_field $primary out:text learned]
        set numbered [expr {max([gr_field $primary out:text generation], [gr_field $primary out:text candidate])}]

        # Requests that never get their answer.
        #
        #   out:calib  34 in a row, to a gate that does not answer yet: its
        #              feedback path is broken (SPEC.md, "No answers at all").
        #              The 32nd stops the gate; the two that still wait
        #              then are forgotten, not missed.
        #   out:short  31 in a row: one short of that.
        #   out:mixed  31, one that is answered at once, and 31 more. An
        #              answer has arrived since the first 31 were asked:
        #              they are lost answers, not an outage (CHANGES.md 9b).
        #   out:spots  24 spot checks. Lost spot checks may hide
        #              disagreements, and count as such in a second
        #              statistic that stops the gate at 10^9 (SPEC.md,
        #              "Getting worse"): here with the 22nd of the 24,
        #              (2^22 0.75^2 + 3^22 0.5^2) / 2 = 3.9 x 10^9, after
        #              0.65 x 10^9 with 21. What still waits then is not
        #              needed any more: forgotten, not missed.
        #   out:fewer  19 spot checks: (2^19 + 3^19) / 2 = 0.58 x 10^9.
        #   out:ended  30 spot checks in a row, numbers 45 to 74 of 94 whose
        #              others all get their answers. A block is 64 spot
        #              checks: the first 20 are of a block that has ended
        #              by the time they are given up, and count only in the
        #              longer blocks, where 30 lost among 94 are nothing.
        #              The open block has lost 10 of its 30: nothing either.
        #              (All 30 in one block of 30 would make 10^14.)
        #   out:text   32, of both labels: no label loses 20 spot checks.
        assert {![gr_drift_reached 0.2 24 21 1e9] && [gr_drift_reached 0.2 24 22 1e9]}
        assert {![gr_drift_reached 0.2 19 19 1e9] && [gr_drift_reached 0.2 20 20 1e9]}
        assert {![gr_drift_reached 0.2 30 10 1e9] && ![gr_drift_reached 0.2 94 30 1e9] && [gr_drift_reached 0.2 30 30 1e9]}
        set lost [dict create]
        foreach {key count} {out:calib 34 out:short 31 out:mixed 31 out:spots 24 out:fewer 19} {
            dict set lost $key [gr_unanswered $primary $key $count]
        }
        lassign [$primary gpredict out:mixed SCORE 0.9] action id
        assert_equal 1 [$primary glearn out:mixed $id CORRECT 1]
        dict lappend lost out:mixed {*}[gr_unanswered $primary out:mixed 31]
        foreach id [gr_spot_checks $primary out:ended 44] {
            assert_equal 1 [$primary glearn out:ended $id CORRECT 1]
        }
        dict set lost out:ended [gr_spot_checks $primary out:ended 30]
        foreach id [gr_spot_checks $primary out:ended 20] {
            assert_equal 1 [$primary glearn out:ended $id CORRECT 1]
        }
        for {set n 0} {$n < 32} {incr n} {
            lassign [$primary gpredict out:text TEXT [gr_text [lindex {spam ham} [expr {$n % 2}]] $n]] action id
            assert {$action ne "ACCEPT" && $id ne {}}
            dict lappend lost out:text $id
        }
        set t1 [clock milliseconds]
        gr_assert_same $primary $replicas $outage "requests waiting"

        # When the feedback timeout is over, the next write to a gate gives
        # all of them up, on every node.
        gr_wait_age $t1 $timeout
        set stopped [$primary gpredict out:calib SCORE 0.9]
        # 31 in a row: this gate goes on. The request is proof like any
        # other, and it gets its answer.
        lassign [$primary gpredict out:short SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn out:short $id CORRECT 1]
        set stale [$primary glearn out:mixed [lindex [dict get $lost out:mixed] 0] CORRECT 1]
        set unchecked [$primary gpredict out:spots SCORE 0.9]
        # 19 lost spot checks: this gate goes on answering, the request is
        # a spot check, and it gets its answer.
        lassign [$primary gpredict out:fewer SCORE 0.9] action id
        assert_equal AUDIT $action
        assert_equal 1 [$primary glearn out:fewer $id CORRECT 1]
        assert_equal OK [$primary gresume out:ended]
        $primary gadd out:text TEXT [gr_text spam 50] LABEL spam
        gr_assert_same $primary $replicas $outage "the feedback timeout is over"
        assert_equal 0 $stale
        # The gates that have stopped hand out an id all the same, so that
        # there is something to answer.
        foreach reply [list $stopped $unchecked] {
            assert_equal ABSTAIN [lindex $reply 0]
            assert {[lindex $reply 1] ne {}}
        }
        foreach node $nodes {
            foreach {key missing} {out:calib 32 out:spots 22 out:text 32} {
                set info [gr_info $node $key]
                assert_equal COLD [dict get $info state] "state of $key"
                assert {[gr_not_answering $info]}
                # No snapshot is taken while no answer arrives.
                assert_equal 0 [dict get $info candidate] "candidate of $key"
                assert_equal $missing [dict get $info missing] "missing of $key"
            }
            # Nothing answered yet, so nothing stopped answering.
            assert_equal 0 [gr_field $node out:calib revocations]
            assert_equal 1 [gr_field $node out:calib pending]
            assert_equal 1 [gr_field $node out:spots revocations]
            assert_equal 1 [gr_field $node out:spots pending]
            assert_equal 1 [gr_field $node out:text revocations]
            # Missing answers say nothing about the model: it is kept.
            assert_equal [expr {$learned + 1}] [gr_field $node out:text learned]
            # One request short of an outage: tested as before, with 31
            # wrong guesses against it.
            gr_expect_fields $node out:short {state CALIBRATING generation 0 candidate 1 missing 31 revocations 0 pending 0 calibration-samples 32} \
                "31 requests in a row without an answer"
            # Lost answers, however many are given up at once.
            gr_expect_fields $node out:mixed {state CALIBRATING generation 0 candidate 1 missing 62 revocations 0 pending 0 calibration-samples 63} \
                "31 lost answers, an answer, 31 lost answers"
            gr_expect_fields $node out:fewer {state ACTIVE generation 1 missing 19 revocations 0 pending 0 audit-samples 1 audit-errors 0} \
                "19 lost spot checks"
            gr_expect_fields $node out:ended [dict create state ACTIVE generation 1 missing 30 revocations 0 pending 0 \
                audit-samples 64 audit-errors 0 calibration-samples [expr {$flawless + 94}]] \
                "30 lost spot checks in a row, 20 of them in a block that had ended"
        }
        dict for {key ids} $lost {
            set id [lindex $ids end]
            if {$key eq "out:text"} {
                assert_equal 0 [$primary glearn $key $id LABEL spam] "GLEARN $key for a request that was given up"
            } else {
                assert_equal 0 [$primary glearn $key $id CORRECT 1] "GLEARN $key for a request that was given up"
            }
        }

        # Requests to the gates that have stopped: every one gets an id,
        # and still no snapshot is taken.
        foreach key {out:calib out:spots} {
            lassign [$primary gpredict $key SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
        }
        lassign [$primary gpredict out:text TEXT [gr_text ham 51]] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        gr_assert_same $primary $replicas $outage "requests to gates that have stopped"
        # A gate that has stopped is a gate like any other to save and to
        # load: the sub-replica starts over from what it would save.
        assert_equal OK [$subreplica debug reload]
        gr_assert_same $primary $replicas $outage "after DEBUG RELOAD on the sub-replica, of gates that have stopped"
        foreach node $nodes {
            foreach key {out:calib out:spots out:text} {
                assert_equal 0 [gr_field $node $key candidate] "candidate of $key"
                assert_equal COLD [gr_field $node $key state]
            }
        }

        # The first answer that arrives (to a request made just now): the
        # gates are tested again, under numbers that were never used.
        foreach key {out:calib out:spots} {
            lassign [$primary gpredict $key SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key, the first answer since the gate stopped"
        }
        lassign [$primary gpredict out:text TEXT [gr_text ham 52]] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn out:text $id LABEL ham]
        gr_assert_same $primary $replicas $outage "answers arrive again"
        foreach node $nodes {
            foreach key {out:calib out:spots} {
                assert_equal CALIBRATING [gr_field $node $key state] "state of $key"
                assert_equal 2 [gr_field $node $key candidate] "candidate of $key"
            }
            assert_equal CALIBRATING [gr_field $node out:text state]
            assert {[gr_field $node out:text candidate] > $numbered}
        }
        # And they answer again, after the right answers the second
        # snapshot of a key needs. (What was asked while the gate had
        # stopped is proof for nothing.)
        set again [gr_right_answers 0.2 0.2 1 2]
        assert {$again > $flawless}
        gr_right_requests $primary out:spots [expr {$again - 1}]
        gr_sync $primary $replicas
        foreach node $nodes {
            assert_equal CALIBRATING [gr_field $node out:spots state]
        }
        gr_right_requests $primary out:spots 1
        gr_assert_same $primary $replicas $outage "the gate answers again"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node out:spots state]
            assert_equal 2 [gr_field $node out:spots generation]
            assert_equal 1 [gr_field $node out:spots revocations]
        }
        # The gate that was one request short of an outage has 31 wrong
        # guesses to make up for: it answers after as many right answers
        # as SPEC.md section 5 asks of a threshold with 31 wrong guesses
        # against it. (It has one right answer so far.)
        set flawed [gr_right_answers 0.2 0.2 1 1 31]
        assert {$flawed > 5 * $flawless}
        gr_right_requests $primary out:short [expr {$flawed - 2}]
        gr_sync $primary $replicas
        foreach node $nodes {
            assert_equal CALIBRATING [gr_field $node out:short state] "out:short after 31 lost answers and [expr {$flawed - 1}] right ones"
        }
        gr_right_requests $primary out:short 1
        gr_assert_same $primary $replicas out:short "a gate that went on after 31 lost answers"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node out:short state] "out:short after 31 lost answers and $flawed right ones"
            assert_equal 1 [gr_field $node out:short generation]
        }
    }

    # The gates of the workloads: feedback timeouts of 120 and 300 ms.
    set tgates {
        {tw:s1 score {} 120}
        {tw:s2 score {} 300}
        {tw:c1 text {spam ham} 120}
        {tw:c2 text {billing tech other} 300}
    }
    set tkeys {tw:s1 tw:s2 tw:c1 tw:c2}

    gr_test {Gate replication over time: answers that come late, out of order or never} {
        $primary config set gate.snapshot-examples 40
        $primary config set gate.feedback-timeout 120
        assert_equal OK [$primary gcreate tw:s1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 SCORE]
        assert_equal OK [$primary gcreate tw:c1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 300
        assert_equal OK [$primary gcreate tw:s2 ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        assert_equal OK [$primary gcreate tw:c2 ALPHA 0.2 DELTA 0.2 AUDIT 0.4 CLASSES 3 billing tech other ALWAYSDEFER 1 other]
        $primary config set gate.feedback-timeout 600000
        $primary config set gate.snapshot-examples 20
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd tw:c1 TEXT [gr_text $label $n] LABEL $label
            }
            foreach label {billing tech} {
                $primary gadd tw:c2 TEXT [gr_text $label $n] LABEL $label
            }
        }
        # A teacher that answers at once and is right: the gates start to
        # answer.
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "the gates answer"

        # From here on an answer may take longer than proof waits for it,
        # longer than the gate remembers the request, or never come.
        set ::gr_fates {now 55 soon 15 late 15 toolate 8}
        for {set round 1} {$round <= 4} {incr round} {
            gr_timed_workload $primary $tgates 400
            gr_phase $primary $replicas $tkeys "late and lost answers, round $round"
            # Whatever was paused answers again in the next round.
            foreach key $tkeys {
                $primary gresume $key
            }
        }
        # The workload met every reply and every fate of an answer.
        foreach what {ACCEPT AUDIT ABSTAIN NOFEEDBACK now soon late toolate never used unused} {
            if {$::gr_timed($what) == 0} {
                fail "the workload never met '$what': [array get ::gr_timed]"
            }
        }
        set missing 0
        foreach key $tkeys {
            incr missing [gr_field $subreplica $key missing]
        }
        assert {$missing > 0}
    }

    gr_test {Gate replication over time: accepted requests carry what falls due between them} {
        gr_timed_usual
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "before the accepted requests"
        # Requests the gates are sure of, and a teacher that answers one
        # spot check in three (at once) and the others never. Most
        # requests are accepted and replicate nothing by themselves; the
        # spot checks that are given up meanwhile must reach the replicas
        # all the same.
        set ::gr_fates {now 34}
        set ::gr_confident 1
        set accepted $::gr_timed(ACCEPT)
        set repl [attach_to_replication_stream]
        for {set step 0} {$step < 8} {incr step} {
            for {set n 0} {$n < 48} {incr n} {
                gr_timed_request $primary [lindex $tgates [expr {$n % [llength $tgates]}]]
            }
            after 40
        }
        set ::gr_confident 0
        set ::gr_fates {now 55 soon 15 late 15 toolate 8}
        $primary set gr:marker 1
        set commands [gr_stream_until $repl {set gr:marker 1}]
        close_replication_stream $repl
        assert_equal 1 [$primary del gr:marker]
        assert {$::gr_timed(ACCEPT) - $accepted > 100}
        # Nothing but realized operations, and among them some that no
        # command of a client stands for.
        set ticks 0
        foreach operation [gr_stream_operations $commands $tkeys] {
            if {[lindex $operation 0] eq "TICK"} {
                incr ticks
            }
        }
        if {$ticks == 0} {
            fail "no accepted request found anything due: the stream carries no TICK"
        }
        gr_phase $primary $replicas $tkeys "accepted requests across the moments answers were given up"
    }

    gr_test {Gate replication over time: the teacher falls silent and the gates stop, it comes back and they start again} {
        gr_timed_usual
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "before the teacher falls silent"
        set before [dict create]
        foreach key $tkeys {
            dict set before $key [gr_info $primary $key]
            dict set ::gr_teacher $key silent
        }
        # What the teacher still owed is lost with it.
        set ::gr_owed {}
        set stopped 0
        for {set round 1} {$round <= 6 && !$stopped} {incr round} {
            # Requests go on, most of them accepted; no answer arrives.
            for {set n 0} {$n < 800} {incr n} {
                gr_timed_request $primary [lindex $tgates [expr {$n % [llength $tgates]}]]
            }
            gr_phase $primary $replicas $tkeys "a silent teacher, round $round"
            # Longer than any feedback timeout: the next write to a gate
            # finds every waiting request overdue.
            after 310
            foreach gate $tgates {
                gr_timed_request $primary $gate
            }
            gr_phase $primary $replicas $tkeys "a silent teacher, round $round, after the feedback timeout"
            set stopped 1
            foreach key $tkeys {
                set info [gr_info $primary $key]
                if {[dict get $info generation] != 0 || [dict get $info candidate] != 0} {
                    set stopped 0
                }
            }
        }
        if {!$stopped} {
            fail "the gates did not stop although their teacher is silent: [gr_describe $primary tw:s1]"
        }
        foreach node $nodes {
            foreach key $tkeys {
                set info [gr_info $node $key]
                assert_equal COLD [dict get $info state] "state of $key"
                assert {[gr_not_answering $info]}
                assert {[dict get $info revocations] > [dict get $before $key revocations]}
                assert {[dict get $info missing] > [dict get $before $key missing]}
            }
        }
        # While nothing is answered the gates take no snapshot.
        for {set n 0} {$n < 80} {incr n} {
            assert_equal ABSTAIN [gr_timed_request $primary [lindex $tgates [expr {$n % [llength $tgates]}]]]
        }
        gr_phase $primary $replicas $tkeys "requests to gates that have stopped"
        foreach node $nodes {
            foreach key $tkeys {
                assert_equal 0 [gr_field $node $key candidate] "candidate of $key"
            }
        }

        # The teacher is back.
        foreach key $tkeys {
            dict set ::gr_teacher $key good
        }
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "the teacher is back"
        foreach node $nodes {
            foreach key $tkeys {
                set info [gr_info $node $key]
                assert_equal ACTIVE [dict get $info state] "state of $key"
                # A snapshot number is never used twice.
                assert {[dict get $info generation] > [dict get $before $key generation]}
                assert {[dict get $info generation] > [dict get $before $key candidate]}
            }
        }
    }

    gr_test {Gate replication over time: the teacher starts to disagree and the gates stop, while answers are still on their way} {
        gr_timed_usual
        set before [dict create]
        foreach key $tkeys {
            dict set before $key [gr_field $primary $key revocations]
        }
        # Answers take their time, but none is lost. Two of the teachers
        # now disagree with whatever their gates guess.
        set ::gr_fates {now 50 soon 25 late 25}
        dict set ::gr_teacher tw:s1 wrong
        dict set ::gr_teacher tw:c1 wrong
        set alarmed 0
        for {set round 1} {$round <= 12 && !$alarmed} {incr round} {
            gr_timed_workload $primary $tgates 250
            gr_phase $primary $replicas $tkeys "a teacher that disagrees, round $round"
            foreach key $tkeys {
                $primary gresume $key
            }
            set alarmed [expr {[gr_field $primary tw:s1 revocations] > [dict get $before tw:s1] &&
                               [gr_field $primary tw:c1 revocations] > [dict get $before tw:c1]}]
        }
        if {!$alarmed} {
            fail "a teacher that disagrees with every guess did not stop its gates: [gr_describe $primary tw:s1] / [gr_describe $primary tw:c1]"
        }
        foreach node $nodes {
            assert {[gr_not_answering [gr_info $node tw:s1]]}
        }

        # The teachers agree again. What they said meanwhile still arrives.
        dict set ::gr_teacher tw:s1 good
        dict set ::gr_teacher tw:c1 good
        for {set round 1} {$round <= 2} {incr round} {
            gr_timed_workload $primary $tgates 250
            gr_phase $primary $replicas $tkeys "the teacher agrees again, round $round"
            foreach key $tkeys {
                $primary gresume $key
            }
        }
        set ::gr_fates {now 55 soon 15 late 15 toolate 8}
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "the gates answer again"
    }

    gr_test {Gate replication over time: GPAUSE, GRESUME and GRESET while answers are on their way} {
        gr_timed_usual
        foreach key $tkeys {
            assert_equal OK [$primary gpause $key]
        }
        gr_timed_workload $primary $tgates 300
        gr_phase $primary $replicas $tkeys "paused"

        # An answer that is on its way to each of two gates when they are
        # reset: it is for a request they have forgotten.
        set numbered [dict create]
        foreach gate [list [lindex $tgates 1] [lindex $tgates 3]] {
            set key [lindex $gate 0]
            # (The workload resumes a gate now and then.)
            assert_equal OK [$primary gpause $key]
            gr_timed_owe $primary $gate
            set info [gr_info $primary $key]
            dict set numbered $key [expr {max([dict get $info generation], [dict get $info candidate])}]
        }
        set refused $::gr_timed(refused)
        assert_equal OK [$primary greset tw:s2]
        assert_equal OK [$primary greset tw:c2]
        gr_timed_void {tw:s2 tw:c2}
        gr_phase $primary $replicas $tkeys "two gates reset"
        foreach node $nodes {
            # A reset keeps the pause, and never hands a number out again.
            assert_equal PAUSED [gr_field $node tw:s2 state]
            assert {[gr_field $node tw:s2 candidate] > [dict get $numbered tw:s2]}
            assert_equal 0 [gr_field $node tw:c2 learned]
            assert_equal 0 [gr_field $node tw:s2 pending]
            assert_equal 0 [gr_field $node tw:c2 pending]
        }
        gr_timed_workload $primary $tgates 300
        gr_phase $primary $replicas $tkeys "after the reset"

        foreach key $tkeys {
            assert_equal OK [$primary gresume $key]
        }
        gr_timed_workload $primary $tgates 300
        after 60
        gr_timed_deliver $primary
        gr_phase $primary $replicas $tkeys "resumed"
        # The answers for requests from before the reset were sent, and
        # were of no use.
        assert {$::gr_timed(refused) >= $refused + 2}
    }

    gr_test {Gate replication over time: the AOF of a replica replays the clock of its primary} {
        gr_timed_usual
        gr_assert_same_digests $primary $replicas $tkeys "before loading the AOF"
        # Everything the replica holds came through the stream and is in
        # its AOF, each operation with the clock it happened at. Replayed
        # at any other time it must give the same gates.
        assert {[status $replica aof_current_size] > 100000}
        assert_equal OK [$replica debug loadaof]
        gr_phase $primary $replicas $tkeys "after DEBUG LOADAOF on the replica"

        # The AOF rewritten while requests wait and answers are owed (as
        # commands, then with an RDB preamble), and more of the workload
        # on top of each.
        foreach preamble {no yes} {
            gr_timed_workload $primary $tgates 200
            gr_sync $primary $replicas
            $replica config set aof-use-rdb-preamble $preamble
            $replica bgrewriteaof
            waitForBgrewriteaof $replica
            gr_timed_workload $primary $tgates 200
            gr_assert_same_digests $primary $replicas $tkeys "workload after the AOF rewrite (preamble $preamble)"
            assert_equal OK [$replica debug loadaof]
            gr_phase $primary $replicas $tkeys "after DEBUG LOADAOF of the rewritten AOF (preamble $preamble)"
        }
        # The sub-replica starts over from what it would save.
        assert_equal OK [$subreplica debug reload]
        gr_phase $primary $replicas $tkeys "after DEBUG RELOAD on the sub-replica"
        gr_timed_workload $primary $tgates 250
        gr_phase $primary $replicas $tkeys "workload after the reloads"
    }

    gr_test {Gate replication over time: a restarted replica and the replica behind it catch up} {
        gr_timed_usual
        gr_timed_workload $primary $tgates 200
        # Remember whose replica this is.
        $replica config rewrite
        restart_server -1 true false
        set replica [srv -1 client]
        set replicas [list $replica $subreplica]
        set nodes [list $primary $replica $subreplica]
        # The primary goes on while the replica loads its AOF and finds
        # its way back.
        gr_timed_workload $primary $tgates 300
        wait_for_condition 150 100 {
            [status $replica master_link_status] eq "up"
        } else {
            fail "the restarted replica did not find its primary"
        }
        wait_for_condition 150 100 {
            [status $subreplica master_link_status] eq "up" &&
            [status $subreplica master_repl_offset] == [status $replica master_repl_offset]
        } else {
            fail "the sub-replica did not find the restarted replica"
        }
        gr_phase $primary $replicas $tkeys "restarted replica"
        gr_timed_workload $primary $tgates 300
        gr_phase $primary $replicas $tkeys "workload after the replica restarted"
    }

    gr_test {Gate replication over time: a replica promoted in the middle of it all} {
        gr_timed_usual
        # Requests wait on the primary, some of them left behind by their
        # proof, and answers are owed.
        gr_timed_workload $primary $tgates 300
        foreach key $tkeys {
            $primary gresume $key
        }
        foreach gate $tgates {
            gr_timed_owe $primary $gate
        }
        gr_phase $primary $replicas $tkeys "before the promotion"
        set before [dict create]
        foreach key $tkeys {
            dict set before $key [gr_info $replica $key]
            assert {[dict get $before $key pending] > 0}
        }

        assert_equal OK [$replica replicaof no one]
        assert_equal master [lindex [$replica role] 0]
        gr_watch -2
        # The new primary cannot know that it has missed nothing: its
        # gates will drop their proof and their waiting requests at their
        # first write, and GINFO says so now.
        set announced [dict create]
        foreach key $tkeys {
            set info [gr_info $replica $key]
            dict set announced $key [gr_recovery_view $replica $key]
            assert {[gr_not_answering $info]}
            assert_equal 0 [dict get $info pending]
            foreach field {learned missing revocations} {
                assert_equal [dict get $before $key $field] [dict get $info $field] "$field of $key"
            }
        }
        assert_equal [$primary debug digest-value {*}$tkeys] [$replica debug digest-value {*}$tkeys]

        # Every answer the teacher owed is for a request of the old
        # primary: the new one uses none of them.
        gr_timed_void $tkeys
        set refused $::gr_timed(refused)
        gr_timed_deliver $replica 1
        assert {$::gr_timed(refused) >= $refused + [llength $tkeys]}
        foreach key $tkeys {
            assert_equal [dict get $announced $key] [gr_recovery_view $replica $key] \
                "GINFO $key before its first write on the new primary (first) and after (second)"
        }
        gr_assert_same $replica $subreplica $tkeys "behind the promoted replica"
        gr_check_watched

        # The workload goes on, on the new primary.
        for {set round 1} {$round <= 2} {incr round} {
            gr_timed_workload $replica $tgates 300
            gr_phase $replica $subreplica $tkeys "workload on the promoted replica, round $round"
        }

        # The old primary gives up and follows. Whatever it still held,
        # it ends up with the gates of the new primary.
        assert_equal OK [$primary replicaof $replica_host $replica_port]
        wait_for_sync $primary
        gr_watch 0 -2
        gr_phase $replica [list $primary $subreplica] $tkeys "the old primary as a replica"
        gr_timed_workload $replica $tgates 300
        gr_phase $replica [list $primary $subreplica] $tkeys "workload with the old primary as a replica"
        gr_timed_until_active $replica $tgates
        gr_phase $replica [list $primary $subreplica] $tkeys "the gates answer on the new primary"
        gr_assert_same_dataset $replica [list $primary $subreplica] "the old primary as a replica"
    }

    gr_test {Gate replication over time: FAILOVER back to the first primary in the middle of it all} {
        gr_timed_usual
        gr_timed_workload $replica $tgates 300
        foreach gate $tgates {
            gr_timed_owe $replica $gate
        }
        gr_phase $replica [list $primary $subreplica] $tkeys "before FAILOVER"

        # The primary of the moment hands over to the server that was the
        # primary at first, and becomes its replica.
        assert_equal OK [$replica failover to $primary_host $primary_port]
        wait_for_condition 100 100 {
            [status $replica master_failover_state] eq "no-failover" &&
            [lindex [$primary role] 0] eq "master" &&
            [status $replica master_link_status] eq "up" &&
            [status $subreplica master_link_status] eq "up"
        } else {
            fail "FAILOVER to the first primary did not finish"
        }
        gr_watch -1 -2
        # It holds the gates it was sent while it was a replica. Nothing
        # shows that it was sent everything.
        foreach key $tkeys {
            set info [gr_info $primary $key]
            assert {[gr_not_answering $info]}
            assert_equal 0 [dict get $info pending]
        }
        gr_timed_void $tkeys
        set refused $::gr_timed(refused)
        gr_timed_deliver $primary 1
        assert {$::gr_timed(refused) >= $refused + [llength $tkeys]}
        gr_phase $primary $replicas $tkeys "after FAILOVER"
        for {set round 1} {$round <= 2} {incr round} {
            gr_timed_workload $primary $tgates 300
            gr_phase $primary $replicas $tkeys "workload after FAILOVER, round $round"
        }
        gr_timed_until_active $primary $tgates
        gr_phase $primary $replicas $tkeys "the gates answer on the first primary again"
        gr_assert_same_dataset $primary $replicas "at the end"
    }
}
}
}
# ----------------------------------------- a replica attached to live gates
#
# The gates exist, answer and have requests waiting before the replica is
# attached: it gets them as a whole, through a full synchronization. Once
# with the dataset written to a file by the primary and loaded from a file by
# the replica, once sent straight from the primary's memory and loaded from
# the socket into a database that is swapped in at the end
# (repl-diskless-load swapdb).

foreach diskless_load {disabled swapdb} {
start_server [list tags {"modules external:skip gate:whole"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    gr_watch -1
    set ::gr_pending {}
    set ::gr_used {}

    gr_test "Gate replication: a replica attached to gates that already exist gets them whole (repl-diskless-load $diskless_load)" {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 40
        $primary config set gate.feedback-timeout 600000
        set waiting {}

        # A score gate that answers, with spot checks waiting for feedback.
        assert_equal OK [$primary gcreate late:active ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        gr_score_until_active $primary late:active
        for {set n 0} {$n < 500 && [llength $waiting] < 12} {incr n} {
            lassign [$primary gpredict late:active SCORE 0.95] action id
            if {$action eq "AUDIT"} {
                lappend waiting late:active $id CORRECT 1
            }
        }
        assert_equal 12 [llength $waiting]

        # A score gate half way: five answers wait behind a request that
        # has none.
        assert_equal OK [$primary gcreate late:calib ALPHA 0.2 DELTA 0.2 SCORE]
        set ids {}
        for {set n 0} {$n < 6} {incr n} {
            lassign [$primary gpredict late:calib SCORE 0.8] action id
            lappend ids $id
        }
        foreach id [lrange $ids 1 end] {
            assert_equal 1 [$primary glearn late:calib $id CORRECT 1]
        }
        lappend waiting late:calib [lindex $ids 0] CORRECT 1

        # A classifier that answers, with requests waiting.
        assert_equal OK [$primary gcreate late:text ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd late:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary late:text {spam ham}
        set asked 0
        for {set n 0} {$n < 500 && $asked < 4} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            lassign [$primary gpredict late:text TEXT [gr_text $label $n]] action id
            if {$action ne "ACCEPT" && $id ne {}} {
                lappend waiting late:text $id LABEL $label
                incr asked
            }
        }
        assert_equal 4 $asked

        # A classifier that has learned little: its requests wait with
        # everything the model will need to learn from them.
        assert_equal OK [$primary gcreate late:cold ALPHA 0.1 CLASSES 3 billing tech other]
        for {set n 0} {$n < 5} {incr n} {
            $primary gadd late:cold TEXT [gr_text billing $n] LABEL billing
        }
        lassign [$primary gpredict late:cold TEXT [gr_text tech 1] FEATURES 2 urgent 1 depth -0.5] action id
        lappend waiting late:cold $id LABEL tech
        lassign [$primary gpredict late:cold TEXT "[string repeat {kernel panic at boot? } 300]\x00\x01 https://example.com"] action id
        lappend waiting late:cold $id LABEL tech
        lassign [$primary gpredict late:cold TEXT ""] action id
        lappend waiting late:cold $id LABEL other

        # A gate that would answer, paused.
        assert_equal OK [$primary gcreate late:paused ALPHA 0.2 DELTA 0.2 SCORE]
        gr_score_until_active $primary late:paused
        assert_equal OK [$primary gpause late:paused]

        # A gate that was reset after it had answered.
        assert_equal OK [$primary gcreate late:reset ALPHA 0.2 DELTA 0.2 SCORE]
        gr_score_until_active $primary late:reset
        assert_equal OK [$primary greset late:reset]
        gr_score_request $primary late:reset 0.9

        # A gate whose drift detector fired.
        assert_equal OK [$primary gcreate late:drift ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        gr_score_until_active $primary late:drift
        for {set n 0} {$n < 200 && [gr_field $primary late:drift revocations] == 0} {incr n} {
            lassign [$primary gpredict late:drift SCORE 0.95] action id
            $primary glearn late:drift $id CORRECT 0
        }
        assert_equal 1 [gr_field $primary late:drift revocations]

        # A gate with a time to live.
        assert_equal OK [$primary gcreate late:ttl ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal 1 [$primary expire late:ttl 3600]

        # A classifier with 64 labels: megabytes of weights, twice (the
        # model that learns and its first snapshot).
        set labels {}
        for {set n 0} {$n < 64} {incr n} {
            lappend labels "class-$n"
        }
        assert_equal OK [$primary gcreate late:wide ALPHA 0.1 CLASSES 64 {*}$labels]
        for {set n 0} {$n < 64} {incr n} {
            $primary gadd late:wide TEXT "words about class $n and [gr_text other $n]" LABEL "class-$n"
        }
        lassign [$primary gpredict late:wide TEXT "words about class 7"] action id
        lappend waiting late:wide $id LABEL class-7

        # A gate that is full: 4,096 requests wait for their feedback, and
        # it takes no more.
        assert_equal OK [$primary gcreate late:full ALPHA 0.2 DELTA 0.2 SCORE]
        for {set n 0} {$n < 4096} {incr n} {
            lassign [$primary gpredict late:full SCORE 0.9] action id
            if {$n == 0} {
                set oldest $id
            }
        }
        assert_equal {ABSTAIN {}} [$primary gpredict late:full SCORE 0.9]

        # A classifier whose waiting requests are long texts: as many as it
        # will take, megabytes of clues.
        assert_equal OK [$primary gcreate late:long ALPHA 0.2 CLASSES 2 spam ham]
        set long {}
        for {set n 0} {$n < 1000} {incr n} {
            lassign [$primary gpredict late:long TEXT [gr_long_text $n]] action id
            if {$id eq {}} break
            lappend long $id
        }
        assert {[llength $long] > 100}

        set gates {late:active late:calib late:text late:cold late:paused late:reset late:drift late:ttl late:wide}
        set big {late:full late:long}
        set everything [concat $gates $big]
        set digests [$primary debug digest-value {*}$everything]

        $primary config set repl-diskless-sync [expr {$diskless_load eq "disabled" ? "no" : "yes"}]
        $replica config set repl-diskless-load $diskless_load
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        assert_equal 1 [status $primary sync_full]
        # Sending the gates did not change them.
        assert_equal $digests [$primary debug digest-value {*}$everything]
        gr_assert_same $primary $replica $everything "after the full synchronization"
        assert_equal ACTIVE [gr_field $replica late:active state]
        assert_equal ACTIVE [gr_field $replica late:text state]
        assert_equal PAUSED [gr_field $replica late:paused state]
        assert_equal 1 [gr_field $replica late:drift revocations]
        assert {[$replica ttl late:ttl] > 0}
    }

    gr_test "Gate replication: feedback for requests made before the replica was attached (repl-diskless-load $diskless_load)" {
        # The replica got the waiting requests with the gates: their
        # feedback, sent now, is used by the primary and must do on the
        # replica what it does on the primary.
        foreach {key id word answer} $waiting {
            assert_equal 1 [$primary glearn $key $id $word $answer] "GLEARN $key $id"
        }
        gr_assert_same $primary $replica $gates "feedback after the full synchronization"
        foreach node [list $primary $replica] {
            assert_equal 8 [gr_field $node late:cold learned]
            assert_equal 6 [gr_field $node late:calib calibration-samples]
        }

        # The full gate has room for one request once one got its
        # feedback, and for one only.
        assert_equal 1 [$primary glearn late:full $oldest CORRECT 1]
        lassign [$primary gpredict late:full SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        assert_equal {ABSTAIN {}} [$primary gpredict late:full SCORE 0.9]
        foreach id [lrange $long 0 19] {
            assert_equal 1 [$primary glearn late:long $id LABEL spam]
        }
        lassign [$primary gpredict late:long TEXT [gr_long_text 2000]] action id
        assert_equal ABSTAIN $action
        gr_assert_same $primary $replica $big "the fullest gates after the full synchronization"
        foreach node [list $primary $replica] {
            assert_equal 20 [gr_field $node late:long learned]
        }
    }

    set mix {
        {late:active score {} 0}
        {late:calib score {} 0}
        {late:text text {spam ham} 0}
        {late:cold text {billing tech other} 0}
        {late:paused score {} 0}
        {late:reset score {} 0}
        {late:drift score {} 0}
        {late:ttl score {} 0}
    }

    gr_test "Gate replication: a replica attached late stays equal under further workload (repl-diskless-load $diskless_load)" {
        for {set round 1} {$round <= 3} {incr round} {
            gr_workload $primary $mix 600
            gr_assert_same $primary $replica $gates "workload after the full synchronization, round $round"
        }
    }

    gr_test "Gate replication: a replica that lost its link catches up from the backlog (repl-diskless-load $diskless_load)" {
        set sync_full [status $primary sync_full]
        set sync_partial [status $primary sync_partial_ok]
        # The link is cut and the primary goes on. What the replica missed
        # is still in the backlog: it asks for that, and applies it to the
        # gates it kept.
        $primary multi
        $primary client kill type replica
        $primary gadd late:cold TEXT [gr_text billing 7] LABEL billing
        $primary gpredict late:calib SCORE 0.9
        $primary exec
        gr_workload $primary $mix 300
        wait_for_condition 100 100 {
            [status $primary sync_partial_ok] > $sync_partial
        } else {
            fail "the replica did not come back with a partial resynchronization"
        }
        wait_for_sync $replica
        assert_equal $sync_full [status $primary sync_full]
        gr_assert_same $primary $replica $gates "after the partial resynchronization"
    }

    gr_test "Gate replication: a replica forced to synchronize again ends up equal (repl-diskless-load $diskless_load)" {
        set sync_full [status $primary sync_full]
        # Drop the link and, before the replica is back, write more than
        # the backlog holds: the replica has to take the whole dataset
        # again, this time while it holds gates of its own.
        $primary config set repl-backlog-size 16384
        # The dataset is sent slowly, so that there is time to write while
        # it travels.
        $primary config set rdb-key-save-delay 20000
        $primary multi
        $primary client kill type replica
        for {set n 0} {$n < 40} {incr n} {
            $primary gadd late:cold TEXT "[gr_text tech $n] [string repeat {filler } 150]" LABEL tech
            $primary gpredict late:calib SCORE 0.9
        }
        $primary exec
        wait_for_condition 100 100 {
            [status $primary sync_full] > $sync_full
        } else {
            fail "the replica did not start a full synchronization"
        }
        # What happens meanwhile is applied on top of what the replica
        # loads.
        gr_workload $primary $mix 300
        $primary config set rdb-key-save-delay 0
        wait_for_sync $replica
        $primary config set repl-backlog-size 1048576
        gr_assert_same $primary $replica $everything "after the second full synchronization"
        gr_workload $primary $mix 400
        gr_assert_same $primary $replica $gates "workload after the second full synchronization"
    }

    gr_test "Gate replication: a restarted replica with nothing on disk takes the gates again (repl-diskless-load $diskless_load)" {
        set sync_full [status $primary sync_full]
        # Remember whose replica this is, and leave no dataset behind: a
        # replica that loaded its last synchronization from a file still
        # has that file.
        $replica config rewrite
        file delete -force [file join [dict get [srv -1 config] dir] dump.rdb]
        restart_server -1 true false
        set replica [srv -1 client]
        # The primary goes on while the replica finds its way back.
        gr_workload $primary $mix 400
        wait_for_sync $replica
        assert_equal [expr {$sync_full + 1}] [status $primary sync_full]
        gr_assert_same $primary $replica $everything "restarted replica"
        gr_workload $primary $mix 400
        gr_assert_same $primary $replica $gates "workload after the replica restarted"
    }

    gr_test "Gate replication: a restarted replica goes on from the gates it saved (repl-diskless-load $diskless_load)" {
        # The replica saves its gates, and its place in the stream with
        # them. What it applies afterwards is not in the file.
        $replica bgsave
        waitForBgsave $replica
        gr_workload $primary $mix 400
        gr_assert_same $primary $replica $gates "workload after the replica saved"

        set sync_full [status $primary sync_full]
        set sync_partial [status $primary sync_partial_ok]
        restart_server -1 true false
        set replica [srv -1 client]
        gr_workload $primary $mix 400
        wait_for_sync $replica
        # It loaded the gates it had saved and asked only for what came
        # after: all of that was applied to gates read from a file.
        assert_equal $sync_full [status $primary sync_full]
        assert_equal [expr {$sync_partial + 1}] [status $primary sync_partial_ok]
        gr_assert_same $primary $replica $everything "restarted replica, partial resynchronization"
        gr_workload $primary $mix 400
        gr_assert_same $primary $replica $gates "workload after the partial resynchronization"
    }

    gr_test "Gate replication: a restarted primary starts its gates over and its replica follows (repl-diskless-load $diskless_load)" {
        gr_workload_drain $primary
        assert_equal OK [$primary gresume late:active]
        gr_score_until_active $primary late:active 2000 0.5
        # A spot check that waits for its feedback.
        for {set n 0} {$n < 500} {incr n} {
            lassign [$primary gpredict late:active SCORE 0.99] action id
            if {$action eq "AUDIT"} break
        }
        assert_equal AUDIT $action
        gr_assert_same $primary $replica $everything "before the primary restarts"
        assert_equal ACTIVE [gr_field $replica late:active state]

        # The primary saves its dataset when it stops and loads it when it
        # starts again.
        $primary config set save "3600 1"
        $primary config rewrite
        restart_server 0 true false
        set primary [srv 0 client]
        wait_for_condition 100 100 {
            [status $replica master_link_status] eq "up"
        } else {
            fail "the replica did not find the restarted primary"
        }
        # Nothing was written: the replica holds what the primary loaded.
        gr_assert_same_digests $primary $replica $everything "primary restarted"

        # The first write to a gate makes the primary drop the gate's
        # proof and its waiting requests, and the replica with it.
        assert_equal 0 [$primary glearn late:active $id CORRECT 1]
        foreach key {late:active late:calib late:reset late:drift late:ttl late:full} {
            lassign [$primary gpredict $key SCORE 0.99] action id
            assert_equal ABSTAIN $action "the first GPREDICT $key on the restarted primary"
            if {$id ne {}} {
                assert_equal 1 [$primary glearn $key $id CORRECT 1]
            }
        }
        assert_equal ABSTAIN [lindex [$primary gpredict late:long TEXT [gr_long_text 3000]] 0]
        $primary gadd late:text TEXT [gr_text spam 1] LABEL spam
        $primary gadd late:cold TEXT [gr_text tech 1] LABEL tech
        $primary gadd late:wide TEXT "words about class 1" LABEL class-1
        assert_equal OK [$primary gresume late:paused]
        gr_assert_same $primary $replica $everything "after the first writes of the restarted primary"
        foreach node [list $primary $replica] {
            foreach key $everything {
                assert {[gr_not_answering [gr_info $node $key]]}
            }
            # 4,096 requests were waiting.
            assert_equal 0 [gr_field $node late:full pending]
        }

        gr_workload $primary $mix 400
        gr_assert_same $primary $replica $gates "workload after the primary restarted"
        gr_assert_same_dataset $primary $replica "at the end"
    }
}
}
}

# ------------------------------- replicas attached in the middle of it all
#
# The workload with a teacher that takes its time runs on a primary that has
# no replica yet. Then a replica is attached, and a replica to that. They get
# the gates whole, through a full synchronization: with requests that wait
# for their answers, requests their proof has moved on without, and gates
# that have stopped for want of answers.

start_server [list tags {"modules external:skip gate:middle"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    gr_watch
    gr_timed_start

    set mgates {
        {mid:s1 score {} 120}
        {mid:s2 score {} 300}
        {mid:c1 text {spam ham} 120}
        {mid:c2 text {billing tech other} 300}
    }
    set mkeys {mid:s1 mid:s2 mid:c1 mid:c2}

    gr_test {Gate replication over time: a workload on a primary that has no replica yet} {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 40
        $primary config set gate.feedback-timeout 120
        assert_equal OK [$primary gcreate mid:s1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 SCORE]
        assert_equal OK [$primary gcreate mid:c1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 300
        assert_equal OK [$primary gcreate mid:s2 ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        assert_equal OK [$primary gcreate mid:c2 ALPHA 0.2 DELTA 0.2 AUDIT 0.4 CLASSES 3 billing tech other ALWAYSDEFER 1 other]
        $primary config set gate.snapshot-examples 20
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd mid:c1 TEXT [gr_text $label $n] LABEL $label
            }
            foreach label {billing tech} {
                $primary gadd mid:c2 TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_timed_until_active $primary $mgates

        # A request that its proof will leave behind before the replicas
        # are attached, and whose answer arrives after: the feedback timeout
        # of this gate is long enough for a synchronization.
        $primary config set gate.feedback-timeout 4000
        assert_equal OK [$primary gcreate mid:late ALPHA 0.2 DELTA 0.2 SCORE]
        set late_t0 [clock milliseconds]
        set ids {}
        for {set n 0} {$n < 3} {incr n} {
            lassign [$primary gpredict mid:late SCORE 0.9] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        set late_t1 [clock milliseconds]
        set late [lindex $ids 0]
        assert_equal 1 [$primary glearn mid:late [lindex $ids 1] CORRECT 1]
        assert_equal 1 [$primary glearn mid:late [lindex $ids 2] CORRECT 1]

        # The workload meanwhile: answers late, out of order or never.
        set ::gr_fates {now 55 soon 15 late 15 toolate 8}
        for {set round 1} {$round <= 3} {incr round} {
            gr_timed_workload $primary $mgates 400
            foreach key $mkeys {
                $primary gresume $key
            }
        }

        # Gates that stop because no answer arrives: 32 requests in a row
        # are given up. A request made afterwards waits for its answer.
        $primary config set gate.feedback-timeout 300
        assert_equal OK [$primary gcreate mid:stop ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate mid:stop:text ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        for {set n 0} {$n < 10} {incr n} {
            foreach label {spam ham} {
                $primary gadd mid:stop:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        for {set n 0} {$n < 32} {incr n} {
            assert_equal ABSTAIN [lindex [$primary gpredict mid:stop SCORE 0.9] 0]
            assert_equal ABSTAIN [lindex [$primary gpredict mid:stop:text TEXT [gr_text spam $n]] 0]
        }
        set stop_t1 [clock milliseconds]

        # A request that its proof will leave behind, and that will have
        # been given up by the time its gate is written to again.
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            set klost mid:lost:$attempt
            $primary config set gate.feedback-timeout 1000
            assert_equal OK [$primary gcreate $klost ALPHA 0.2 DELTA 0.2 SCORE]
            set lost_t0 [clock milliseconds]
            set ids {}
            for {set n 0} {$n < 3} {incr n} {
                lassign [$primary gpredict $klost SCORE 0.9] action id
                assert_equal ABSTAIN $action
                lappend ids $id
            }
            set lost_t1 [clock milliseconds]
            set lost [lindex $ids 0]
            assert_equal 1 [$primary glearn $klost [lindex $ids 1] CORRECT 1]
            assert_equal 1 [$primary glearn $klost [lindex $ids 2] CORRECT 1]
            gr_wait_age $lost_t1 250
            assert_equal OK [$primary gresume $klost]
            set info [gr_info $primary $klost]
            if {![gr_in_time $lost_t0 1000]} continue
            assert_equal 3 [dict get $info calibration-samples]
            assert_equal 1 [dict get $info pending]
            assert_equal 0 [dict get $info missing]
            set done 1
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
        $primary config set gate.feedback-timeout 600000

        # Answers that wait for an older one whose time is far from over.
        assert_equal OK [$primary gcreate mid:head ALPHA 0.2 DELTA 0.2 SCORE]
        set ids {}
        for {set n 0} {$n < 6} {incr n} {
            lassign [$primary gpredict mid:head SCORE 0.9] action id
            lappend ids $id
        }
        set head [lindex $ids 0]
        foreach id [lrange $ids 1 end] {
            assert_equal 1 [$primary glearn mid:head $id CORRECT 1]
        }

        # As many answers as a gate lets wait for an older one, however
        # young that one is: 16,384.
        assert_equal OK [$primary gcreate mid:waiting ALPHA 0.2 DELTA 0.2 SCORE]
        lassign [$primary gpredict mid:waiting SCORE 0.9] action waiting_head
        assert_equal ABSTAIN $action
        assert_equal 16384 [$primary eval $::gr_answered_requests 1 mid:waiting 16384]
        assert_equal 0 [gr_field $primary mid:waiting calibration-samples]
        assert_equal 1 [gr_field $primary mid:waiting pending]

        # The gates without answers stop now ...
        gr_wait_age $stop_t1 300
        lassign [$primary gpredict mid:stop SCORE 0.9] action stop_id
        assert_equal ABSTAIN $action
        assert {$stop_id ne {}}
        $primary gadd mid:stop:text TEXT [gr_text ham 40] LABEL ham
        lassign [$primary gpredict mid:stop:text TEXT [gr_text ham 41]] action stop_text_id
        assert_equal ABSTAIN $action
        set stop_t2 [clock milliseconds]
        foreach key {mid:stop mid:stop:text} {
            set info [gr_info $primary $key]
            assert_equal COLD [dict get $info state]
            assert_equal 0 [dict get $info candidate]
            assert_equal 32 [dict get $info missing]
            assert_equal 1 [dict get $info pending]
        }
        # ... and the proof of mid:late moves on, a quarter of its feedback
        # timeout after the request it waited for.
        gr_wait_age $late_t1 1000
        assert_equal OK [$primary gresume mid:late]
        set info [gr_info $primary mid:late]
        # (Not left behind but given up only if this test took four seconds.)
        set left_behind [gr_in_time $late_t0 4000]
        if {$left_behind} {
            assert_equal 3 [dict get $info calibration-samples]
            assert_equal 1 [dict get $info pending]
            assert_equal 0 [dict get $info missing]
        }
    }

    gr_test {Gate replication over time: replicas attached in the middle get the requests that wait, also those their proof has left behind} {
        set everything [concat $mkeys [list mid:late $klost mid:head mid:waiting mid:stop mid:stop:text]]
        set digests [$primary debug digest-value {*}$everything]
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $subreplica
        gr_watch -1 -2
        assert_equal 1 [status $primary sync_full]
        # Sending the gates did not change them.
        assert_equal $digests [$primary debug digest-value {*}$everything]
        gr_phase $primary $replicas $everything "after the full synchronization"
        foreach node $replicas {
            assert_equal 1 [gr_field $node $klost pending]
            assert_equal 3 [gr_field $node $klost calibration-samples]
            assert_equal 1 [gr_field $node mid:head pending]
            assert_equal 0 [gr_field $node mid:head calibration-samples]
            assert_equal 1 [gr_field $node mid:waiting pending]
            assert_equal 0 [gr_field $node mid:waiting calibration-samples]
            assert_equal COLD [gr_field $node mid:stop state]
            assert_equal COLD [gr_field $node mid:stop:text state]
        }

        # The answer the proof of mid:late did not wait for arrives now. It
        # is used, and the guess that was counted as wrong is set right, on
        # every node: the gate answers after as many right answers as a
        # gate needs that never met a wrong guess.
        set used [$primary glearn mid:late $late CORRECT 1]
        set in_time [gr_in_time $late_t0 4000]
        gr_phase $primary $replicas $everything "a late answer after the full synchronization"
        if {$left_behind && $in_time} {
            assert_equal 1 $used "GLEARN mid:late for a request its proof had left behind"
            set flawless [gr_right_answers 0.2 0.2 1 1]
            gr_right_requests $primary mid:late [expr {$flawless - 3 - 1}]
            gr_sync $primary $replicas
            foreach node $nodes {
                assert_equal CALIBRATING [gr_field $node mid:late state]
            }
            gr_right_requests $primary mid:late 1
            gr_phase $primary $replicas $everything "the late answer set right"
            foreach node $nodes {
                assert_equal ACTIVE [gr_field $node mid:late state]
            }
        }

        # The other request that was left behind is given up at the next
        # write to its gate.
        gr_wait_age $lost_t1 1000
        assert_equal OK [$primary gresume $klost]
        gr_phase $primary $replicas $everything "a request given up after the full synchronization"
        foreach node $nodes {
            assert_equal 1 [gr_field $node $klost missing]
            assert_equal 0 [gr_field $node $klost pending]
            assert_equal 3 [gr_field $node $klost calibration-samples]
        }
        assert_equal 0 [$primary glearn $klost $lost CORRECT 1]

        # The oldest answer lets the five that waited through.
        assert_equal 1 [$primary glearn mid:head $head CORRECT 1]
        gr_phase $primary $replicas $everything "an answer for a request made before the replicas were attached"
        foreach node $nodes {
            assert_equal 6 [gr_field $node mid:head calibration-samples]
            assert_equal 0 [gr_field $node mid:head pending]
        }

        # One answer more than a gate lets wait: proof moves on without the
        # oldest answer at once, and lets all the others through.
        assert_equal 1 [$primary eval $::gr_answered_requests 1 mid:waiting 1]
        gr_phase $primary $replicas $everything "more answers waiting than a gate lets wait"
        foreach node $nodes {
            set info [gr_info $node mid:waiting]
            assert_equal 16386 [dict get $info calibration-samples]
            assert_equal ACTIVE [dict get $info state]
            assert_equal 1 [dict get $info pending]
            assert_equal 0 [dict get $info missing]
        }
        assert_equal 1 [$primary glearn mid:waiting $waiting_head CORRECT 1]
        gr_phase $primary $replicas $everything "the oldest answer after all"
        foreach node $nodes {
            assert_equal 0 [gr_field $node mid:waiting pending]
        }

        # The gates that had stopped still take no snapshot. The requests
        # that waited in them are given up by now, and the replicas hear
        # of it; an answer to a new one, and they are tested again.
        gr_wait_age $stop_t2 300
        assert_equal 0 [$primary glearn mid:stop $stop_id CORRECT 1]
        assert_equal 0 [$primary glearn mid:stop:text $stop_text_id LABEL ham]
        gr_phase $primary $replicas $everything "requests given up in gates that had stopped"
        foreach node $nodes {
            foreach key {mid:stop mid:stop:text} {
                assert_equal COLD [gr_field $node $key state] "state of $key"
                assert_equal 0 [gr_field $node $key candidate] "candidate of $key"
                assert_equal 0 [gr_field $node $key pending]
                assert_equal 32 [gr_field $node $key missing]
            }
        }
        lassign [$primary gpredict mid:stop SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn mid:stop $id CORRECT 1]
        lassign [$primary gpredict mid:stop:text TEXT [gr_text ham 42]] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn mid:stop:text $id LABEL ham]
        gr_phase $primary $replicas $everything "answers for gates that had stopped"
        foreach node $nodes {
            foreach key {mid:stop mid:stop:text} {
                assert_equal CALIBRATING [gr_field $node $key state] "state of $key"
                assert_equal 2 [gr_field $node $key candidate] "candidate of $key"
            }
        }
    }

    gr_test {Gate replication over time: replicas attached in the middle stay equal while the workload goes on} {
        # The answers the teacher owed when the replicas were attached are
        # for requests they received with the gates.
        for {set round 1} {$round <= 3} {incr round} {
            gr_timed_workload $primary $mgates 400
            gr_phase $primary $replicas $everything "workload after the full synchronization, round $round"
            foreach key $mkeys {
                $primary gresume $key
            }
        }
        gr_assert_same_dataset $primary $replicas "at the end"
    }
}
}
}

# ------------------------------------------------------------------ failover
#
# Two servers that swap roles three times: REPLICAOF NO ONE on the replica,
# FAILOVER back, and REPLICAOF NO ONE again while the primary still takes
# writes. 'first' is the primary when the block starts, 'second' its replica.

start_server [list tags {"modules external:skip gate:failover"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set first [srv 0 client]
    set first_host [srv 0 host]
    set first_port [srv 0 port]
    set second [srv -1 client]
    set second_host [srv -1 host]
    set second_port [srv -1 port]
    gr_watch -1
    set ::gr_pending {}
    set ::gr_used {}

    set gates {fo:score fo:text fo:calib fo:paused fo:cold}
    set mix {
        {fo:score score {} 0}
        {fo:text text {spam ham} 0}
        {fo:calib score {} 0}
        {fo:paused score {} 0}
        {fo:cold text {billing tech other} 0}
    }
    # A first request for each gate.
    set probes {
        fo:score {SCORE 0.99}
        fo:text {TEXT {winner prize money}}
        fo:calib {SCORE 0.99}
        fo:paused {SCORE 0.99}
        fo:cold {TEXT {invoice refund charge}}
    }
    set truth {
        fo:score {CORRECT 1}
        fo:text {LABEL spam}
        fo:calib {CORRECT 1}
        fo:paused {CORRECT 1}
        fo:cold {LABEL billing}
    }

    gr_test {Gate failover: gates that answer and requests that wait, on a primary and its replica} {
        $first config set gate.test-seed 0
        $first config set gate.snapshot-examples 40
        $first config set gate.feedback-timeout 600000
        # No PING may move the offset of a primary that has lost its
        # replica: it must be able to go on from where the replica left.
        $first config set repl-ping-replica-period 3600
        $second config set repl-ping-replica-period 3600
        $second replicaof $first_host $first_port
        wait_for_sync $second
        set stale {}

        # A score gate that answers, with spot checks waiting.
        assert_equal OK [$first gcreate fo:score ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        gr_score_until_active $first fo:score
        for {set n 0} {$n < 500 && [llength $stale] < 12} {incr n} {
            lassign [$first gpredict fo:score SCORE 0.95] action id
            if {$action eq "AUDIT"} {
                lappend stale fo:score $id CORRECT 1
            }
        }
        assert_equal 12 [llength $stale]

        # A classifier that answers, with requests waiting.
        assert_equal OK [$first gcreate fo:text ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $first gadd fo:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $first fo:text {spam ham}
        set asked 0
        for {set n 0} {$n < 500 && $asked < 3} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            lassign [$first gpredict fo:text TEXT [gr_text $label $n]] action id
            if {$action ne "ACCEPT" && $id ne {}} {
                lappend stale fo:text $id LABEL $label
                incr asked
            }
        }
        assert_equal 3 $asked

        # A score gate half way.
        assert_equal OK [$first gcreate fo:calib ALPHA 0.2 DELTA 0.2 SCORE]
        for {set n 0} {$n < 10} {incr n} {
            gr_score_request $first fo:calib 0.9
        }
        for {set n 0} {$n < 3} {incr n} {
            lassign [$first gpredict fo:calib SCORE 0.9] action id
            lappend stale fo:calib $id CORRECT 1
        }

        # A gate that would answer, paused.
        assert_equal OK [$first gcreate fo:paused ALPHA 0.2 DELTA 0.2 SCORE]
        gr_score_until_active $first fo:paused
        assert_equal OK [$first gpause fo:paused]

        # A classifier that has not learned enough for a snapshot.
        assert_equal OK [$first gcreate fo:cold ALPHA 0.2 DELTA 0.2 CLASSES 3 billing tech other]
        for {set n 0} {$n < 6} {incr n} {
            set label [lindex {billing tech} [expr {$n % 2}]]
            $first gadd fo:cold TEXT [gr_text $label $n] LABEL $label
        }
        for {set n 0} {$n < 2} {incr n} {
            lassign [$first gpredict fo:cold TEXT [gr_text other $n]] action id
            lappend stale fo:cold $id LABEL other
        }

        # A gate that answers, with a spot check waiting, to which nobody
        # will write for as long as the roles are swapped.
        assert_equal OK [$first gcreate fo:idle ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        gr_score_until_active $first fo:idle
        for {set n 0} {$n < 500} {incr n} {
            lassign [$first gpredict fo:idle SCORE 0.95] action idle
            if {$action eq "AUDIT"} break
        }
        assert_equal AUDIT $action

        gr_assert_same $first $second [concat $gates fo:idle] "before the failover"
        set before [dict create]
        foreach key $gates {
            dict set before $key [gr_info $second $key]
        }
        set idle_before [gr_info $second fo:idle]
        assert_equal ACTIVE [dict get $idle_before state]
        assert_equal 1 [dict get $idle_before pending]
        assert_equal ACTIVE [dict get $before fo:score state]
        assert_equal ACTIVE [dict get $before fo:text state]
        assert_equal CALIBRATING [dict get $before fo:calib state]
        assert_equal PAUSED [dict get $before fo:paused state]
        assert_equal COLD [dict get $before fo:cold state]
    }

    gr_test {Gate failover: a promoted replica shows that its gates no longer answer} {
        assert_equal OK [$second replicaof no one]
        assert_equal master [lindex [$second role] 0]
        set announced [dict create]
        foreach key $gates {
            set info [gr_info $second $key]
            dict set announced $key [gr_recovery_view $second $key]
            assert {[gr_not_answering $info]}
            assert {[dict get $info state] ne "ACTIVE"}
            # The gate keeps its configuration and its model.
            foreach field {kind alpha delta audit classes alphas learned} {
                assert_equal [dict get $before $key $field] [dict get $info $field] "$field of $key"
            }
        }
        assert_equal PAUSED [gr_field $second fo:paused state]
        # Looking at a gate does not change it, and neither does a command
        # that is refused: arguments are checked before anything changes.
        assert_error {ERR*} {$second gpredict fo:score SCORE 7}
        assert_error {ERR*} {$second gpredict fo:text SCORE 0.5}
        assert_error {ERR*} {$second glearn fo:score [lindex $stale 1] LABEL spam}
        assert_error {ERR*} {$second gadd fo:text TEXT hello LABEL nolabel}
        assert_error {ERR*} {$second gadd fo:score TEXT hello LABEL spam}
        assert_equal [$first debug digest-value {*}$gates] [$second debug digest-value {*}$gates]
        foreach key $gates {
            assert_equal [dict get $announced $key] [gr_recovery_view $second $key]
        }
    }

    gr_test {Gate failover: feedback for requests of the old primary is not used, a first request is not answered} {
        # For three of the gates, the first thing the new primary hears is
        # feedback for a request the old primary made. The paused gate is
        # paused once more.
        foreach {key id word answer} $stale {
            if {$key in {fo:score fo:text fo:cold}} {
                assert_equal 0 [$second glearn $key $id $word $answer] "GLEARN $key $id on the new primary"
            }
        }
        assert_equal OK [$second gpause fo:paused]
        # That feedback was not used. It made the gate drop its proof and
        # its waiting requests, which is what GINFO had announced.
        foreach key {fo:score fo:text fo:cold fo:paused} {
            assert_equal [dict get $announced $key] [gr_recovery_view $second $key] \
                "GINFO $key before its first write (first) and after (second)"
        }

        # For every gate, a first request: the teacher must be asked.
        set fresh {}
        foreach {key request} $probes {
            lassign [$second gpredict $key {*}$request] action id
            assert_equal ABSTAIN $action "the first GPREDICT $key on the new primary"
            if {$id ne {}} {
                lappend fresh $key $id {*}[dict get $truth $key]
            }
        }
        assert {[llength $fresh] > 0}

        # No id of the old primary is good any more; those of the new are.
        foreach {key id word answer} $stale {
            assert_equal 0 [$second glearn $key $id $word $answer] "GLEARN $key $id on the new primary"
        }
        foreach {key id word answer} $fresh {
            assert_equal 1 [$second glearn $key $id $word $answer] "GLEARN $key $id on the new primary"
        }
        foreach key $gates {
            assert {[gr_not_answering [gr_info $second $key]]}
        }
    }

    gr_test {Gate failover: the gates prove themselves again on the new primary} {
        gr_score_until_active $second fo:score
        # The model was kept: a snapshot of it is tested at once.
        gr_text_until_active $second fo:text {spam ham}
        foreach key {fo:score fo:text} {
            set info [gr_info $second $key]
            assert_equal ACTIVE [dict get $info state]
            # A snapshot number is never reused, whatever the node.
            assert {[dict get $info generation] > [dict get $before $key generation]}
            assert {[dict get $info generation] > [dict get $before $key candidate]}
        }
        set accepted 0
        for {set n 0} {$n < 200} {incr n} {
            if {[gr_score_request $second fo:score 0.95] eq "ACCEPT"} {
                incr accepted
            }
        }
        assert {$accepted > 0}
        gr_score_until_active $second fo:calib
        # Paused it was and paused it stays, proving itself all the same:
        # it answers as soon as it is resumed.
        for {set n 0} {$n < 100} {incr n} {
            assert {[gr_score_request $second fo:paused 0.9] ne "ACCEPT"}
        }
        assert_equal PAUSED [gr_field $second fo:paused state]
        assert_equal OK [$second gresume fo:paused]
        assert_equal ACTIVE [gr_field $second fo:paused state]
        assert_equal OK [$second gpause fo:paused]
    }

    gr_test {Gate failover: the old primary, made a replica of the new one, converges} {
        set partial [status $second sync_partial_ok]
        assert_equal OK [$first replicaof $second_host $second_port]
        wait_for_sync $first
        gr_watch 0
        gr_assert_same $second $first $gates "the old primary as a replica"
        assert_equal ACTIVE [gr_field $first fo:score state]
        # It did nothing since its replica left, so it kept its own gates
        # and was only sent what the new primary did meanwhile. Nobody
        # wrote to fo:idle: both servers hold it as it was, the old primary
        # with the proof it collected itself.
        assert_equal [expr {$partial + 1}] [status $second sync_partial_ok]
        gr_assert_same_digests $second $first fo:idle "a gate nobody wrote to since the failover"
        assert_equal [gr_replicated_info $first fo:idle] [dict remove $idle_before {*}$::gr_primary_only]
    }

    gr_test {Gate failover: FAILOVER back, the returning primary does not trust what it remembers} {
        # Requests that wait on the primary of the moment.
        set stale {}
        for {set n 0} {$n < 500 && [llength $stale] < 12} {incr n} {
            lassign [$second gpredict fo:score SCORE 0.95] action id
            if {$action eq "AUDIT"} {
                lappend stale fo:score $id CORRECT 1
            }
        }
        assert_equal 12 [llength $stale]
        for {set n 0} {$n < 500 && [llength $stale] < 20} {incr n} {
            set label [lindex {spam ham} [expr {$n % 2}]]
            lassign [$second gpredict fo:text TEXT [gr_text $label $n]] action id
            if {$action ne "ACCEPT" && $id ne {}} {
                lappend stale fo:text $id LABEL $label
            }
        }
        assert_equal 20 [llength $stale]
        gr_assert_same $second $first $gates "before FAILOVER"
        set before [dict create]
        foreach key $gates {
            dict set before $key [gr_info $first $key]
        }
        assert_equal ACTIVE [dict get $before fo:score state]
        assert_equal ACTIVE [dict get $before fo:text state]

        assert_equal OK [$second failover to $first_host $first_port]
        wait_for_condition 100 100 {
            [status $second master_failover_state] eq "no-failover" &&
            [lindex [$first role] 0] eq "master" &&
            [status $second master_link_status] eq "up"
        } else {
            fail "FAILOVER to the old primary did not finish"
        }
        gr_watch -1

        # These are the gates this server created and once answered for.
        # It has been a replica since: nothing shows that it missed nothing.
        foreach key $gates {
            set info [gr_info $first $key]
            assert {[gr_not_answering $info]}
            assert {[dict get $info state] ne "ACTIVE"}
            assert_equal [dict get $before $key learned] [dict get $info learned]
        }
        set fresh {}
        foreach {key request} $probes {
            lassign [$first gpredict $key {*}$request] action id
            assert_equal ABSTAIN $action "the first GPREDICT $key on the returning primary"
            if {$id ne {}} {
                lappend fresh $key $id {*}[dict get $truth $key]
            }
        }
        foreach {key id word answer} $stale {
            assert_equal 0 [$first glearn $key $id $word $answer] "GLEARN $key $id on the returning primary"
        }
        foreach {key id word answer} $fresh {
            assert_equal 1 [$first glearn $key $id $word $answer] "GLEARN $key $id on the returning primary"
        }
        gr_assert_same $first $second $gates "after FAILOVER"

        # Nor does it trust the gate that nobody wrote to while it was a
        # replica, although it holds that gate exactly as it left it: it
        # has been a replica, and that is all a gate needs to know.
        set info [gr_info $first fo:idle]
        assert {[gr_not_answering $info]}
        assert_equal 0 [dict get $info pending]
        assert {[dict get $info state] ne "ACTIVE"}
        # (Its replica shows the gate as it is until the first write.)
        assert_equal ACTIVE [gr_field $second fo:idle state]
        gr_assert_same_digests $first $second fo:idle "before the first write to a gate that was idle"
        assert_equal 0 [$first glearn fo:idle $idle CORRECT 1] "GLEARN fo:idle for a spot check from before the roles were swapped"
        lassign [$first gpredict fo:idle SCORE 0.95] action id
        assert_equal ABSTAIN $action "the first GPREDICT fo:idle on the returning primary"
        assert_equal 1 [$first glearn fo:idle $id CORRECT 1]
        gr_assert_same $first $second fo:idle "after the first write to a gate that was idle"
        foreach node [list $first $second] {
            assert {[gr_not_answering [gr_info $node fo:idle]]}
            assert_equal 0 [gr_field $node fo:idle pending]
        }

        for {set round 1} {$round <= 2} {incr round} {
            gr_workload $first $mix 500
            gr_assert_same $first $second $gates "workload after FAILOVER, round $round"
        }
    }

    gr_test {Gate failover: writes the replica never saw are gone once it has taken over} {
        gr_workload_drain $first
        gr_assert_same $first $second $gates "before the second failover"
        set learned [gr_field $second fo:cold learned]

        # The replica is promoted while the primary still takes writes:
        # these never reach it.
        assert_equal OK [$second replicaof no one]
        set lost {}
        for {set n 0} {$n < 20} {incr n} {
            lassign [$first gpredict fo:calib SCORE 0.9] action id
            if {$id ne {}} {
                lappend lost fo:calib $id CORRECT 1
            }
            $first gadd fo:cold TEXT [gr_text tech $n] LABEL tech
        }
        assert_equal OK [$first greset fo:score]
        assert_equal [expr {$learned + 20}] [gr_field $first fo:cold learned]

        # The new primary takes requests of its own.
        foreach {key request} $probes {
            lassign [$second gpredict $key {*}$request] action id
            assert_equal ABSTAIN $action "the first GPREDICT $key on the new primary"
            if {$id ne {}} {
                assert_equal 1 [$second glearn $key $id {*}[dict get $truth $key]]
            }
        }
        set kept [gr_field $second fo:cold learned]
        assert {$kept <= $learned + 1}

        # The old primary gives up and follows: what it did on its own
        # never happened.
        assert_equal OK [$first replicaof $second_host $second_port]
        wait_for_sync $first
        gr_watch 0
        gr_assert_same $second $first $gates "after the writes that were lost"
        assert_equal $kept [gr_field $first fo:cold learned]
        foreach {key id word answer} $lost {
            assert_equal 0 [$second glearn $key $id $word $answer] "GLEARN $key $id, issued by a primary that lost"
        }
        gr_assert_same $second $first $gates "feedback for writes that were lost"

        for {set round 1} {$round <= 2} {incr round} {
            gr_workload $second $mix 500
            gr_assert_same $second $first $gates "workload after the second failover, round $round"
        }
        gr_assert_same_dataset $second $first "at the end"
    }
}
}

# ------------------------------ failover while the old primary still answers
#
# A failover that nobody coordinates leaves a moment in which the replica has
# been promoted and the old primary still answers its clients. What the old
# primary accepts then changes nothing (an ACCEPT writes nothing), so it has
# nothing to give up when it is finally made a replica of the new primary: it
# keeps its gates and is only sent what the new primary did meanwhile.
#
# Each of the two tests has its own servers, so that a replica that differs
# from its primary cannot spoil another test.

# 'first' is a primary and 'second' its replica. Create the score gate 'key'
# with the given feedback timeout (ms), make it answer, promote the replica
# and have it take a request, let the old primary accept one 'pause' ms
# later, then make the old primary a replica of the new one. Returns the id
# of the request the new primary took and the time (ms) it was taken at.
proc gr_failover_with_late_accept {first second key timeout pause} {
    # The same dice in every run, and a gate that hardly ever spot-checks:
    # the request the old primary gets below is accepted.
    $first config set gate.test-seed 20261128
    $first config set gate.feedback-timeout $timeout
    # No PING may move the offset of the old primary: it must be able to
    # go on from where its replica left.
    $first config set repl-ping-replica-period 3600
    $second replicaof [srv 0 host] [srv 0 port]
    wait_for_sync $second
    assert_equal OK [$first gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 0.001 SCORE]
    for {set n 0} {$n < 600 && [gr_field $first $key state] ne "ACTIVE"} {incr n} {
        lassign [$first gpredict $key SCORE 0.9] action id
        $first glearn $key $id CORRECT 1
    }
    assert_equal ACTIVE [gr_field $first $key state]
    gr_assert_same $first $second $key "before the failover"

    # The replica is promoted and takes a request ...
    assert_equal OK [$second replicaof no one]
    lassign [$second gpredict $key SCORE 0.9] action id
    set taken [clock milliseconds]
    assert_equal ABSTAIN $action

    # ... while the old primary, which nobody told, goes on answering its
    # clients. Its place in the stream does not move.
    after $pause
    set offset [status $first master_repl_offset]
    assert_equal {ACCEPT {}} [$first gpredict $key SCORE 0.9]
    assert_equal $offset [status $first master_repl_offset]

    # Then it is made a replica of the new primary.
    set partial [status $second sync_partial_ok]
    assert_equal OK [$first replicaof [srv -1 host] [srv -1 port]]
    wait_for_sync $first
    gr_watch 0
    assert_equal [expr {$partial + 1}] [status $second sync_partial_ok]
    return [list $id $taken]
}

start_server [list tags {"modules external:skip gate:failover"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set first [srv 0 client]
    set second [srv -1 client]
    gr_watch -1

    gr_test {Gate failover: an old primary that went on accepting requests converges all the same} {
        lassign [gr_failover_with_late_accept $first $second sb:score 600000 50] id taken
        gr_assert_same $second $first sb:score "the old primary as a replica"

        # And it keeps following.
        assert_equal 1 [$second glearn sb:score $id CORRECT 1]
        gr_score_until_active $second sb:score
        gr_assert_same $second $first sb:score "the gate proven again"
    }
}
}

start_server [list tags {"modules external:skip gate:failover"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set first [srv 0 client]
    set second [srv -1 client]
    gr_watch -1

    gr_test {Gate failover: an old primary that went on accepting requests misses the feedback its new primary misses} {
        # The request the new primary took waits 400 ms for its feedback.
        # The old primary accepted its last request 250 ms after it.
        lassign [gr_failover_with_late_accept $first $second sb:late 400 250] id taken
        set missing [gr_field $second sb:late missing]

        # The feedback never comes. The new primary finds out at its first
        # write once the 400 ms are over, and so must its replica.
        set wait [expr {$taken + 430 - [clock milliseconds]}]
        if {$wait > 0} {
            after $wait
        }
        assert_equal OK [$second gpause sb:late]
        assert_equal OK [$second gresume sb:late]
        wait_for_ofs_sync $second $first
        assert_equal [expr {$missing + 1}] [gr_field $second sb:late missing]
        assert_equal [gr_replicated_info $second sb:late] [gr_replicated_info $first sb:late] \
            "GINFO on the new primary (first) and on the old one, now its replica (second)"

        # Feedback that comes too late is used by neither.
        assert_equal 0 [$second glearn sb:late $id CORRECT 1]
        wait_for_ofs_sync $second $first
        assert_equal [gr_replicated_info $second sb:late] [gr_replicated_info $first sb:late] \
            "GINFO after late feedback, on the new primary (first) and on its replica (second)"
        gr_assert_same $second $first sb:late "the old primary as a replica"
    }
}
}

# -------------------------------------------- a gate whose clock runs ahead
#
# A gate keeps the latest clock it was written at and never falls behind it.
# The clock a replica is sent is that one, not the one the primary read
# (gate_core.h, TIME). The two differ for a gate that comes from a server
# whose clock ran ahead: after a failover, through DUMP and RESTORE, in a
# file. Two servers on one machine share one clock, so such a gate is made
# here: the dump of a new gate, restored with its clock moved ahead.

# The dump of 'key', a score gate that was not written to since it was
# created, and where in it some numbers of the gate are (gate_codec.c):
# returns {dump clock-offset clock requests-offset snapshots-offset
# missing-offset}.
proc gr_fresh_dump {client key} {
    set dump [$client dump $key]
    # The encoding of a gate starts with "GATE". In a score gate the time it
    # was created at, its clock and the number of snapshots it has taken
    # (one, at its creation), three 64 bit integers, follow 65 bytes later.
    set at [string first GATE $dump]
    set created 0
    set clock 0
    set snapshots 0
    if {$at >= 0} {
        binary scan [string range $dump [expr {$at + 65}] [expr {$at + 88}]] www created clock snapshots
    }
    # The number of the next request, another 64 bit integer, stands before
    # the count of waiting requests (none), the fifteen counters and what
    # DUMP adds after a value (11 bytes): 143 bytes before the end. The
    # counter of missing answers is the fifth counter from the end.
    set requests [expr {[string length $dump] - 143}]
    set missing [expr {[string length $dump] - 51}]
    set next 0
    set missed -1
    binary scan [string range $dump $requests [expr {$requests + 7}]] w next
    binary scan [string range $dump $missing [expr {$missing + 7}]] w missed
    set now [clock milliseconds]
    if {$at < 0 || $created != $clock || $clock > $now || $clock < $now - 60000 || $snapshots != 1 || $next != 1 || $missed != 0} {
        fail "the dump of a gate does not hold its clock, its snapshot number, its request number and its missing answers where this test looks for them (found $created, $clock, $snapshots, $next and $missed; the time is $now): the encoding has changed, and gr_fresh_dump must follow"
    }
    return [list $dump [expr {$at + 73}] $clock $requests [expr {$at + 81}] $missing]
}

# RESTORE a copy of such a gate as 'copy', with a clock that is 'ahead' ms
# ahead. Returns that clock.
proc gr_restore_ahead {client key copy ahead} {
    lassign [gr_fresh_dump $client $key] dump at clock
    set dump [string replace $dump $at [expr {$at + 7}] [binary format w [expr {$clock + $ahead}]]]
    assert_equal OK [$client restore $copy 0 $dump]
    return [expr {$clock + $ahead}]
}

start_server [list tags {"modules external:skip gate:clock"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    gr_watch -1

    gr_test {Gate failover: a gate whose clock is ahead of its primary's is the same gate on the replica} {
        # The dumps are changed by hand, so they must not be compressed, and
        # their checksums are no longer right.
        $primary config set rdbcompression no
        foreach node [list $primary $replica] {
            $node debug set-skip-checksum-validation 1
        }
        $primary config set gate.test-seed 0
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        $primary config set gate.feedback-timeout 600000
        assert_equal OK [$primary gcreate ck:new ALPHA 0.2 DELTA 0.2 SCORE]
        gr_restore_ahead $primary ck:new ck:ahead 3600000
        gr_assert_same_digests $primary $replica {ck:new ck:ahead} "a gate restored with its clock an hour ahead"

        # Whatever clock the primary gives the gate, the replica has it.
        set ids {}
        for {set n 0} {$n < 3} {incr n} {
            lassign [$primary gpredict ck:ahead SCORE 0.9] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        gr_assert_same $primary $replica ck:ahead "requests to a gate whose clock was ahead"
        foreach id [lrange $ids 1 end] {
            assert_equal 1 [$primary glearn ck:ahead $id CORRECT 1]
        }
        assert_equal OK [$primary gpause ck:ahead]
        assert_equal OK [$primary gresume ck:ahead]
        gr_assert_same $primary $replica ck:ahead "answers to a gate whose clock was ahead"
        assert_equal 1 [$primary glearn ck:ahead [lindex $ids 0] CORRECT 1]
        gr_assert_same $primary $replica ck:ahead "a late answer to a gate whose clock was ahead"
        assert_equal OK [$primary greset ck:ahead]
        gr_right_requests $primary ck:ahead 5
        gr_assert_same $primary $replica ck:ahead "after GRESET of a gate whose clock was ahead"
        gr_assert_same_dataset $primary $replica "at the end"
    }

    gr_test {Gate failover: a gate whose clock ran ahead still counts its feedback timeout in real time} {
        # After a restart, a failover or a RESTORE a gate drops its proof and
        # its waiting requests, and GRESET drops everything: the gate
        # "proves itself again by itself" (SPEC.md), on the server it now
        # lives on. Its clock may stay ahead of that server's, but it must
        # not stand still until the server has caught up with it: a gate
        # that did would give up no request and let no proof move on for an
        # hour here, however many answers were lost.
        set timeout 400
        set grace 100
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            # One gate starts over by itself, at its first write after the
            # RESTORE. The other one is reset.
            set kwrite ck:write:$attempt
            set kreset ck:reset:$attempt
            set keys [list $kwrite $kreset]
            $primary config set gate.feedback-timeout $timeout
            assert_equal OK [$primary gcreate ck:fresh:$attempt ALPHA 0.2 DELTA 0.2 SCORE]
            $primary config set gate.feedback-timeout 600000
            gr_restore_ahead $primary ck:fresh:$attempt $kwrite 3600000
            gr_restore_ahead $primary ck:fresh:$attempt $kreset 3600000
            assert_equal OK [$primary greset $kreset]

            # Three requests each; the second and the third are answered.
            set t0 [clock milliseconds]
            set unanswered [dict create]
            foreach key $keys {
                set ids {}
                for {set n 0} {$n < 3} {incr n} {
                    lassign [$primary gpredict $key SCORE 0.9] action id
                    assert_equal ABSTAIN $action
                    lappend ids $id
                }
                dict set unanswered $key [lindex $ids 0]
                assert_equal 1 [$primary glearn $key [lindex $ids 1] CORRECT 1]
                assert_equal 1 [$primary glearn $key [lindex $ids 2] CORRECT 1]
            }
            set t1 [clock milliseconds]

            # A quarter of the feedback timeout later proof moves on ...
            gr_wait_age $t1 $grace
            set moved [dict create]
            foreach key $keys {
                assert_equal OK [$primary gresume $key]
                dict set moved $key [gr_info $primary $key]
            }
            if {![gr_in_time $t0 $timeout]} continue
            gr_assert_same $primary $replica $keys "a quarter of the feedback timeout after the requests"
            foreach key $keys {
                assert_equal 3 [dict get $moved $key calibration-samples] \
                    "calibration-samples of $key, a quarter of its feedback timeout after a request that got no answer"
                assert_equal 1 [dict get $moved $key pending]
            }

            # ... and when the feedback timeout is over the request is
            # given up.
            gr_wait_age $t1 $timeout
            foreach key $keys {
                assert_equal OK [$primary gresume $key]
            }
            gr_assert_same $primary $replica $keys "after the feedback timeout"
            foreach node [list $primary $replica] {
                foreach key $keys {
                    assert_equal 1 [gr_field $node $key missing] "missing of $key after its feedback timeout"
                    assert_equal 0 [gr_field $node $key pending] "pending of $key after its feedback timeout"
                }
            }
            foreach key $keys {
                assert_equal 0 [$primary glearn $key [dict get $unanswered $key] CORRECT 1]
            }
            set done 1
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
    }

    gr_test {Gate failover: an accepted request of a gate whose clock ran ahead tells the replica what fell due, at the clock of the gate} {
        # The dice start over: this test meets the same draws in every run.
        $primary config set gate.test-seed 20261128
        set timeout 300
        set done 0
        for {set attempt 1} {$attempt <= 6 && !$done} {incr attempt} {
            set key ck:tick:$attempt
            $primary config set gate.feedback-timeout $timeout
            assert_equal OK [$primary gcreate ck:plain:$attempt ALPHA 0.2 DELTA 0.2 AUDIT 0.2 SCORE]
            $primary config set gate.feedback-timeout 600000
            set ahead [gr_restore_ahead $primary ck:plain:$attempt $key 3600000]
            for {set n 0} {$n < 200 && [gr_field $primary $key state] ne "ACTIVE"} {incr n} {
                gr_right_requests $primary $key 1
            }
            assert_equal ACTIVE [gr_field $primary $key state]

            # A spot check that never gets its answer.
            set repl [attach_to_replication_stream]
            set t0 [clock milliseconds]
            for {set n 0} {$n < 400} {incr n} {
                lassign [$primary gpredict $key SCORE 0.9] action lost
                if {$action ne "ACCEPT"} break
            }
            assert_equal AUDIT $action
            set t1 [clock milliseconds]
            # When the feedback timeout is over, the next request finds it
            # overdue. Four in five are accepted and send a TICK.
            gr_wait_age $t1 $timeout
            lassign [$primary gpredict $key SCORE 0.9] action id
            set t2 [clock milliseconds]
            $primary set gr:marker 1
            set commands [gr_stream_until $repl {set gr:marker 1}]
            close_replication_stream $repl
            assert_equal 1 [$primary del gr:marker]
            if {$action ne "ACCEPT"} {
                # A spot check: the step travelled with it. Once more.
                assert_equal 1 [$primary glearn $key $id CORRECT 1]
                continue
            }
            set operations [gr_stream_operations $commands [list $key]]
            assert_equal {PREDICT TICK} [list [lindex $operations 0 0] [lindex $operations 1 0]]
            assert_equal 2 [llength $operations]
            set asked [lindex $operations 0 2]
            set given_up [lindex $operations 1 2]
            # The gate never falls behind the clock it came with, and from
            # there on its clock moves as time passes.
            assert {$asked >= $ahead}
            assert {$given_up - $asked >= $timeout}
            assert {$given_up - $asked <= $t2 - $t0 + 5}
            gr_assert_same $primary $replica $key "a spot check given up by an accepted request"
            foreach node [list $primary $replica] {
                assert_equal 1 [gr_field $node $key missing]
                assert_equal 0 [gr_field $node $key pending]
            }
            assert_equal 0 [$primary glearn $key $lost CORRECT 1]
            set done 1
        }
        if {!$done} {
            fail "six attempts, each met by a spot check: this test could not show what it is about"
        }
        $primary config set gate.test-seed 0
        gr_assert_same_dataset $primary $replica "at the end"
    }
}
}

# -------------------------------------------------------- a writable replica
#
# GPREDICT runs on the primary (SPEC.md). A replica that takes writes all the
# same (replica-read-only no) is on its own: what it does to a gate is sent
# nowhere, and from then on its gate is not the gate of its primary. Two
# things hold even so (CHANGES.md, section 8): once such a replica has applied
# an operation of its primary again, it does not answer on proof it built by
# itself; and when an operation of its primary no longer fits what the
# replica has made of the gate, the replica says so, in its log and in INFO.

start_server [list tags {"modules external:skip gate:writable"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    # This replica is meant to differ from its primary, and to say so.
    gr_watch

    gr_test {Gate replication: a writable replica does not answer on proof it built by itself, and says when it can no longer follow} {
        $primary config set gate.test-seed 0
        $primary config set gate.feedback-timeout 600000
        $replica config set gate.feedback-timeout 600000
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        # Every request this gate could answer is a spot check (AUDIT 1):
        # nothing here depends on the dice.
        assert_equal OK [$primary gcreate wr:score ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        gr_right_requests $primary wr:score [gr_right_answers 0.2 0.2 1 1]
        gr_assert_same $primary $replica wr:score "before the replica takes writes"
        assert_equal ACTIVE [gr_field $replica wr:score state]
        set sync_full [status $primary sync_full]

        $replica config set replica-read-only no
        # Its first own request: like a primary after a failover, the
        # replica cannot know what it has missed, and starts the gate over.
        lassign [$replica gpredict wr:score SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$replica glearn wr:score $id CORRECT 1]
        # With answers of its own it gets as far as answering by itself.
        for {set n 0} {$n < 200 && [gr_field $replica wr:score generation] == 0} {incr n} {
            gr_right_requests $replica wr:score 1
        }
        lassign [$replica gpredict wr:score SCORE 0.9] action id
        assert_equal AUDIT $action
        assert_equal 1 [$replica glearn wr:score $id CORRECT 1]
        # None of this reached the primary.
        assert_equal 1 [gr_field $primary wr:score generation]

        # The primary writes to the gate, and the replica applies it.
        assert_equal OK [$primary gpause wr:score]
        assert_equal OK [$primary gresume wr:score]
        gr_sync $primary $replica
        # What the replica holds now is neither the gate of its primary nor
        # a gate it has proven: it must not answer for it.
        lassign [$replica gpredict wr:score SCORE 0.9] action id
        assert_equal ABSTAIN $action "GPREDICT on a writable replica, after it applied an operation of its primary"
        assert {[gr_not_answering [gr_info $replica wr:score]]}
        assert_equal ACTIVE [gr_field $primary wr:score state]

        # So far everything the primary sent could be applied. A request of
        # the primary cannot: the replica has handed out numbers of its own.
        assert_equal 0 [count_message_lines [srv -1 stdout] "did not fit its gate"]
        assert_equal 0 [gr_mismatches $replica]
        lassign [$primary gpredict wr:score SCORE 0.9] action id
        assert_equal AUDIT $action
        assert_equal 1 [$primary glearn wr:score $id CORRECT 1]
        gr_sync $primary $replica
        assert_equal PONG [$replica ping]
        assert_equal 1 [gr_mismatches $replica] "gate_apply_mismatches of a replica that went its own way"
        assert_equal 1 [count_message_lines [srv -1 stdout] "did not fit its gate"]
        assert_equal 0 [status $replica unexpected_error_replies]

        # A full synchronization gives it the gate of its primary again.
        $replica config set replica-read-only yes
        assert_equal OK [$replica replicaof no one]
        assert_equal OK [$replica replicaof $primary_host $primary_port]
        wait_for_sync $replica
        assert_equal [expr {$sync_full + 1}] [status $primary sync_full]
        gr_assert_same $primary $replica wr:score "after a full synchronization"
        assert_equal ACTIVE [gr_field $replica wr:score state]
        assert_error {READONLY*} {$replica gpredict wr:score SCORE 0.9}
        # It follows as any replica does: nothing more fails to fit.
        assert_equal OK [$primary gcreate wr:follow ALPHA 0.2 DELTA 0.2 SCORE]
        gr_right_requests $primary wr:follow 20
        lassign [$primary gpredict wr:score SCORE 0.9] action id
        assert_equal 1 [$primary glearn wr:score $id CORRECT 1]
        gr_assert_same $primary $replica {wr:score wr:follow} "after the full synchronization, following"
        assert_equal 1 [gr_mismatches $replica]
        gr_assert_same_dataset $primary $replica "at the end"
    }
}
}

# ------------------- the drift detector and the comparison of two snapshots
#
# CHANGES.md, section 9. A spot check gets its number when it is asked, the
# detector counts the disagreements and the lost answers of blocks of spot
# checks, a newer snapshot is compared with the answering one request by
# request, and a snapshot that left too much to the teacher is dropped. All
# of that is state a replica must hold exactly as its primary does: the next
# answer may be the one that stops the gate, or the one that lets a snapshot
# take over.
#
# The chain once more: primary -> replica (with an AOF) -> sub-replica,
# attached before any gate exists.

start_server [list tags {"modules external:skip gate:drift"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule appendonly yes auto-aof-rewrite-percentage 0]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    gr_watch -1 -2

    gr_test {Gate drift: replicas attach before any gate exists} {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 20
        $primary config set gate.feedback-timeout 600000
        $replica replicaof $primary_host $primary_port
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $replica
        wait_for_sync $subreplica
        waitForBgrewriteaof $replica
        assert_equal 0 [$primary dbsize]
    }

    gr_test {Gate drift: the disagreements of one block stop the gate on every node, at the count the specification gives} {
        # SPEC.md section 5. At ALPHA 0.2 a disagreement multiplies the two
        # likelihood ratios by 2 and by 3 (0.4 / 0.2 and 0.6 / 0.2: four
        # times the error rate would be more than halfway to 1), an
        # agreement by 0.75 and by 0.5. At ALPHA 0.05: by 2 and by 4, and by
        # 0.947 and by 0.842. The alarm is at 1,000 for their average.
        #
        #   fresh    only disagreements, from the first spot check on:
        #            (2^6 + 3^6) / 2 = 396.5, (2^7 + 3^7) / 2 = 1,157.5.
        #   strict   the same at ALPHA 0.05:
        #            (2^5 + 4^5) / 2 = 528, (2^6 + 4^6) / 2 = 2,080.
        #   after    5 spot checks agree first, in the same block, and there
        #            is no restart inside a block:
        #            (0.75^5 2^9 + 0.5^5 3^9) / 2 = 368.3, with a tenth
        #            disagreement 1,044.1.
        foreach {key alpha right alarm} {dd:fresh 0.2 0 7 dd:strict 0.05 0 6 dd:after 0.2 5 10} {
            assert_equal OK [$primary gcreate $key ALPHA $alpha DELTA 0.2 AUDIT 1 SCORE]
            gr_right_requests $primary $key [gr_right_answers $alpha 0.2 1 1]
            # The detector of this file gives the count worked out above.
            set detector [gr_detector $alpha]
            for {set n 0} {$n < $right} {incr n} {
                gr_detector_ask detector
            }
            for {set n 1} {$n <= $alarm} {incr n} {
                assert_equal [expr {$n < $alarm ? "quiet" : "worse"}] [gr_detector_count detector [gr_detector_ask detector] wrong] \
                    "disagreement $n after $right agreements at ALPHA $alpha, by the detector of this file"
            }

            for {set n 0} {$n < $right} {incr n} {
                lassign [$primary gpredict $key SCORE 0.9] action id
                assert_equal AUDIT $action
                assert_equal 1 [$primary glearn $key $id CORRECT 1]
            }
            for {set n 1} {$n < $alarm} {incr n} {
                lassign [$primary gpredict $key SCORE 0.9] action id
                assert_equal AUDIT $action "GPREDICT $key after [expr {$n - 1}] disagreements"
                assert_equal 1 [$primary glearn $key $id CORRECT 0]
            }
            gr_expect $primary $replicas $key [dict create state ACTIVE generation 1 revocations 0 \
                audit-samples [expr {$right + $alarm - 1}] audit-errors [expr {$alarm - 1}] pending 0] \
                "[expr {$alarm - 1}] disagreements after $right agreements at ALPHA $alpha"
            lassign [$primary gpredict $key SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [$primary glearn $key $id CORRECT 0]
            # It got worse: the gate starts over, with its next snapshot.
            gr_expect $primary $replicas $key {state CALIBRATING generation 0 candidate 2 revocations 1 audit-samples 0 audit-errors 0 pending 0} \
                "$alarm disagreements after $right agreements at ALPHA $alpha"
        }
        gr_phase $primary $replicas {dd:fresh dd:strict dd:after} "after the alarms"
    }

    gr_test {Gate drift: a replica that loads its AOF and one that reloads keep what open blocks, lost answers and comparisons need} {
        set story [gr_story_begin $primary st]
        gr_story_middle $primary $replicas $story
        set keys [gr_story_keys st]

        # Everything the replica holds came through the stream and is in its
        # AOF, operation by operation. Replayed, it must give the gates as
        # they are, in the middle of their blocks and comparisons.
        assert_equal OK [$replica debug loadaof]
        gr_story_middle $primary $replicas $story
        # A rewritten AOF holds each gate whole (as a command, then inside
        # an RDB preamble).
        foreach preamble {no yes} {
            $replica config set aof-use-rdb-preamble $preamble
            $replica bgrewriteaof
            waitForBgrewriteaof $replica
            assert_equal OK [$replica debug loadaof]
            gr_phase $primary $replicas $keys "after DEBUG LOADAOF of a rewritten AOF (preamble $preamble)"
        }
        # The sub-replica starts over from what it would save.
        assert_equal OK [$subreplica debug reload]
        gr_story_middle $primary $replicas $story

        # From the middle to the end, on gates each replica has loaded.
        gr_story_end $primary $replicas $story
    }

    gr_test {Gate drift: lost spot checks and then disagreements stop the gate, which keeps what it has learned} {
        # Sixteen lost spot checks in a fresh block are not enough to stop a
        # gate at ALPHA 0.2: (2^16 + 3^16) / 2 = 2.2 x 10^7. Spot checks that
        # disagree come on top: (2^19 + 3^19) / 2 = 5.8 x 10^8, and with the
        # fourth (2^20 + 3^20) / 2 = 1.7 x 10^9 reaches 10^9. Four
        # disagreements among 20 spot checks are nowhere near 1,000: the
        # gate is not known to be worse, it can no longer be checked. It
        # stops, keeps its model, and waits for an answer.
        set timeout 300
        $primary config set gate.feedback-timeout $timeout
        assert_equal OK [$primary gcreate du:score ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        assert_equal OK [$primary gcreate du:text ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 600000
        gr_right_requests $primary du:score [gr_right_answers 0.2 0.2 1 1]
        for {set n 0} {$n < 10} {incr n} {
            foreach label {spam ham} {
                $primary gadd du:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary du:text {spam ham}
        # The classifier answers for one of its labels so far: a request it
        # would answer is one it takes for that label, and a spot check of
        # that label's detector.
        set info [gr_info $primary du:text]
        assert_equal 1 [dict get $info generation]
        set proven {}
        foreach label {spam ham} threshold [dict get $info thresholds] {
            if {$threshold ne {}} {
                lappend proven $label
            }
        }
        assert_equal 1 [llength $proven] "labels du:text answers for"
        set mine [lindex $proven 0]
        set other [expr {$mine eq "spam" ? "ham" : "spam"}]

        gr_spot_checks $primary du:score 16
        for {set n 0} {$n < 16} {incr n} {
            lassign [$primary gpredict du:text TEXT [gr_text $mine $n]] action id
            assert_equal AUDIT $action "GPREDICT du:text, a text the answering snapshot knows"
        }
        set asked [clock milliseconds]
        gr_phase $primary $replicas {du:score du:text} "16 spot checks waiting"
        gr_wait_age $asked $timeout
        assert_equal OK [$primary gresume du:score]
        assert_equal OK [$primary gresume du:text]
        gr_expect $primary $replicas du:score {state ACTIVE generation 1 missing 16 revocations 0 pending 0} "16 lost spot checks"
        gr_expect $primary $replicas du:text {state ACTIVE generation 1 missing 16 revocations 0 pending 0} "16 lost spot checks"
        set learned [gr_field $primary du:text learned]

        for {set n 1} {$n <= 3} {incr n} {
            lassign [$primary gpredict du:score SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [$primary glearn du:score $id CORRECT 0]
            lassign [$primary gpredict du:text TEXT [gr_text $mine [expr {20 + $n}]]] action id
            assert_equal AUDIT $action
            assert_equal 1 [$primary glearn du:text $id LABEL $other]
        }
        gr_expect $primary $replicas du:score {state ACTIVE generation 1 audit-samples 3 audit-errors 3 revocations 0} \
            "3 disagreements after 16 lost spot checks"
        gr_expect $primary $replicas du:text [dict create state ACTIVE generation 1 audit-samples 3 audit-errors 3 revocations 0 \
            learned [expr {$learned + 3}]] "3 disagreements after 16 lost spot checks"

        lassign [$primary gpredict du:score SCORE 0.9] action id
        assert_equal 1 [$primary glearn du:score $id CORRECT 0]
        lassign [$primary gpredict du:text TEXT [gr_text $mine 24]] action id
        assert_equal 1 [$primary glearn du:text $id LABEL $other]
        # A gate that got worse tests its next snapshot at once, and a
        # classifier forgets its model. Not these two.
        gr_expect $primary $replicas du:score {state COLD generation 0 candidate 0 revocations 1 missing 16 pending 0} \
            "the 4th disagreement after 16 lost spot checks"
        gr_expect $primary $replicas du:text [dict create state COLD generation 0 candidate 0 revocations 1 missing 16 \
            learned [expr {$learned + 4}]] "the 4th disagreement after 16 lost spot checks"

        # An answer arrives: tested again, under numbers never used.
        lassign [$primary gpredict du:score SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn du:score $id CORRECT 1]
        lassign [$primary gpredict du:text TEXT [gr_text $mine 25]] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn du:text $id LABEL $mine]
        gr_expect $primary $replicas du:score {state CALIBRATING generation 0 candidate 2} "an answer for a gate that had stopped"
        # (Snapshot 2 of the classifier was being tested when it stopped.)
        gr_expect $primary $replicas du:text [dict create state CALIBRATING generation 0 candidate 3 \
            learned [expr {$learned + 5}]] "an answer for a gate that had stopped"
        gr_phase $primary $replicas {du:score du:text} "tested again"
    }

    gr_test {Gate drift: disagreeing answers that arrive late, in batches and out of order stop the gate when the specification says so, on every node} {
        # The dice decide which requests are spot checks (AUDIT 0.4), the
        # teacher which of them it disagrees with, when it says so and in
        # what company. The detector of this file follows every spot check
        # and says when the gate must stop: not one answer earlier, not one
        # later.
        assert_equal OK [$primary gcreate dx:rand ALPHA 0.2 DELTA 0.2 AUDIT 0.4 SCORE]
        set m [gr_exact_new dx:rand 0.2]
        gr_exact_prove $primary m
        set held {}
        foreach {phase wrong} {1 8 2 55 3 10 4 60 5 12 6 50} {
            gr_exact_workload $primary m held 300 $wrong
            gr_exact_check $primary m "phase $phase"
            gr_phase $primary $replicas dx:rand "answers late, in batches and out of order, phase $phase"
            switch $phase {
                2 {
                    assert_equal OK [$replica debug loadaof]
                    gr_phase $primary $replicas dx:rand "after DEBUG LOADAOF on the replica"
                }
                4 {
                    assert_equal OK [$subreplica debug reload]
                    gr_phase $primary $replicas dx:rand "after DEBUG RELOAD on the sub-replica"
                }
            }
        }
        # In the end the teacher disagrees with every spot check, and says
        # so at once: the gate stops for certain.
        if {[dict get $m generation] == 0} {
            gr_exact_workload $primary m held 1 0
        }
        set worse $::gr_exact(worse)
        for {set n 0} {$n < 1000 && [dict get $m generation] != 0} {incr n} {
            set id [gr_exact_ask $primary m 0.9]
            if {$id ne {}} {
                gr_exact_answer $primary m $id 0
            }
        }
        assert_equal [expr {$worse + 1}] $::gr_exact(worse) "times the gate stopped while every spot check disagreed"
        gr_exact_check $primary m "after the last alarm"
        foreach {id correct} $held {
            assert_equal 0 [$primary glearn dx:rand $id CORRECT $correct] "GLEARN dx:rand $id, for a request the gate has forgotten"
        }
        gr_phase $primary $replicas dx:rand "at the end"
        # The workload went through answers that came late, in transactions
        # and in scripts.
        foreach what {late multi script paused} {
            if {$::gr_exact($what) == 0} {
                fail "the workload never met '$what': [array get ::gr_exact]"
            }
        }
        assert_equal 0 $::gr_exact(unchecked)
        assert_equal 0 $::gr_exact(blind)
    }

    gr_test {Gate drift: answers that arrive after their proof has moved on are reported to the detector all the same} {
        # A quarter of the feedback timeout after a request, proof moves on
        # without its answer and counts it as a wrong guess for now. The
        # detector is not concerned: a spot check counts as right until its
        # answer says otherwise, whenever that is.
        set timeout 1200
        set grace 300
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            set key dt:late:$attempt
            $primary config set gate.feedback-timeout $timeout
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
            $primary config set gate.feedback-timeout 600000
            set m [gr_exact_new $key 0.2]
            set stalled 0
            set late 0
            set moved 0
            foreach {round wrong} {1 10 2 60 3 15 4 70} {
                if {[dict get $m generation] == 0} {
                    gr_exact_prove $primary m
                }
                # Requests. A third of the spot checks is answered at once,
                # the answers of the others are on their way.
                set t0 [clock milliseconds]
                set held {}
                for {set n 0} {$n < 60 && [dict get $m generation] != 0} {incr n} {
                    set id [gr_exact_ask $primary m [expr {[gr_rand 101] / 100.0}]]
                    if {$id eq {}} continue
                    set correct [expr {[gr_rand 100] >= $wrong}]
                    if {[gr_rand 100] < 35} {
                        gr_exact_answer $primary m $id $correct
                    } else {
                        lappend held $id $correct
                    }
                }
                set t1 [clock milliseconds]
                gr_phase $primary $replicas $key "round $round, answers on their way"
                gr_wait_age $t1 $grace
                if {![gr_in_time $t0 [expr {$timeout - 300}]]} {
                    set stalled 1
                    break
                }
                # Proof has moved on without the answers of requests that
                # had answered requests behind them: the gate has counted
                # more proof than the answers that came in order account for.
                assert_equal OK [$primary gresume $key]
                if {[dict get $m generation] != 0} {
                    set ahead [expr {[gr_field $primary $key calibration-samples] - [dict get $m samples]}]
                    assert {$ahead >= 0}
                    incr moved $ahead
                }
                # The answers arrive, in no order, a few at a time.
                set failed [catch {
                    while {[llength $held] > 0} {
                        set answers {}
                        for {set n [expr {1 + [gr_rand 4]}]} {$n > 0 && [llength $held] > 0} {incr n -1} {
                            set i [expr {2 * [gr_rand [expr {[llength $held] / 2}]]}]
                            lappend answers {*}[lrange $held $i [expr {$i + 1}]]
                            set held [lreplace $held $i [expr {$i + 1}]]
                        }
                        if {[dict get $m generation] == 0} {
                            # The gate has stopped. It still takes the
                            # answer of a request its proof had moved on
                            # without, and has forgotten the others.
                            foreach {id correct} $answers {
                                assert {[$primary glearn $key $id CORRECT $correct] in {0 1}}
                            }
                        } else {
                            incr late [expr {[llength $answers] / 2}]
                            gr_exact_batch $primary m $answers multi 1
                        }
                    }
                    gr_exact_check $primary m "round $round, every answer has arrived"
                } err]
                if {$failed} {
                    if {![gr_in_time $t0 [expr {$timeout - 20}]]} {
                        # An answer may have come after its request was
                        # given up: this attempt cannot tell.
                        set stalled 1
                        break
                    }
                    error $err
                }
                gr_phase $primary $replicas $key "round $round, every answer has arrived"
            }
            if {!$stalled} {
                set done 1
            }
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
        assert {$late > 10}
        assert {$moved > 0}
        assert_equal 0 $::gr_exact(unchecked)
        assert_equal 0 $::gr_exact(blind)
    }

    gr_test {Gate drift: a gate that loses exactly the spot checks its teacher disagrees with stops on every node, and keeps what it has learned} {
        # A teacher, or a client, that fails on the hard cases loses the
        # answers that matter. No disagreement ever arrives; the agreeing
        # answers arrive at once, so this is no outage either. The gate
        # must stop all the same once it can no longer rule out that it got
        # worse: when the lost answers of a block, counted as disagreements,
        # reach 10^9.
        set timeout 400
        set done 0
        for {set attempt 1} {$attempt <= 3 && !$done} {incr attempt} {
            set key dl:score:$attempt
            set tkey dl:text:$attempt
            $primary config set gate.feedback-timeout $timeout
            assert_equal OK [$primary gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
            assert_equal OK [$primary gcreate $tkey ALPHA 0.2 DELTA 0.2 AUDIT 0.5 CLASSES 2 spam ham]
            $primary config set gate.feedback-timeout 600000
            set m [gr_exact_new $key 0.2]
            gr_exact_prove $primary m
            for {set n 0} {$n < 10} {incr n} {
                foreach label {spam ham} {
                    $primary gadd $tkey TEXT [gr_text $label $n] LABEL $label
                }
            }
            gr_text_until_active $primary $tkey {spam ham}
            set before [gr_info $primary $tkey]
            set learned [dict get $before learned]
            set stalled 0
            set stops 0
            set tstops 0
            for {set round 1} {$round <= 12 && !$stalled && ($stops == 0 || $tstops == 0)} {incr round} {
                # The teacher disagrees with a third of the spot checks at
                # first, then with most of them.
                set wrong [expr {$round <= 2 ? 30 : 85}]
                if {[dict get $m generation] == 0} {
                    gr_exact_prove $primary m
                }
                set t0 [clock milliseconds]
                for {set n 0} {$n < 60} {incr n} {
                    set id [gr_exact_ask $primary m [expr {[gr_rand 101] / 100.0}]]
                    if {$id ne {} && [gr_rand 100] >= $wrong} {
                        gr_exact_answer $primary m $id 1
                    }
                    # The classifier: texts it takes for 'spam', and right.
                    lassign [$primary gpredict $tkey TEXT [gr_text spam [gr_rand 1000]]] action id
                    if {$action eq "AUDIT" && [gr_rand 100] < $wrong} continue
                    if {$action ne "ACCEPT" && $id ne {}} {
                        assert_equal 1 [$primary glearn $tkey $id LABEL spam]
                    }
                }
                if {![gr_in_time $t0 [expr {$timeout - 50}]]} {
                    set stalled 1
                    break
                }
                set t1 [clock milliseconds]
                gr_phase $primary $replicas [list $key $tkey] "round $round, the spot checks the teacher disagrees with wait"
                if {$round % 2 == 0} {
                    # A quarter of the feedback timeout later a write finds
                    # that proof has to move on without their answers. They
                    # are given up later as any other request would be.
                    gr_wait_age $t1 [expr {$timeout / 4}]
                    assert_equal OK [$primary gresume $key]
                    assert_equal OK [$primary gresume $tkey]
                    gr_phase $primary $replicas [list $key $tkey] "round $round, proof has moved on without the spot checks the teacher disagrees with"
                }
                # No answer comes for them. Once the feedback timeout is
                # over, the next write gives them up.
                gr_wait_age $t1 $timeout
                set verdict [gr_exact_give_up $primary m]
                gr_exact_check $primary m "round $round, after the feedback timeout"
                assert_equal OK [$primary gresume $tkey]
                gr_phase $primary $replicas [list $key $tkey] "round $round, the spot checks the teacher disagrees with were given up"
                # (Stopped by 32 requests in a row without an answer only
                # if the dice made nearly every request of a round a spot
                # check the teacher disagrees with: then once more.)
                if {$verdict eq "unchecked"} {
                    incr stops
                }
                set info [gr_info $primary $tkey]
                # No disagreement was ever reported: the model is kept.
                assert {[dict get $info learned] >= $learned}
                set learned [dict get $info learned]
                if {[dict get $info revocations] > [dict get $before revocations]} {
                    incr tstops
                    assert_equal COLD [dict get $info state]
                    assert_equal 0 [dict get $info candidate]
                    assert {[gr_not_answering $info]}
                    # At ALPHA 0.2 it takes 20 lost spot checks at the least.
                    assert {[dict get $info missing] - [dict get $before missing] >= 20}
                    # Answers arrive again, and the classifier is tested again.
                    set numbered [dict get $before generation]
                    gr_text_until_active $primary $tkey {spam ham}
                    set before [gr_info $primary $tkey]
                    assert {[dict get $before generation] > $numbered}
                    assert {[dict get $before learned] > $learned}
                    set learned [dict get $before learned]
                }
            }
            if {$stalled} continue
            if {$stops == 0 || $tstops == 0} {
                fail "after 12 rounds of lost spot checks a gate was never stopped by its detector: [gr_describe $primary $key] / [gr_describe $primary $tkey]"
            }
            gr_phase $primary $replicas [list $key $tkey] "at the end"
            set done 1
        }
        if {!$done} {
            fail "the test machine stalled three times in a row: this test could not tell on which side of a deadline its commands fell"
        }
        assert {$::gr_exact(unchecked) > 0}
    }

    gr_test {Gate drift: a newer snapshot compared with the answering one for a long time is dropped, or takes over, on every node at the same request} {
        # The comparisons of the story, with dice: three requests in ten
        # are spot checks (AUDIT 0.3), and only those are proof for the
        # newer snapshot. An AUDIT reply is such a request. An ABSTAIN reply
        # may be one: the dice are not told.
        foreach name {keep swap} {
            gr_takeover_begin $primary tr:$name 0.3
        }
        set keys {tr:keep tr:swap}
        gr_phase $primary $replicas $keys "the second snapshot is compared with the first"

        # tr:swap: requests the answering snapshot answers, all of them. The
        # newer snapshot cannot answer more, however long this goes on.
        set audits 0
        for {set n 0} {$n < 4000 && $audits < 110} {incr n} {
            lassign [lindex $::gr_vocabularies [expr {$n % 2}]] words label
            set action [gr_text_request $primary tr:swap [gr_text $words $n] $label]
            if {$action eq "AUDIT"} {
                incr audits
            } elseif {$action eq "ABSTAIN" && $n > 3} {
                fail "request $n in a vocabulary the answering snapshot of tr:swap knows was left to the teacher: [gr_describe $primary tr:swap]"
            }
        }
        gr_expect $primary $replicas tr:swap {state ACTIVE generation 1 candidate 2 revocations 0} "$n requests the answering snapshot answers"
        assert {[lindex [gr_field $primary tr:swap thresholds] 0] > 0.5}
        # Requests in the other vocabularies: the answering snapshot leaves
        # them to the teacher, the newer one answers them. It takes over
        # with the first of them that is a spot check.
        for {set n 0} {$n < 400 && [gr_field $primary tr:swap generation] == 1} {incr n} {
            lassign [lindex $::gr_vocabularies [expr {2 + $n % 2}]] words label
            set action [gr_text_request $primary tr:swap [gr_text $words $n] $label]
            if {$n == 0} {
                assert_equal ABSTAIN $action "a request in a vocabulary the answering snapshot of tr:swap does not know"
            }
        }
        gr_expect $primary $replicas tr:swap {state ACTIVE generation 2 revocations 0} "requests only the newer snapshot answers"

        # tr:keep: the same, to the end. The newer snapshot is dropped after
        # 880 pieces of proof (SPEC.md section 5: 10 x 44 x 2 labels).
        set budget [expr {[gr_right_answers 0.2 0.2 2 2] * 10 * 2}]
        set least 0
        set most 0
        set checked 0
        for {set n 0} {$n < 20000 && $least < $budget} {incr n} {
            lassign [lindex $::gr_vocabularies [expr {$n % 2}]] words label
            set action [gr_text_request $primary tr:keep [gr_text $words $n] $label]
            if {$action eq "AUDIT"} {
                incr least
                incr most
            } elseif {$action eq "ABSTAIN"} {
                if {$n > 3} {
                    fail "request $n in a vocabulary the answering snapshot of tr:keep knows was left to the teacher: [gr_describe $primary tr:keep]"
                }
                incr most
            }
            if {$most == $budget - 1 && !$checked} {
                # One piece of proof short, at the most.
                gr_expect $primary $replicas tr:keep {state ACTIVE generation 1 candidate 2 revocations 0} \
                    "at most [expr {$budget - 1}] pieces of proof for a snapshot that cannot answer more"
                set checked 1
            }
            if {$n % 800 == 799} {
                gr_phase $primary $replicas $keys "a long comparison, after [expr {$n + 1}] requests"
            }
        }
        assert {$checked}
        gr_expect $primary $replicas tr:keep {state ACTIVE generation 1 candidate 3 revocations 0} \
            "at least $budget pieces of proof for a snapshot that cannot answer more"
        gr_phase $primary $replicas $keys "at the end of the comparisons"
    }

    gr_test {Gate drift: a replica promoted in the middle of open blocks and comparisons starts its gates over} {
        set story [gr_story_begin $primary fo]
        gr_story_middle $primary $replicas $story
        set keys [gr_story_keys fo]
        set numbered [dict create]
        foreach key $keys {
            set info [gr_info $replica $key]
            dict set numbered $key [expr {max([dict get $info generation], [dict get $info candidate])}]
        }

        assert_equal OK [$replica replicaof no one]
        assert_equal master [lindex [$replica role] 0]
        gr_watch -2
        # Nothing shows that the new primary was sent everything: at their
        # first write its gates drop their proof, the blocks of their
        # detectors and their comparisons with it, and GINFO says so now.
        foreach key $keys {
            set info [gr_info $replica $key]
            assert {[gr_not_answering $info]}
            assert_equal 0 [dict get $info pending]
        }
        foreach id [dict get $story worse] {
            assert_equal 0 [$replica glearn fo:worse $id CORRECT 0] "GLEARN fo:worse $id, a spot check of the old primary"
        }
        foreach id [dict get $story block] {
            assert_equal 0 [$replica glearn fo:block $id CORRECT 0] "GLEARN fo:block $id, a spot check of the old primary"
        }
        foreach key {fo:lost fo:run fo:mark fo:clear} {
            assert_equal OK [$replica gresume $key]
        }
        foreach key {fo:keep fo:swap fo:defer} {
            $replica gadd $key TEXT [gr_text other 1] LABEL [lindex [gr_field $replica $key classes] 0]
        }
        gr_phase $replica $subreplica $keys "behind the promoted replica"
        foreach key $keys {
            set info [gr_info $subreplica $key]
            assert {[gr_not_answering $info]}
            assert_equal 0 [dict get $info pending] "pending of $key"
            # Every gate is tested again at once, under a number that was
            # never used: the detector of that snapshot is new.
            assert_equal CALIBRATING [dict get $info state] "state of $key"
            assert_equal [expr {[dict get $numbered $key] + 1}] [dict get $info candidate] "candidate of $key"
        }

        # fo:worse had met 3 disagreements, fo:lost had lost 12 spot checks.
        # None of that counts against the snapshots that answer now: it
        # takes 7 disagreements again, and 12 lost spot checks stop nothing.
        set again [gr_right_answers 0.2 0.2 1 2]
        gr_right_requests $replica fo:worse $again
        gr_right_requests $replica fo:lost $again
        gr_spot_checks $replica fo:lost 12
        set asked [clock milliseconds]
        for {set n 1} {$n <= 6} {incr n} {
            lassign [$replica gpredict fo:worse SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [$replica glearn fo:worse $id CORRECT 0]
        }
        gr_expect $replica $subreplica fo:worse {state ACTIVE generation 2 audit-samples 6 audit-errors 6 revocations 0} \
            "6 disagreements on the promoted replica"
        lassign [$replica gpredict fo:worse SCORE 0.9] action id
        assert_equal 1 [$replica glearn fo:worse $id CORRECT 0]
        gr_expect $replica $subreplica fo:worse {state CALIBRATING generation 0 candidate 3 revocations 1} \
            "the 7th disagreement on the promoted replica"

        # The old primary gives up and follows.
        assert_equal OK [$primary replicaof $replica_host $replica_port]
        wait_for_sync $primary
        gr_watch 0 -2
        set followers [list $primary $subreplica]
        gr_phase $replica $followers $keys "the old primary as a replica"

        gr_wait_age $asked 300
        assert_equal OK [$replica gresume fo:lost]
        gr_expect $replica $followers fo:lost {state ACTIVE generation 2 missing 24 revocations 0 pending 0} \
            "12 lost spot checks on the promoted replica"

        # A story from the start to the end on the new primary, with the old
        # one and the sub-replica behind it.
        set story [gr_story_begin $replica f2]
        gr_story_middle $replica $followers $story
        gr_story_end $replica $followers $story
        gr_phase $replica $followers [concat $keys [gr_story_keys f2]] "at the end"
        gr_assert_same_dataset $replica $followers "at the end"
    }

    gr_test {Gate drift: FAILOVER back in the middle of open blocks and comparisons} {
        set followers [list $primary $subreplica]
        set story [gr_story_begin $replica f3]
        gr_story_middle $replica $followers $story
        set keys [gr_story_keys f3]
        # The workload with a teacher that disagrees and answers when it
        # likes, on the primary of the moment: answers are out.
        assert_equal OK [$replica gcreate f3:rand ALPHA 0.2 DELTA 0.2 AUDIT 0.4 SCORE]
        set m [gr_exact_new f3:rand 0.2]
        gr_exact_prove $replica m
        set held {}
        gr_exact_workload $replica m held 300 30
        gr_phase $replica $followers f3:rand "before FAILOVER"
        foreach node [concat [list $replica] $followers] {
            gr_exact_check $node m "before FAILOVER"
        }

        # The primary of the moment hands over to the server that was the
        # primary at first, and becomes its replica.
        assert_equal OK [$replica failover to $primary_host $primary_port]
        wait_for_condition 100 100 {
            [status $replica master_failover_state] eq "no-failover" &&
            [lindex [$primary role] 0] eq "master" &&
            [status $replica master_link_status] eq "up" &&
            [status $subreplica master_link_status] eq "up"
        } else {
            fail "FAILOVER to the first primary did not finish"
        }
        gr_watch -1 -2
        # It was sent these gates as a replica: at their first write they
        # drop their proof, the blocks of their detectors and their
        # comparisons.
        foreach key $keys {
            set info [gr_info $primary $key]
            assert {[gr_not_answering $info]}
            assert_equal 0 [dict get $info pending]
        }
        foreach id [dict get $story worse] {
            assert_equal 0 [$primary glearn f3:worse $id CORRECT 0] "GLEARN f3:worse $id, a spot check from before the FAILOVER"
        }
        foreach key {f3:block f3:lost f3:run f3:mark f3:clear} {
            assert_equal OK [$primary gresume $key]
        }
        foreach key {f3:keep f3:swap f3:defer} {
            $primary gadd $key TEXT [gr_text other 1] LABEL [lindex [gr_field $primary $key classes] 0]
        }
        gr_phase $primary $replicas $keys "after FAILOVER"
        foreach node $nodes {
            foreach key $keys {
                set info [gr_info $node $key]
                assert {[gr_not_answering $info]}
                assert_equal CALIBRATING [dict get $info state] "state of $key"
                assert_equal 0 [dict get $info pending] "pending of $key"
            }
        }
        # The snapshot that is tested now starts from a new detector: the 3
        # disagreements f3:worse had met do not count against it.
        gr_right_requests $primary f3:worse [gr_right_answers 0.2 0.2 1 2]
        for {set n 1} {$n <= 6} {incr n} {
            lassign [$primary gpredict f3:worse SCORE 0.9] action id
            assert_equal AUDIT $action
            assert_equal 1 [$primary glearn f3:worse $id CORRECT 0]
        }
        gr_expect $primary $replicas f3:worse {state ACTIVE generation 2 audit-samples 6 audit-errors 6 revocations 0} \
            "6 disagreements after FAILOVER"
        lassign [$primary gpredict f3:worse SCORE 0.9] action id
        assert_equal 1 [$primary glearn f3:worse $id CORRECT 0]
        gr_expect $primary $replicas f3:worse {state CALIBRATING generation 0 candidate 3 revocations 1} \
            "the 7th disagreement after FAILOVER"

        # The workload goes on, on the returning primary. Every answer that
        # was out is for a request of the other primary: none is used.
        assert_equal OK [$primary gresume f3:rand]
        dict set m paused 0
        gr_exact_recover m
        foreach {id correct} $held {
            assert_equal 0 [$primary glearn f3:rand $id CORRECT $correct] "GLEARN f3:rand $id, a spot check from before the FAILOVER"
        }
        set held {}
        gr_exact_prove $primary m
        foreach {phase wrong} {1 55 2 10} {
            gr_exact_workload $primary m held 250 $wrong
            gr_phase $primary $replicas f3:rand "workload after FAILOVER, phase $phase"
            foreach node $nodes {
                gr_exact_check $node m "workload after FAILOVER, phase $phase"
            }
        }
        gr_phase $primary $replicas $keys "at the end"
        gr_assert_same_dataset $primary $replicas "at the end"
    }
}
}
}

# -------------- replicas attached in the middle of blocks and of comparisons
#
# The story and the workload again, on a primary that has no replica yet.
# Then a replica is attached, and a replica to that: they get the gates
# whole, through a full synchronization, with spot checks that wait for
# their answers, blocks of the detector that are open, lost answers that
# were counted and a newer snapshot half way through its comparison. The
# replica is sent a file and loads it; the sub-replica is sent the dataset
# straight from the memory of the replica and loads it from the socket.

start_server [list tags {"modules external:skip gate:drift"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    gr_watch

    gr_test {Gate drift: a primary that has no replica yet, in the middle of open blocks, lost answers and comparisons} {
        $primary config set gate.test-seed 0
        $primary config set gate.snapshot-examples 20
        $primary config set gate.feedback-timeout 600000
        set story [gr_story_begin $primary sy]

        # A teacher that disagrees now and then and answers when it likes.
        assert_equal OK [$primary gcreate sy:rand ALPHA 0.2 DELTA 0.2 AUDIT 0.4 SCORE]
        set m [gr_exact_new sy:rand 0.2]
        gr_exact_prove $primary m
        set held {}
        gr_exact_workload $primary m held 300 10
        gr_exact_workload $primary m held 200 30
        if {[dict get $m generation] == 0} {
            gr_exact_workload $primary m held 1 0
        }
        # Spot checks whose answers are still out when the replicas are
        # attached, a dozen at the least. Two in five will disagree.
        for {set n 0} {$n < 2000 && [llength $held] < 24} {incr n} {
            set id [gr_exact_ask $primary m [expr {[gr_rand 101] / 100.0}]]
            if {$id ne {}} {
                lappend held $id [expr {[gr_rand 100] >= 40}]
            }
        }
        assert {[llength $held] >= 24}
        gr_exact_check $primary m "before the replicas are attached"
    }

    gr_test {Gate drift: replicas attached in the middle take open blocks, lost answers and comparisons whole} {
        set everything [concat [gr_story_keys sy] sy:rand]
        set digests [$primary debug digest-value {*}$everything]
        $primary config set repl-diskless-sync no
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        $replica config set repl-diskless-sync yes
        $subreplica config set repl-diskless-load swapdb
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $subreplica
        gr_watch -1 -2
        assert_equal 1 [status $primary sync_full]
        assert_equal 1 [status $replica sync_full]
        # Sending the gates did not change them.
        assert_equal $digests [$primary debug digest-value {*}$everything]
        gr_story_middle $primary $replicas $story
        gr_phase $primary $replicas sy:rand "after the full synchronization"
        foreach node $nodes {
            gr_exact_check $node m "after the full synchronization"
        }

        # From the middle to the end, on gates the replicas were sent whole.
        gr_story_end $primary $replicas $story

        # The answers that were out when the replicas were attached are for
        # spot checks they received with the gate, each under its number.
        # They arrive in no order; the gate stops when the detector of this
        # file says so, on every node.
        set order {}
        foreach {id correct} $held {
            lappend order [list $id $correct]
        }
        set held {}
        foreach answer [gr_shuffle $order] {
            gr_exact_answer $primary m {*}$answer
        }
        gr_sync $primary $replicas
        foreach node $nodes {
            gr_exact_check $node m "the answers that were out during the full synchronization"
        }
        foreach {phase wrong} {1 55 2 10 3 60} {
            gr_exact_workload $primary m held 250 $wrong
            gr_phase $primary $replicas sy:rand "workload after the full synchronization, phase $phase"
            foreach node $nodes {
                gr_exact_check $node m "workload after the full synchronization, phase $phase"
            }
        }
        gr_phase $primary $replicas $everything "at the end"
        gr_assert_same_dataset $primary $replicas "at the end"
    }
}
}
}

# ------------------------------ operations that do not fit, down the chain
#
# A replica applies what its primary sends. An operation that does not fit
# the gate it is sent to must not stop replication: it is skipped, counted
# (gate_apply_mismatches) and passed on as it came, so that the replicas
# behind see what this one saw (gate.c; CHANGES.md, sections 8 and 9).
#
# No primary sends such operations. A connection marked as internal (DEBUG
# MARK-INTERNAL-CLIENT) can: here it sends them to the head of a chain, as
# the stream would bring them to a replica.
#
#   _GATE.APPLY key PREDICT now draw has-serving label score has-candidate
#                           label score request-number
#   _GATE.APPLY key CREATE now lineage epoch snapshot-examples timeout <GCREATE arguments>
#   _GATE.APPLY key TICK now
#
# (the draw and the scores travel as the 16 hex digits of a double)

# The 16 hex digits a double travels as.
proc gr_bits {value} {
    binary scan [binary format Q $value] H16 hex
    return $hex
}

# What every node must show after an operation: 'counted' more operations
# that did not fit than before ('expectedvar' counts them all), no error
# sent to a primary, and the gates as the head of the chain holds them.
proc gr_misfit_check {primary replicas gates expectedvar counted what} {
    upvar 1 $expectedvar expected
    incr expected $counted
    gr_sync $primary $replicas
    set n 0
    foreach node [concat [list $primary] $replicas] {
        assert_equal $expected [gr_mismatches $node] "gate_apply_mismatches on node #$n after $what"
        assert_equal PONG [$node ping]
        assert_equal 0 [status $node unexpected_error_replies]
        incr n
    }
    gr_assert_same_digests $primary $replicas $gates $what
}

start_server [list tags {"modules external:skip gate:drift"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]
    set subreplica [srv -2 client]
    set replicas [list $replica $subreplica]
    set nodes [list $primary $replica $subreplica]
    # These servers are meant to meet operations that do not fit.
    gr_watch

    gr_test {Gate replication: an operation that does not fit is skipped and counted by every node down the chain, and the gates stay equal} {
        $primary config set gate.test-seed 0
        $primary config set gate.feedback-timeout 600000
        $replica replicaof $primary_host $primary_port
        $subreplica replicaof $replica_host $replica_port
        wait_for_sync $replica
        wait_for_sync $subreplica
        set internal [redis_client]
        $internal debug mark-internal-client

        # A gate that does not answer yet, with a request that waits, and a
        # gate that answers.
        assert_equal OK [$primary gcreate fx:calib ALPHA 0.2 DELTA 0.2 SCORE]
        gr_right_requests $primary fx:calib 3
        lassign [$primary gpredict fx:calib SCORE 0.9] action waiting
        assert_equal 4 [gr_id_number $waiting]
        assert_equal OK [$primary gcreate fx:active ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        set need [gr_right_answers 0.2 0.2 1 1]
        gr_right_requests $primary fx:active $need
        assert_equal OK [$primary gcreate fx:last ALPHA 0.2 DELTA 0.2 SCORE]
        set gates {fx:calib fx:active fx:last}
        gr_assert_same $primary $replicas $gates "before the operations that do not fit"
        set digests [$primary debug digest-value {*}$gates]
        set expected 0
        foreach node $nodes {
            assert_equal 0 [gr_mismatches $node]
        }
        set now [clock milliseconds]
        set zero [gr_bits 0.0]
        set sure [gr_bits 0.9]
        set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
        set last_number [expr {1 << 62}]
        set seen [dict create]
        foreach node $replicas {
            dict set seen $node [gr_info $node fx:calib]
        }

        # A request number at the end of the range (gate_defs.h: no request
        # number goes that far): refused. The gate is as it was, to the last
        # counter.
        assert_equal OK [$internal _gate.apply fx:calib PREDICT $now [gr_bits 0.5] 0 -1 $zero 1 -1 $sure $last_number]
        gr_misfit_check $primary $replicas $gates expected 1 "a request number at the end of the range"
        assert_equal $digests [$primary debug digest-value {*}$gates]
        foreach node $replicas {
            assert_equal [dict get $seen $node] [gr_info $node fx:calib] "GINFO fx:calib on a replica, before and after a request number at the end of the range"
        }
        # A number further on than the next one is the number the primary
        # gave the request: it is recorded under it.
        assert_equal OK [$internal _gate.apply fx:calib PREDICT $now [gr_bits 0.5] 0 -1 $zero 1 -1 $sure 1000]
        gr_misfit_check $primary $replicas $gates expected 0 "a request number further on"
        foreach node $replicas {
            assert_equal 2 [gr_field $node fx:calib pending]
        }
        # So is the last number there is. After it the gate has none left
        # and takes no request, on any node (SPEC.md, "Full").
        assert_equal OK [$internal _gate.apply fx:last PREDICT $now [gr_bits 0.5] 0 -1 $zero 1 -1 $sure [expr {$last_number - 1}]]
        gr_misfit_check $primary $replicas $gates expected 0 "the last request number"
        foreach node $replicas {
            assert_equal 1 [gr_field $node fx:last pending]
        }
        assert_equal {ABSTAIN {}} [$primary gpredict fx:last SCORE 0.9]
        assert_equal {ABSTAIN {}} [$primary gpredict fx:last SCORE 0.9]
        gr_misfit_check $primary $replicas $gates expected 0 "requests to a gate that has no request number left"
        gr_assert_same $primary $replicas fx:last "a gate that has no request number left"

        # A request the gate answers by itself is not one a primary records:
        # the operation does not fit, and the number it came with is not
        # used up. The next request of the gate gets the number that was
        # due. (This server no longer trusts the gate: it starts it over.)
        assert_equal OK [$internal _gate.apply fx:active PREDICT $now $sure 1 -1 $sure 0 -1 $zero 1000]
        gr_misfit_check $primary $replicas $gates expected 1 "a recorded request that the gate would have answered"
        lassign [$primary gpredict fx:active SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal [expr {$need + 1}] [gr_id_number $id] "the number of the next request after an operation that recorded nothing"
        assert_equal 1 [$primary glearn fx:active $id CORRECT 1]
        gr_misfit_check $primary $replicas $gates expected 0 "the next request"
        gr_assert_same $primary $replicas fx:active "after the next request"

        # A gate with a feedback timeout or a snapshot step outside their
        # ranges is not created, on any node.
        foreach {snapshot timeout} [list 20 0  20 [expr {7 * 86400 * 1000 + 1}]  0 60000] {
            assert_equal OK [$internal _gate.apply fx:new CREATE $now 00000000000000aa 00000000000000bb $snapshot $timeout ALPHA 0.2 DELTA 0.2 SCORE]
            gr_misfit_check $primary $replicas $gates expected 1 "CREATE with snapshot step $snapshot and feedback timeout $timeout"
            foreach node $nodes {
                assert_equal 0 [$node exists fx:new]
            }
        }
        # With both at the end of their ranges it is.
        assert_equal OK [$internal _gate.apply fx:new CREATE $now 00000000000000aa 00000000000000bb 1000000000 [expr {7 * 86400 * 1000}] ALPHA 0.2 DELTA 0.2 SCORE]
        lappend gates fx:new
        gr_misfit_check $primary $replicas $gates expected 0 "CREATE at the end of the ranges"
        foreach node $nodes {
            assert_equal score [gr_field $node fx:new kind]
        }

        # A clock outside the range a gate works in is refused ...
        foreach clock [list -1 [expr {$last_clock + 1}] 9223372036854775807] {
            assert_equal OK [$internal _gate.apply fx:calib TICK $clock]
            gr_misfit_check $primary $replicas $gates expected 1 "an operation at the clock $clock"
        }
        # ... and the last value of the range is a clock like any other:
        # every request that waited has waited long enough.
        assert_equal OK [$internal _gate.apply fx:calib TICK $last_clock]
        gr_misfit_check $primary $replicas $gates expected 0 "an operation at the last clock"
        foreach node $replicas {
            assert_equal 0 [gr_field $node fx:calib pending]
            assert_equal 2 [gr_field $node fx:calib missing]
        }
        # The gate lives on at that clock, on every node.
        lassign [$primary gpredict fx:calib SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn fx:calib $id CORRECT 1]
        gr_misfit_check $primary $replicas $gates expected 0 "a request after the last clock"
        # (The head of the chain trusts a gate again once it has written to
        # it itself.)
        assert_equal OK [$primary gresume fx:new]
        gr_assert_same $primary $replicas $gates "at the end"

        $internal close
        assert_equal 8 $expected
        foreach level {0 -1 -2} {
            assert_equal $expected [count_message_lines [srv $level stdout] "did not fit its gate"] "operations the server at level $level logged as not fitting"
        }
        gr_assert_same_dataset $primary $replicas "at the end"
    }
}
}
}

# ------------------------------------- a gate at the end of what it can count
#
# The clock of a gate and its counters have a range (gate_defs.h: the clock
# ends at INT64_MAX less the longest feedback timeout, a counter, a request
# number or a snapshot number at 2^62), and a gate beyond it is not loaded:
# not from a dump, not from a file, not by a replica. A dump can put a gate
# exactly at the end of a range, or a step before it. Whatever a server
# takes with RESTORE it must be able to keep (CHANGES.md 9d): a gate that
# still works one request later, that its replicas hold as it does, and that
# can be saved and loaded again. Refusing such a dump is as good.
#
# (These servers come last: a replica that could not follow stays behind.)

start_server [list tags {"modules external:skip gate:edge"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    gr_watch -1
    # The last clock a gate may have, and the last number a counter may
    # reach (gate_defs.h).
    set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
    set last_number [expr {1 << 62}]

    gr_test {Gate replication: a gate restored at the end of the range of its clock, of its request numbers or of its snapshot numbers is refused, or one a replica can follow and take whole} {
        # The dumps are changed by hand, so they must not be compressed, and
        # their checksums are no longer right.
        $primary config set rdbcompression no
        foreach node [list $primary $replica] {
            $node debug set-skip-checksum-validation 1
        }
        $primary config set gate.test-seed 0
        $primary config set gate.feedback-timeout 600000
        $replica replicaof $primary_host $primary_port
        wait_for_sync $replica
        assert_equal OK [$primary gcreate edge:new ALPHA 0.2 DELTA 0.2 SCORE]
        lassign [gr_fresh_dump $primary edge:new] dump clock_at clock requests_at snapshots_at
        # Gates at the end of a range and a step before it: where the number
        # stands in the dump, and the number.
        set edges [dict create \
            edge:clock [list $clock_at $last_clock] \
            edge:clock:near [list $clock_at [expr {$last_clock - 3}]] \
            edge:requests [list $requests_at $last_number] \
            edge:requests:near [list $requests_at [expr {$last_number - 2}]] \
            edge:snapshots [list $snapshots_at $last_number] \
            edge:snapshots:near [list $snapshots_at [expr {$last_number - 1}]]]
        set taken {}
        dict for {key edge} $edges {
            lassign $edge at number
            set forged [string replace $dump $at [expr {$at + 7}] [binary format w $number]]
            if {[catch {$primary restore $key 0 $forged} reply]} {
                # Refused: nothing to keep.
                assert_match {*Bad data format*} $reply
                assert_equal 0 [$primary exists $key]
            } else {
                lappend taken $key
            }
        }
        if {[llength $taken] == 0} return
        gr_assert_same_digests $primary $replica $taken "restored"

        set problems {}
        set waiting [dict create]
        foreach key $taken {
            switch $key {
                edge:clock - edge:clock:near {
                    # A few requests and their answers, some ms apart. The
                    # clock has nowhere to go, or will have none in a
                    # moment; the gate works all the same.
                    for {set n 0} {$n < 3} {incr n} {
                        lassign [$primary gpredict $key SCORE 0.9] action id
                        assert_equal ABSTAIN $action "GPREDICT $key"
                        after 5
                        assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key $id"
                    }
                    # One more, whose answer is still out when the gate is
                    # saved and when it is sent.
                    lassign [$primary gpredict $key SCORE 0.9] action id
                    assert_equal ABSTAIN $action "GPREDICT $key"
                    dict set waiting $key $id
                    set expected {state CALIBRATING candidate 2 calibration-samples 3 pending 1}
                }
                edge:requests {
                    # No request number is left: the gate takes no request
                    # (SPEC.md, "Full": ABSTAIN without an id).
                    for {set n 0} {$n < 3} {incr n} {
                        assert_equal {ABSTAIN {}} [$primary gpredict $key SCORE 0.9] "GPREDICT $key"
                    }
                    set expected {state CALIBRATING candidate 2 calibration-samples 0 pending 0}
                }
                edge:requests:near {
                    # Two numbers are left, and are handed out.
                    set ids {}
                    foreach number [list [expr {$last_number - 2}] [expr {$last_number - 1}]] {
                        lassign [$primary gpredict $key SCORE 0.9] action id
                        assert_equal ABSTAIN $action "GPREDICT $key"
                        assert_equal $number [gr_id_number $id] "the number of a request of $key"
                        lappend ids $id
                    }
                    assert_equal {ABSTAIN {}} [$primary gpredict $key SCORE 0.9] "GPREDICT $key, no request number left"
                    # While the two requests wait for their answers the
                    # gate is, as at any time, what the primary would save
                    # and what a replica would be sent whole.
                    gr_sync $primary $replica
                    assert_equal [$primary debug digest-value $key] [$replica debug digest-value $key] \
                        "DEBUG DIGEST-VALUE $key while the requests with its last numbers wait, on the primary (first) and on the replica (second)"
                    if {[catch {$primary restore $key:waiting 0 [$primary dump $key]} reply]} {
                        lappend problems "$key: while the requests with its last two numbers wait for their answers,\
                            a DUMP of the gate is refused by RESTORE ($reply): the gate cannot be loaded from a file or by a replica"
                    }
                    foreach id $ids {
                        assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key $id"
                    }
                    set expected {state CALIBRATING candidate 2 calibration-samples 2 pending 0}
                }
                edge:snapshots {
                    # No snapshot number is left: nothing is tested, so no
                    # request is proof for anything.
                    assert_equal {ABSTAIN {}} [$primary gpredict $key SCORE 0.9] "GPREDICT $key"
                    set expected {state COLD generation 0 candidate 0 pending 0}
                }
                edge:snapshots:near {
                    # One snapshot number is left, the last there is. That
                    # snapshot is tested and answers after the right answers
                    # SPEC.md section 5 asks of a snapshot with that number.
                    set need [gr_right_answers 0.2 0.2 1 $last_number]
                    gr_right_requests $primary $key [expr {$need - 1}]
                    gr_expect $primary $replica $key [dict create state CALIBRATING generation 0 candidate $last_number \
                        calibration-samples [expr {$need - 1}]] "[expr {$need - 1}] right answers for the last snapshot of a key"
                    gr_right_requests $primary $key 1
                    gr_expect $primary $replica $key [dict create state ACTIVE generation $last_number candidate 0] \
                        "$need right answers for the last snapshot of a key"
                    if {[catch {$primary restore $key:answering 0 [$primary dump $key]} reply]} {
                        lappend problems "$key: while its last snapshot answers, a DUMP of the gate is refused by RESTORE ($reply)"
                    }
                    # GRESET never hands a number back, and there is no
                    # other: from here on nothing is tested.
                    assert_equal OK [$primary greset $key]
                    assert_equal {ABSTAIN {}} [$primary gpredict $key SCORE 0.9] "GPREDICT $key, no snapshot number left"
                    set expected {state COLD generation 0 candidate 0 pending 0}
                }
            }
            gr_expect $primary $replica $key $expected "after the requests to $key"
            gr_sync $primary $replica
            if {[$primary debug digest-value $key] ne [$replica debug digest-value $key]} {
                lappend problems "$key: after its requests the replica does not hold the gate of its primary\
                    (gate_apply_mismatches [gr_mismatches $replica])\
                    \n    primary: [gr_describe $primary $key]\
                    \n    replica: [gr_describe $replica $key]"
            }
            # What the primary holds now is what it would save, and what a
            # replica would be sent whole.
            if {[catch {$primary restore $key:copy 0 [$primary dump $key]} reply]} {
                lappend problems "$key: after its requests a DUMP of the gate is refused by RESTORE ($reply):\
                    the gate can no longer be loaded from a file or by a replica"
            }
        }
        # A replica can take these gates whole, too, and follow from there.
        # (What went wrong above is reported first.)
        set failed [catch {
            gr_assert_same $primary $replica $taken "after the requests"
            assert_equal OK [$replica replicaof no one]
            assert_equal OK [$replica replicaof $primary_host $primary_port]
            wait_for_sync $replica
            gr_assert_same $primary $replica $taken "after a full synchronization"
            foreach key $taken {
                if {[dict exists $waiting $key]} {
                    assert_equal 1 [$primary glearn $key [dict get $waiting $key] CORRECT 1] \
                        "GLEARN $key, for a request that waited through the full synchronization"
                }
                lassign [$primary gpredict $key SCORE 0.9] action id
                assert_equal ABSTAIN $action
                if {$key in {edge:clock edge:clock:near}} {
                    assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key $id"
                } else {
                    assert_equal {} $id "the id of a request to $key"
                }
            }
            gr_assert_same $primary $replica $taken "requests after the full synchronization"
        } err]
        if {[llength $problems] > 0} {
            fail "RESTORE took a gate that the server cannot keep:\n[join $problems \n]"
        }
        if {$failed} {
            error $err
        }
    }

    gr_test {Gate replication: a gate restored with a counter at the end of its range counts on, and can still be saved, loaded and sent whole} {
        # CHANGES.md 9d: the encoding holds no counter above 2^62, whatever
        # the gate has counted since.
        $primary config set gate.feedback-timeout 100
        assert_equal OK [$primary gcreate edge:quick ALPHA 0.2 DELTA 0.2 SCORE]
        $primary config set gate.feedback-timeout 600000
        lassign [gr_fresh_dump $primary edge:quick] dump clock_at clock requests_at snapshots_at missing_at
        set forged [string replace $dump $missing_at [expr {$missing_at + 7}] [binary format w $last_number]]
        if {[catch {$primary restore edge:missing 0 $forged} reply]} {
            assert_match {*Bad data format*} $reply
            assert_equal 0 [$primary exists edge:missing]
            return
        }
        gr_assert_same_digests $primary $replica edge:missing "restored"
        assert_equal $last_number [gr_field $primary edge:missing missing]
        # A request that never gets its answer: one more than the encoding
        # can hold.
        lassign [$primary gpredict edge:missing SCORE 0.9] action id
        assert_equal ABSTAIN $action
        gr_wait_age [clock milliseconds] 100
        assert_equal OK [$primary gresume edge:missing]
        assert_equal 0 [$primary glearn edge:missing $id CORRECT 1]
        # The replica counted with its primary.
        gr_assert_same $primary $replica edge:missing "one missing answer more than a counter can hold"
        assert {[gr_field $primary edge:missing missing] >= $last_number}
        # Saved and loaded again, and sent whole.
        assert_equal OK [$primary restore edge:missing:copy 0 [$primary dump edge:missing]]
        assert_equal $last_number [gr_field $primary edge:missing:copy missing]
        assert_equal OK [$replica replicaof no one]
        assert_equal OK [$replica replicaof $primary_host $primary_port]
        wait_for_sync $replica
        gr_assert_same_digests $primary $replica {edge:missing edge:missing:copy} "after a full synchronization"
        assert_equal ABSTAIN [lindex [$primary gpredict edge:missing SCORE 0.9] 0]
        assert_equal OK [$primary gpause edge:missing]
        assert_equal OK [$primary gresume edge:missing]
        gr_assert_same_digests $primary $replica edge:missing "requests after the full synchronization"
    }

    gr_test {Gate replication: a restored gate that has no snapshot number left says so before its first write} {
        # After a RESTORE a gate drops its proof at its first write, and
        # GINFO on the primary shows it as it will be then (gate.c). A gate
        # that has used up its snapshot numbers will test nothing.
        if {[lsearch -exact $taken edge:snapshots] < 0} return
        lassign [gr_fresh_dump $primary edge:new] dump clock_at clock requests_at snapshots_at
        set forged [string replace $dump $snapshots_at [expr {$snapshots_at + 7}] [binary format w $last_number]]
        assert_equal OK [$primary restore edge:announced 0 $forged]
        set announced [gr_recovery_view $primary edge:announced]
        assert_equal OK [$primary gresume edge:announced]
        gr_assert_same $primary $replica edge:announced "after the first write"
        assert_equal COLD [gr_field $primary edge:announced state]
        assert_equal 0 [gr_field $primary edge:announced candidate]
        assert_equal [gr_recovery_view $primary edge:announced] $announced \
            "GINFO of a restored gate without a snapshot number left, after its first write (first) and before (second)"
    }
}
}
