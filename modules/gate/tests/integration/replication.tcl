# Redis Gate: primaries, replicas and failover.
#
# The contract under test (SPEC.md, section 3: "Redis", "Late and lost
# answers", "No answers at all"; CHANGES.md):
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
#     requests in a row ended without an answer: each of these happens inside
#     whatever write command comes next, at the clock of the primary, and
#     reaches the replicas as what happened: with the operation of that
#     command or, when the command replicates nothing by itself (an ACCEPT),
#     as an operation of its own (TICK), and only then.
#   * After a failover Redis cannot show that nothing was lost, so a gate
#     keeps its model, drops its proof and its waiting requests, and proves
#     itself again by itself.
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
# the workloads check that after every phase.
#
# Run from the repository root:
#   ./runtest --single ../modules/gate/tests/integration/replication

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
# whole suite, and every test ends by checking the watched servers.
proc gr_body {body} {
    if {[catch {uplevel 1 $body} err]} {
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
        lassign [$client gpredict $key SCORE $score] action id
        assert_equal ABSTAIN $action "GPREDICT $key, not answering yet"
        assert_equal 1 [$client glearn $key $id CORRECT 1] "GLEARN $key $id"
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
# ---------------------------------------------- a replica attached at birth
#
# Three servers in a chain: primary -> replica -> sub-replica. Both replicas
# are attached before any gate exists, so everything they know about a gate
# came through the replication stream. The replica keeps an AOF.

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

    gr_test {Gate replication over time: a gate that gets no answers stops on the replicas too, and starts again when answers come} {
        set flawless [gr_right_answers 0.2 0.2 1 1]
        set timeout 300
        $primary config set gate.feedback-timeout $timeout
        # Two gates that answer and check every request they could answer
        # (AUDIT 1), one that does not answer yet, and a classifier.
        assert_equal OK [$primary gcreate out:active ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        assert_equal OK [$primary gcreate out:almost ALPHA 0.2 DELTA 0.2 AUDIT 1 SCORE]
        assert_equal OK [$primary gcreate out:calib ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate out:text ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 600000
        set outage {out:active out:almost out:calib out:text}
        gr_right_requests $primary out:active $flawless
        gr_right_requests $primary out:almost $flawless
        for {set n 0} {$n < 10} {incr n} {
            foreach label {spam ham} {
                $primary gadd out:text TEXT [gr_text $label $n] LABEL $label
            }
        }
        gr_text_until_active $primary out:text {spam ham}
        gr_assert_same $primary $replicas $outage "before the answers stop"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node out:active state]
            assert_equal ACTIVE [gr_field $node out:almost state]
            assert_equal CALIBRATING [gr_field $node out:calib state]
            assert_equal ACTIVE [gr_field $node out:text state]
        }
        set learned [gr_field $primary out:text learned]
        set numbered [expr {max([gr_field $primary out:text generation], [gr_field $primary out:text candidate])}]

        # Requests that never get their answer: 32 in a row for three of
        # the gates, 31 for the fourth. Each of them is a spot check, or
        # proof for a gate that does not answer yet: its answer is needed.
        set lost [dict create]
        foreach {key count} {out:active 32 out:almost 31 out:calib 32 out:text 32} {
            for {set n 0} {$n < $count} {incr n} {
                if {$key eq "out:text"} {
                    lassign [$primary gpredict $key TEXT [gr_text [lindex {spam ham} [expr {$n % 2}]] $n]] action id
                } else {
                    lassign [$primary gpredict $key SCORE 0.9] action id
                }
                assert {$action ne "ACCEPT" && $id ne {}}
                dict lappend lost $key $id
            }
        }
        set t1 [clock milliseconds]
        gr_assert_same $primary $replicas $outage "requests waiting"

        # When the feedback timeout is over, the next write to a gate gives
        # all of them up, on every node.
        gr_wait_age $t1 $timeout
        set stopped [$primary gpredict out:active SCORE 0.9]
        # 31 in a row: this gate goes on, the request is a spot check, and
        # it gets its answer.
        lassign [$primary gpredict out:almost SCORE 0.9] action id
        assert_equal AUDIT $action
        assert_equal 1 [$primary glearn out:almost $id CORRECT 1]
        set stale [$primary glearn out:calib [lindex [dict get $lost out:calib] 0] CORRECT 1]
        $primary gadd out:text TEXT [gr_text spam 50] LABEL spam
        gr_assert_same $primary $replicas $outage "the feedback timeout is over"
        assert_equal 0 $stale
        # 32 in a row: the gate has stopped. It hands out an id all the
        # same, so that there is something to answer.
        assert_equal ABSTAIN [lindex $stopped 0]
        assert {[lindex $stopped 1] ne {}}
        foreach node $nodes {
            foreach key {out:active out:calib out:text} {
                set info [gr_info $node $key]
                assert_equal COLD [dict get $info state] "state of $key"
                assert {[gr_not_answering $info]}
                # No snapshot is taken while no answer arrives.
                assert_equal 0 [dict get $info candidate] "candidate of $key"
                assert_equal 32 [dict get $info missing] "missing of $key"
            }
            assert_equal 1 [gr_field $node out:active revocations]
            assert_equal 1 [gr_field $node out:active pending]
            # Nothing answered yet, so nothing stopped answering.
            assert_equal 0 [gr_field $node out:calib revocations]
            assert_equal 0 [gr_field $node out:calib pending]
            assert_equal 1 [gr_field $node out:text revocations]
            # Missing answers say nothing about the model: it is kept.
            assert_equal [expr {$learned + 1}] [gr_field $node out:text learned]
            set info [gr_info $node out:almost]
            assert_equal ACTIVE [dict get $info state]
            assert_equal 1 [dict get $info generation]
            assert_equal 31 [dict get $info missing]
            assert_equal 0 [dict get $info revocations]
            assert_equal 0 [dict get $info pending]
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
        foreach key {out:active out:calib} {
            lassign [$primary gpredict $key SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert {$id ne {}}
        }
        lassign [$primary gpredict out:text TEXT [gr_text ham 51]] action id
        assert_equal ABSTAIN $action
        assert {$id ne {}}
        gr_assert_same $primary $replicas $outage "requests to gates that have stopped"
        foreach node $nodes {
            foreach key {out:active out:calib out:text} {
                assert_equal 0 [gr_field $node $key candidate] "candidate of $key"
                assert_equal COLD [gr_field $node $key state]
            }
        }

        # The first answer that arrives (to a request made just now): the
        # gates are tested again, under numbers that were never used.
        foreach key {out:active out:calib} {
            lassign [$primary gpredict $key SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert_equal 1 [$primary glearn $key $id CORRECT 1] "GLEARN $key, the first answer since the gate stopped"
        }
        lassign [$primary gpredict out:text TEXT [gr_text ham 52]] action id
        assert_equal ABSTAIN $action
        assert_equal 1 [$primary glearn out:text $id LABEL ham]
        gr_assert_same $primary $replicas $outage "answers arrive again"
        foreach node $nodes {
            foreach key {out:active out:calib} {
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
        gr_right_requests $primary out:active [expr {$again - 1}]
        gr_sync $primary $replicas
        foreach node $nodes {
            assert_equal CALIBRATING [gr_field $node out:active state]
        }
        gr_right_requests $primary out:active 1
        gr_assert_same $primary $replicas $outage "the gate answers again"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node out:active state]
            assert_equal 2 [gr_field $node out:active generation]
            assert_equal 1 [gr_field $node out:active revocations]
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
start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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
# created, and where in it two numbers of the gate are (gate_codec.c):
# returns {dump clock-offset clock requests-offset}.
proc gr_fresh_dump {client key} {
    set dump [$client dump $key]
    # The encoding of a gate starts with "GATE". In a score gate the time it
    # was created at and its clock, two 64 bit integers, follow 65 bytes
    # later.
    set at [string first GATE $dump]
    set created 0
    set clock 0
    if {$at >= 0} {
        binary scan [string range $dump [expr {$at + 65}] [expr {$at + 80}]] ww created clock
    }
    # The number of the next request, another 64 bit integer, stands before
    # the count of waiting requests (none), the fifteen counters and what
    # DUMP adds after a value (11 bytes): 143 bytes before the end.
    set requests [expr {[string length $dump] - 143}]
    set next 0
    binary scan [string range $dump $requests [expr {$requests + 7}]] w next
    set now [clock milliseconds]
    if {$at < 0 || $created != $clock || $clock > $now || $clock < $now - 60000 || $next != 1} {
        fail "the dump of a gate does not hold its clock and its request number where this test looks for them (found $created, $clock and $next; the time is $now): the encoding has changed, and gr_fresh_dump must follow"
    }
    return [list $dump [expr {$at + 73}] $clock $requests]
}

# RESTORE a copy of such a gate as 'copy', with a clock that is 'ahead' ms
# ahead. Returns that clock.
proc gr_restore_ahead {client key copy ahead} {
    lassign [gr_fresh_dump $client $key] dump at clock
    set dump [string replace $dump $at [expr {$at + 7}] [binary format w [expr {$clock + $ahead}]]]
    assert_equal OK [$client restore $copy 0 $dump]
    return [expr {$clock + $ahead}]
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
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

# ------------------------------------- a gate at the end of what it can count
#
# The clock of a gate and its counters have a range, and a gate beyond it is
# not loaded: not from a dump, not from a file, not by a replica (gate_core.h,
# TIME; gate_codec.c). A dump can put a gate exactly at the end of a range.
# Whatever a server takes with RESTORE it must be able to keep: a gate that
# still works one request later, that its replicas hold as it does, and that
# can be saved and loaded again. Refusing such a dump is as good.
#
# (These servers come last: a replica that could not follow stays behind.)

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule]] {
start_server [list overrides [list loadmodule $gatemodule]] {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set replica [srv -1 client]
    gr_watch -1

    gr_test {Gate replication: a gate restored at the end of the range of its clock or of its request numbers is refused, or one a replica can follow and take whole} {
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
        lassign [gr_fresh_dump $primary edge:new] dump clock_at clock requests_at
        # The last clock a gate may have, and the last number a counter may
        # reach (gate_core.h, gate_codec.c).
        set last_clock [expr {9223372036854775807 - 7 * 86400 * 1000}]
        set last_number [expr {1 << 62}]
        set made [dict create \
            edge:clock [string replace $dump $clock_at [expr {$clock_at + 7}] [binary format w $last_clock]] \
            edge:requests [string replace $dump $requests_at [expr {$requests_at + 7}] [binary format w $last_number]]]

        set taken {}
        set problems {}
        dict for {key dump} $made {
            if {[catch {$primary restore $key 0 $dump} reply]} {
                # Refused: nothing to keep.
                assert_match {*Bad data format*} $reply
                assert_equal 0 [$primary exists $key]
                continue
            }
            lappend taken $key
            # A few requests and their answers, some ms apart.
            for {set n 0} {$n < 3} {incr n} {
                lassign [$primary gpredict $key SCORE 0.9] action id
                assert_equal ABSTAIN $action
                after 5
                assert_equal 1 [$primary glearn $key $id CORRECT 1]
            }
            gr_sync $primary $replica
            if {[$primary debug digest-value $key] ne [$replica debug digest-value $key]} {
                lappend problems "$key: after three requests the replica does not hold the gate of its primary\
                    (gate_apply_mismatches [gr_mismatches $replica])\
                    \n    primary: [gr_describe $primary $key]\
                    \n    replica: [gr_describe $replica $key]"
            }
            # What the primary holds now is what it would save, and what a
            # replica would be sent whole.
            if {[catch {$primary restore $key:copy 0 [$primary dump $key]} reply]} {
                lappend problems "$key: after three requests a DUMP of the gate is refused by RESTORE ($reply):\
                    the gate can no longer be loaded from a file or by a replica"
            }
        }
        if {[llength $problems] > 0} {
            fail "RESTORE took a gate that the server cannot keep:\n[join $problems \n]"
        }
        if {[llength $taken] > 0} {
            # Nothing wrong so far: then a replica can take these gates
            # whole, too.
            assert_equal OK [$replica replicaof no one]
            assert_equal OK [$replica replicaof $primary_host $primary_port]
            wait_for_sync $replica
            gr_assert_same_digests $primary $replica $taken "after a full synchronization"
        }
    }
}
}
