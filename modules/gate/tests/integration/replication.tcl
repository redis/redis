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
}

proc gr_text {label n} {
    set words [dict get $::gr_words $label]
    set len [llength $words]
    return "[lindex $words [expr {$n % $len}]] [lindex $words [expr {($n * 7 + 3) % $len}]] [lindex $words [expr {($n * 3 + 1) % $len}]]"
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
        if {!$lossy || [gr_rand 100] >= 15} {
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
                $client gadd $key TEXT [gr_text $label [gr_rand 1000]] LABEL $label
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
                $primary gadd cl:three TEXT [gr_text $label $n] LABEL $label
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

    gr_test {Gate replication: feedback out of order, repeated and unknown} {
        assert_equal OK [$primary gcreate sc:order ALPHA 0.2 DELTA 0.2 SCORE]
        set ids {}
        for {set n 0} {$n < 30} {incr n} {
            lassign [$primary gpredict sc:order SCORE 0.9] action id
            assert_equal ABSTAIN $action
            lappend ids $id
        }
        gr_assert_same $primary $replicas sc:order "30 requests waiting"

        # Newest first, and not the oldest: every answer is used, but proof
        # is applied in request order, so all of them wait for the oldest.
        foreach id [lreverse [lrange $ids 1 end]] {
            assert_equal 1 [$primary glearn sc:order $id CORRECT 1]
        }
        gr_assert_same $primary $replicas sc:order "29 answers waiting for an older one"
        foreach node $nodes {
            assert_equal 0 [gr_field $node sc:order calibration-samples]
        }

        assert_equal 1 [$primary glearn sc:order [lindex $ids 0] CORRECT 1]
        gr_assert_same $primary $replicas sc:order "all answered"
        foreach node $nodes {
            assert_equal 30 [gr_field $node sc:order calibration-samples]
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
            assert_equal 30 [gr_field $node sc:order calibration-samples]
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
        assert_equal OK [$primary gcreate to:score ALPHA 0.2 DELTA 0.2 SCORE]
        assert_equal OK [$primary gcreate to:text ALPHA 0.2 DELTA 0.2 CLASSES 2 spam ham]
        assert_equal OK [$primary gcreate to:active ALPHA 0.2 DELTA 0.2 AUDIT 0.5 SCORE]
        $primary config set gate.feedback-timeout 600000

        set lost {}
        for {set n 0} {$n < 5} {incr n} {
            lassign [$primary gpredict to:score SCORE 0.9] action id
            assert_equal ABSTAIN $action
            lappend lost to:score $id CORRECT 1
            lassign [$primary gpredict to:text TEXT [gr_text spam $n]] action id
            assert_equal ABSTAIN $action
            lappend lost to:text $id LABEL spam
        }
        gr_assert_same $primary $replicas {to:score to:text} "requests waiting"

        # The replicas do not watch the clock: they learn that the requests
        # waited too long when the primary finds out, at its next write.
        after 200
        lassign [$primary gpredict to:score SCORE 0.9] action fresh
        $primary gadd to:text TEXT [gr_text ham 1] LABEL ham
        gr_assert_same $primary $replicas {to:score to:text} "requests timed out"
        foreach node $nodes {
            # Each was needed as proof and never got its GLEARN.
            assert_equal 5 [gr_field $node to:score missing]
        }
        foreach {key id word answer} $lost {
            assert_equal 0 [$primary glearn $key $id $word $answer] "GLEARN of a request that timed out"
        }
        gr_assert_same $primary $replicas {to:score to:text} "late feedback"

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
        assert_equal OK [$primary gcreate mix:c1 ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        assert_equal OK [$primary gcreate mix:c2 ALPHA 0.2 DELTA 0.2 CLASSES 3 billing tech other ALWAYSDEFER 1 other]
        $primary config set gate.feedback-timeout 100
        assert_equal OK [$primary gcreate mix:lossy:s ALPHA 0.2 DELTA 0.2 AUDIT 0.3 SCORE]
        assert_equal OK [$primary gcreate mix:lossy:c ALPHA 0.2 DELTA 0.2 AUDIT 0.3 CLASSES 2 spam ham]
        $primary config set gate.feedback-timeout 600000
        set mix {
            {mix:s1 score {} 0}
            {mix:s2 score {} 0}
            {mix:c1 text {spam ham} 0}
            {mix:c2 text {billing tech other} 0}
            {mix:lossy:s score {} 1}
            {mix:lossy:c text {spam ham} 1}
            {sc:plain score {} 0}
            {sc:audit score {} 0}
            {cl:two text {spam ham} 0}
            {cl:three text {billing tech other} 0}
        }
        set mixkeys {}
        foreach gate $mix {
            lappend mixkeys [lindex $gate 0]
        }

        for {set round 1} {$round <= 8} {incr round} {
            gr_workload $primary $mix 600
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
                6 {
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
        gr_assert_same $primary $replicas [concat $mixkeys {sc:order cl:order cl:wide to:score to:text to:active}] "after the mixed workload"
    }
}
}
}
