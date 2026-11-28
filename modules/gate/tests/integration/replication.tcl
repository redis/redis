# Redis Gate: primaries, replicas and failover.
#
# The contract under test (SPEC.md, section 3, "Redis"):
#
#   * GPREDICT runs on the primary. Replicas and the AOF receive what actually
#     happened (the random draw, the clock, the recorded guess), never the
#     command, so they never re-roll dice. A replica therefore holds, for
#     every gate, exactly the state of its primary: DEBUG DIGEST-VALUE is the
#     same on both once the replica has caught up.
#   * An ACCEPT writes nothing.
#   * After a failover Redis cannot show that nothing was lost, so a gate
#     keeps its model, drops its proof and its waiting requests, and proves
#     itself again by itself.
#
# The tests use real randomness (gate.test-seed 0) wherever the point is that
# realized values are replicated, and assert only what holds for any draw.
# gate.test-seed is used where exact outcomes are asserted.
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

# Every replica has applied the whole stream of its primary and holds, for
# each of 'keys', the gate the primary holds: same DEBUG DIGEST-VALUE.
proc gr_assert_same_digests {primary replicas keys {phase ""}} {
    foreach replica $replicas {
        wait_for_ofs_sync $primary $replica
    }
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
            if {$w ne $g} {
                fail "$phase: gate $key on replica #$n differs from its primary\
                      \nprimary: [gr_describe $primary $key]\
                      \nreplica: [gr_describe $replica $key]"
            }
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
    foreach replica $replicas {
        wait_for_ofs_sync $primary $replica
    }
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

# The feedback the workload still owes, sent in request order: nothing it
# asked keeps waiting.
proc gr_workload_drain {client} {
    foreach feedback $::gr_pending {
        $client glearn {*}$feedback
    }
    set ::gr_pending {}
    set ::gr_used {}
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

proc gr_log_tail {log {lines 25}} {
    if {[catch {exec tail -n $lines $log} tail]} {
        return "(cannot read $log)"
    }
    return $tail
}

# The servers every test must leave alive and well: levels as 'srv' takes
# them. A replica that could not apply what its primary sent either dies
# (propagation-error-behavior panic) or logs that an operation did not fit
# its gate.
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
        set labels {ALPHA SCORE CLASSES LABEL TEXT {two words} café 日本}
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
        for {set n 0} {$n < 20} {incr n} {
            foreach label {spam ham} {
                $primary gadd cl:swap TEXT [gr_text $label $n] LABEL $label
            }
        }
        # The first snapshot knows one vocabulary and answers for it.
        gr_text_until_active $primary cl:swap {spam ham}
        set generation [gr_field $primary cl:swap generation]
        gr_assert_same $primary $replicas cl:swap "the first snapshot answers"

        # The model learns a second vocabulary: its next snapshot answers
        # more of the same requests, and replaces the first one.
        for {set n 0} {$n < 20} {incr n} {
            $primary gadd cl:swap TEXT [gr_text spam2 $n] LABEL spam
            $primary gadd cl:swap TEXT [gr_text ham2 $n] LABEL ham
        }
        gr_assert_same $primary $replicas cl:swap "the next snapshot is tested"
        foreach node $nodes {
            assert_equal $generation [gr_field $node cl:swap generation]
            assert {[gr_field $node cl:swap candidate] > $generation}
        }
        for {set n 0} {$n < 4000 && [gr_field $primary cl:swap generation] == $generation} {incr n} {
            lassign [lindex {{spam spam} {ham ham} {spam2 spam} {ham2 ham}} [expr {$n % 4}]] words label
            assert {[gr_text_request $primary cl:swap [gr_text $words $n] $label] ne "ACCEPT"}
        }
        gr_assert_same $primary $replicas cl:swap "the next snapshot took over"
        foreach node $nodes {
            assert_equal ACTIVE [gr_field $node cl:swap state]
            assert {[gr_field $node cl:swap generation] > $generation}
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
            assert_equal 3 [gr_field $node to:active missing]
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

    gr_test {Gate replication: RENAME, EXPIRE, DEL, DUMP and RESTORE of a gate} {
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
        gr_workload_drain $primary
        assert_equal OK [$primary gresume sc:audit]
        lassign [$primary gpredict sc:audit SCORE 0.99] action id
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
        lassign [$primary gpredict tmp:copy SCORE 0.99] action id
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
    }

    gr_test {Gate replication: the stream carries what happened, never the commands} {
        set repl [attach_to_replication_stream]
        set changes 0

        assert_equal OK [$primary gcreate st:score ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate st:text ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        assert_equal 1 [$primary gadd st:text TEXT [gr_text spam 1] LABEL spam]
        lassign [$primary gpredict st:score SCORE 0.9] action id
        assert_equal 1 [$primary glearn st:score $id CORRECT 1]
        lassign [$primary gpredict st:text TEXT [gr_text ham 1] FEATURES 1 depth 0.5] action id
        assert_equal 1 [$primary glearn st:text $id LABEL ham]
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
        gr_assert_same $primary $replicas {st:score st:text} "after the watched commands"
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
        lassign [$primary gpredict sc:audit SCORE 0.99] action id
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
        set gates [concat $allgates {st:score st:text}]
        gr_assert_same_digests $primary $replicas $gates "before loading the AOF"
        # Everything the replica wrote to its AOF so far came from the
        # stream: replaying it must give the same gates.
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
        gr_assert_same_dataset $replica $subreplica "at the end"
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
        assert_equal 4 [gr_field $primary ac:late missing]
        assert_equal 4 [gr_field $replica ac:late missing]
        $primary config set repl-ping-replica-period 10
        gr_assert_same_dataset $primary $replica "at the end"
    }
}
}

# ----------------------------------------- a replica attached to live gates
#
# The gates exist, answer and have requests waiting before the replica is
# attached: it gets them as a whole, through a full synchronization. Once
# with the dataset loaded from a file, once loaded from the socket into a
# database swapped in at the end (repl-diskless-load swapdb).

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
        gr_assert_same_dataset $primary $replica "at the end"
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

        gr_assert_same $first $second $gates "before the failover"
        set before [dict create]
        foreach key $gates {
            dict set before $key [gr_info $second $key]
        }
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
        # Looking at a gate does not change it.
        assert_equal [$first debug digest-value {*}$gates] [$second debug digest-value {*}$gates]
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
        # Paused it was, paused it stays, proving itself all the same.
        gr_score_until_active $second fo:calib
        for {set n 0} {$n < 100} {incr n} {
            assert {[gr_score_request $second fo:paused 0.9] ne "ACCEPT"}
        }
        assert_equal PAUSED [gr_field $second fo:paused state]
        assert {[gr_field $second fo:paused generation] > [dict get $before fo:paused generation]}
    }

    gr_test {Gate failover: the old primary, made a replica of the new one, converges} {
        assert_equal OK [$first replicaof $second_host $second_port]
        wait_for_sync $first
        gr_watch 0
        gr_assert_same $second $first $gates "the old primary as a replica"
        assert_equal ACTIVE [gr_field $first fo:score state]
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
