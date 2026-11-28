# Gate module: persistence.
#
# RDB (DEBUG RELOAD, SAVE and restart, BGSAVE), AOF (DEBUG LOADAOF, rewrite,
# restart) and DUMP / RESTORE, including damaged payloads.
#
# What SPEC.md promises (CHANGES.md says what the revised core changed) and
# these tests check:
#
#   - a gate is one key: whatever Redis does to persist a key keeps the gate
#     exactly as it was (same DEBUG DIGEST-VALUE), in every state a gate can
#     rest in. The revised core added four: answers that wait for the answer
#     of an older request; requests that proof moved on without, which count
#     as wrong guesses until their answer arrives; a gate that stopped
#     because its answers stopped arriving; and one that has just resumed;
#   - the AOF receives what actually happened, never the command, and that
#     includes what the clock did: a late answer, a lost one, an outage;
#   - after a restart, a reload or a RESTORE the gate keeps its configuration
#     and its model, drops its proof and its waiting requests, and proves
#     itself again by itself. Until then it does not answer, ids issued
#     before are refused (GLEARN replies 0), and snapshot numbers are never
#     reused, so the proof it needs is that of the next number;
#   - a damaged payload is refused, or what it leaves is a gate.
#
# The final round of the core (CHANGES.md section 9) added to what a gate
# holds, and with it to what must come back: the spot checks of an
# answering snapshot, numbered in the order they were asked and counted in
# blocks, with the disagreements and the lost answers of the blocks that are
# open; for a snapshot that is tested behind another, how many of its proof
# requests the other one would answer (what a takeover is decided on) and
# how many it left to the teacher for nothing; and the request number at the
# last answer, which tells an outage from answers lost one by one. It also
# says that a gate that loads can be saved and loaded for good, whatever
# happens to it, even if it was restored with a counter or its clock at the
# end of its range. A client's first write makes a gate that came back
# start over; an AOF, or a primary, goes on from the gate as it is. The
# tests take that way too, with operations appended to the AOF file or sent
# by a connection that is marked internal. Where a test needs to see a field
# of a serialized gate it reads the layout stated in gate_codec.c
# (gp_blob_fields), which a test of its own holds against live gates.
#
# Proof needs many requests at realistic settings, so the gates here use
# ALPHA 0.2 DELTA 0.2 (a threshold is proven after a few dozen right
# answers: gp_need says how many) and a small snapshot step. Every scenario
# seeds the module's test sequence (gate.test-seed), so runs repeat exactly.
#
# TIME. A gate does its bookkeeping when it is written to, never by itself.
# The tests that need time to pass create their gates with a short feedback
# timeout, sleep once (::gp_wait) and then write. Three timeouts are used:
#
#   ::gp_lost   shorter than the sleep: after it, what was not answered is
#               lost;
#   ::gp_late   four times longer than the sleep, nearly: after it, proof
#               moves on without an unanswered request that holds up
#               answers (a quarter of the timeout has passed), and its
#               answer can still arrive;
#   ::gp_mid    between the two: proof moves on after the first sleep, the
#               answer is lost after a second one (::gp_wait2).
#
# What must not happen is a pause of the whole machine of more than about
# 0.6 seconds at the wrong moment; the scenarios notice that and say so.
#
# Each test builds the gates it needs. The exceptions are the tests around a
# restart and those that follow one set of gates through several ways of
# saving them; they say what they follow from.

set gatemodule [file normalize modules/gate/gate.so]
# Another build of the module, for mutation testing.
if {[info exists ::env(GATE_TEST_MODULE)]} { set gatemodule $::env(GATE_TEST_MODULE) }

# The largest value of a counter, a request number or a snapshot number, and
# the last moment of a gate's clock (gate_defs.h: GATE_COUNT_MAX, 2^62, and
# GATE_MAX_CLOCK_MS, a week before the largest number there is).
set ::gp_count_max 4611686018427387904
set ::gp_clock_max [expr {9223372036854775807 - 7 * 86400 * 1000}]

set ::gp_wait 800
set ::gp_wait2 850
set ::gp_lost 700
set ::gp_late 3000
set ::gp_mid 1600

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
# numbering of the synthetic requests. (Redis does not call the setter of a
# value that does not change: 0 first, so that the sequence restarts even
# if the seed is the one already set.)
proc gp_begin {seed} {
    r config set gate.test-seed 0
    r config set gate.test-seed $seed
    set ::gp_n 0
}

# GINFO as a dict, or one field of it.
proc gp_info {key {field {}}} {
    set info [r ginfo $key]
    if {$field eq {}} { return $info }
    dict get $info $field
}

# The two server settings that are read when a gate is created, back to
# their defaults.
proc gp_defaults {} {
    r config set gate.snapshot-examples 1000
    r config set gate.feedback-timeout 60000
}

# A classifier gate over two topics. The snapshot step and the feedback
# timeout are server settings read when the gate is created. They are put
# back to their defaults at once, so every test also shows that a gate keeps
# the values it was created with.
proc gp_classifier {key {snapshot 20} {audit 0.2} {timeout 60000}} {
    r config set gate.snapshot-examples $snapshot
    r config set gate.feedback-timeout $timeout
    assert_equal OK [r gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT $audit CLASSES 2 spam ham]
    gp_defaults
}

proc gp_score {key {audit 0.2} {timeout 60000}} {
    r config set gate.feedback-timeout $timeout
    assert_equal OK [r gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT $audit SCORE]
    gp_defaults
}

# How many right answers a threshold of snapshot number $g needs before it
# is proven (SPEC.md section 5). Its wealth starts at 1 and is multiplied by
# 1 + c alpha / (1 - alpha) for a right guess and by 1 - c for a wrong one,
# for each c of 0.1, 0.25, 0.5 and 0.9; it is proven when the average of the
# four reaches 32 x labels x g (g + 1) / delta, where labels is the number
# of labels the gate may answer (1 for a score gate). $errors wrong guesses
# come before the right ones.
proc gp_need {g {labels 1} {errors 0} {alpha 0.2} {delta 0.2}} {
    set bar [expr {32.0 * $labels * $g * ($g + 1) / $delta}]
    set wealth {}
    foreach c {0.1 0.25 0.5 0.9} { lappend wealth [expr {pow(1.0 - $c, $errors)}] }
    set before 1.0
    for {set n 1} {$n <= 100000} {incr n} {
        set sum 0.0
        set grown {}
        foreach w $wealth c {0.1 0.25 0.5 0.9} {
            set w [expr {$w * (1.0 + $c * $alpha / (1.0 - $alpha))}]
            lappend grown $w
            set sum [expr {$sum + $w}]
        }
        set wealth $grown
        set now [expr {$sum / 4.0}]
        if {$now >= $bar} {
            # The tests assert this number exactly: it must not hang on the
            # last digits of a sum.
            if {$now < $bar * 1.0001 || $before > $bar * 0.9999} {
                fail "gp_need: snapshot $g of $labels labels reaches its bar too closely for an exact test"
            }
            return $n
        }
        set before $now
    }
    fail "gp_need: no proof in 100000 answers"
}

# The 32 thresholds of a label (SPEC.md section 5): evenly spaced in
# log(1 - threshold), from max(1 / classes, 0.25) to 0.999 for a classifier
# and from 0 to 0.998 for a score gate ($classes 0).
proc gp_grid {classes} {
    if {$classes} {
        set low [expr {max(1.0 / $classes, 0.25)}]
        set high 0.999
    } else {
        set low 0.0
        set high 0.998
    }
    set grid {}
    for {set t 0} {$t < 32} {incr t} {
        lappend grid [expr {1.0 - (1.0 - $low) * pow((1.0 - $high) / (1.0 - $low), $t / 31.0)}]
    }
    return $grid
}

# Which of them a threshold is that GINFO shows: 0 for the lowest.
proc gp_grid_index {classes threshold} {
    set t 0
    foreach value [gp_grid $classes] {
        if {abs($value - $threshold) < 1e-9} { return $t }
        incr t
    }
    fail "gp_grid_index: $threshold is none of the 32 thresholds"
}

# The drift detector (SPEC.md section 5). The spot checks of a label are cut
# into blocks in the order they were asked. The statistic of a block is the
# average of two likelihood ratios, for an error rate of 2 and of 4 times
# ALPHA (at most halfway from ALPHA to 1): every spot check asked in the
# block so far multiplies a ratio by (1 - p) / (1 - ALPHA), or by p / ALPHA
# if it counts against the gate. $asked spot checks were asked, $against of
# them count against the gate. (The tests ask fewer than 64 spot checks of
# any label of a snapshot, so all of them are in its first block of each
# length.)
proc gp_block {asked against {alpha 0.2}} {
    set sum 0.0
    foreach times {2 4} {
        set p [expr {min($times * $alpha, ($alpha + 1.0) / 2)}]
        set sum [expr {$sum + pow($p / $alpha, $against) * pow((1.0 - $p) / (1.0 - $alpha), $asked - $against)}]
    }
    expr {$sum / 2.0}
}

# 1 if a block with these counts makes the gate stop. Counting the
# disagreements the level is 1,000 (the gate got worse: it forgets its model
# and starts over); counting the lost answers as well it is 10^9 (the gate
# can no longer be checked: it stops, keeps its model, and waits for an
# answer).
proc gp_block_stops {asked against level {alpha 0.2}} {
    set stat [gp_block $asked $against $alpha]
    # The tests assert the outcome exactly: it must not hang on the last
    # digits of a product.
    if {$stat > $level * 0.999 && $stat < $level * 1.001} {
        fail "gp_block_stops: $against of $asked spot checks is too close to $level for an exact test"
    }
    expr {$stat >= $level}
}

# Of $asked spot checks asked in a block, $wrong of which were answered and
# disagreed, how many lost answers make the gate stop: the smallest number,
# or 0 if losing all the others does not.
proc gp_lost_stop {asked {wrong 0} {alpha 0.2}} {
    for {set lost 1} {$wrong + $lost <= $asked} {incr lost} {
        if {[gp_block_stops $asked [expr {$wrong + $lost}] 1e9 $alpha]} { return $lost }
    }
    return 0
}

# Request number $n of the synthetic traffic, as the arguments of GPREDICT,
# and what the teacher says about it, as the arguments of GLEARN.
# Classifier: two topics that are easy to tell apart. Score gate: three
# requests out of four have a high score and a right cheap answer, the
# fourth has a low score and a wrong one, so the proven threshold lies
# between the two scores.
proc gp_request {kind n} {
    if {$kind eq "classifier"} {
        return [gp_text [expr {$n % 2 ? "spam" : "ham"}] $n]
    }
    if {$n % 4 == 0} { return [list {SCORE 0.3} {CORRECT 0}] }
    return [list {SCORE 0.9} {CORRECT 1}]
}

# A text about one topic for a classifier, and the teacher's label for it:
# spam, ham, or "odd": a text the teacher calls ham that shares no word
# with the other ham.
proc gp_text {topic n} {
    switch $topic {
        spam { return [list [list TEXT "buy cheap pills now offer $n"] {LABEL spam}] }
        ham { return [list [list TEXT "meeting notes project schedule $n"] {LABEL ham}] }
        odd { return [list [list TEXT "invoice attached quarterly report $n"] {LABEL ham}] }
    }
    fail "gp_text: no topic $topic"
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
# GINFO shows such a gate as its first write will leave it.
#
# $from says where it came from. An RDB file and a DUMP payload hold the
# request counters; the AOF only knows of the requests that were written to
# it (a request the gate answered by itself was not), so there the counters
# only have to add up.
proc gp_assert_came_back {key before {from rdb}} {
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
    set state [dict get $now state]
    if {[dict get $before state] eq "PAUSED"} {
        assert_equal PAUSED $state "state of $key"
    } else {
        assert {$state in {COLD CALIBRATING}}
    }
    # A snapshot is going to be tested, with a number that was never used,
    # or none is. A score gate has nothing to learn first. ("Before" may
    # itself be what GINFO shows of a gate that came back and was not
    # written to since: the snapshot it announces was not taken, and is
    # announced again. A gate that held proof or requests was no such gate.)
    set used [expr {max([dict get $before generation], [dict get $before candidate])}]
    set held [expr {[dict get $before generation] > 0 || [dict get $before pending] > 0 ||
                    [dict get $before calibration-samples] > 0}]
    set candidate [dict get $now candidate]
    if {$state eq "CALIBRATING" || [dict get $now kind] eq "score"} {
        assert {$candidate > 0}
        if {$held} { assert {$candidate > $used} } else { assert {$candidate >= $used} }
    } elseif {$state eq "COLD"} {
        assert_equal 0 $candidate "candidate of $key"
    } elseif {$held} {
        assert {$candidate == 0 || $candidate > $used}
    }
    if {[dict get $now kind] eq "score" && $state ne "PAUSED"} {
        assert_equal CALIBRATING $state "state of $key"
    }
    # What went wrong before is neither forgotten nor counted twice.
    foreach field {missing revocations} {
        assert_equal [dict get $before $field] [dict get $now $field] "$field of $key"
    }
    if {$from ne "aof"} {
        foreach field {requests accepted audited abstained} {
            assert_equal [dict get $before $field] [dict get $now $field] "$field of $key"
        }
    }
    assert_equal [dict get $now requests] \
        [expr {[dict get $now accepted] + [dict get $now audited] + [dict get $now abstained]}] \
        "requests of $key and the sum of what became of them"
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
    foreach cmd {gcreate gpredict glearn gadd gpause gresume greset ginfo} {
        assert {![dict exists $names $cmd]}
    }
}

# How many operations (of its AOF, or of a primary) the server found not to
# fit their gate, by its own count.
proc gp_mismatches {} {
    getInfoProperty [r info gate] gate_apply_mismatches
}

# Every operation the server has applied so far fitted its gate.
proc gp_assert_all_fit {} {
    assert_equal 0 [count_log_message 0 "did not fit"]
    assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
}

# Make sure that everything the server has to write is in its AOF file: with
# appendfsync everysec a write can wait for a sync that is still running.
proc gp_aof_flush {} {
    r config set appendfsync always
    r set gp:flushed 1
    r config set appendfsync everysec
}

# The operations an AOF file (not an RDB preamble) holds for a key, in
# order: the names that follow "_GATE.APPLY key", or {name clock} pairs
# (a RESTORE written by a rewrite has no clock), or the whole commands.
proc gp_aof_ops {path key {what names}} {
    set fp [open $path r]
    fconfigure $fp -translation binary
    set ops {}
    while {1} {
        set cmd [read_from_aof $fp]
        if {$cmd eq {}} break
        if {[lindex $cmd 0] ne "_gate.apply" || [lindex $cmd 1] ne $key} continue
        switch $what {
            names { lappend ops [lindex $cmd 2] }
            clocks { lappend ops [list [lindex $cmd 2] [lindex $cmd 3]] }
            commands { lappend ops $cmd }
        }
    }
    close $fp
    return $ops
}

# The same for every key at once: a dict, key -> the commands of its
# operations, in order.
proc gp_aof_read {path} {
    set fp [open $path r]
    fconfigure $fp -translation binary
    set ops {}
    while {1} {
        set cmd [read_from_aof $fp]
        if {$cmd eq {}} break
        if {[lindex $cmd 0] eq "_gate.apply"} { dict lappend ops [lindex $cmd 1] $cmd }
    }
    close $fp
    return $ops
}

# One command in the format of the AOF.
proc gp_aof_format {args} {
    set out "*[llength $args]\r\n"
    foreach arg $args { append out "\$[string length $arg]\r\n$arg\r\n" }
    return $out
}

# Append commands to the AOF file behind the back of the server, as a
# damaged or hand-made file would hold them, and make the server load its
# file. (The operations of a gate are an internal command: no client can
# send them, but the file can hold anything.)
proc gp_aof_load_with {commands} {
    gp_aof_flush
    set fp [open [get_last_incr_aof_path r] a]
    fconfigure $fp -translation binary
    foreach command $commands { puts -nonewline $fp [gp_aof_format {*}$command] }
    close $fp
    r debug loadaof
}

# The same for a file that holds operations which do not fit their gate: the
# server reads its whole file again and counts every one of them once more.
# $misfits of the $commands are such operations (::gp_file_misfits is how
# many the file holds so far). Holds the server's count against that.
proc gp_aof_load_counting {commands misfits} {
    set before [gp_mismatches]
    incr ::gp_file_misfits $misfits
    set reply [gp_aof_load_with $commands]
    assert_equal [expr {$before + $::gp_file_misfits}] [gp_mismatches] "operations that did not fit their gate"
    return $reply
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
# $crc is the checksum of what came before $data, if anything did.
proc gp_crc64 {data {crc 0}} {
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

# Many damaged copies of one large gate: the checksum of what a copy has in
# common with the original is not computed again for each. gp_dump_base
# notes the checksum of the original payload at every 4,096th byte of the
# serialized gate.
proc gp_dump_base {head blob} {
    set prefix $head
    append prefix [gp_rdb_length [string length $blob]]
    set crc [gp_crc64 $prefix]
    set marks [list $crc]
    set length [string length $blob]
    for {set at 0} {$at + 4096 <= $length} {incr at 4096} {
        set crc [gp_crc64 [string range $blob $at [expr {$at + 4095}]] $crc]
        lappend marks $crc
    }
    list $prefix $length $marks
}

# The payload for a serialized gate that has the length of the original
# behind $base and its bytes up to byte $from, with a valid checksum.
proc gp_dump_damage {base damaged version from} {
    lassign $base prefix length marks
    if {[string length $damaged] != $length} { fail "gp_dump_damage: the damaged gate must keep its length" }
    set mark [expr {min($from / 4096, [llength $marks] - 1)}]
    set tail [string range $damaged [expr {$mark * 4096}] end]
    append tail "\x00" $version
    set payload $prefix
    append payload $damaged "\x00" $version [binary format w [gp_crc64 $tail [lindex $marks $mark]]]
    return $payload
}

# ----------------------- The layout of a serialized gate -------------------
# gate_codec.c states the layout: integers little endian, f64 as IEEE-754
# bits, K the number of classes (0 for a score gate), R the number of rows
# (K, or 1), V the number of weight vectors (1 when K is 2, else K):
#
#   header      "GATE", u16 codec version, u16 features version
#   config      u8 kind, f64 alpha, f64 delta, f64 audit, u32 K,
#               K x (u32 length, label), K x f64 class alpha, K x u8 defer,
#               u32 snapshot examples, i64 feedback timeout
#   ledger      u64 lineage, u64 epoch, i64 created, i64 clock,
#               u64 generations, u8 paused, u64 snapshot_updates, u8 blind,
#               u32 missing_run, u64 answer_mark
#   trainer     u8 present; a model
#   candidate   u8 present; a generation
#   serving     u8 present; a generation
#   requests    u64 next request number, u32 count, the requests
#   counters    u64 requests, accepted, audited, abstained, full, window,
#               f64 coverage, u64 observations, learned, ignored, missing,
#               dropped, promotions, revocations, retired
#
#   model       u64 updates, V x 8192 x f32 weights, V x f32 biases
#   generation  u64 gen, i64 frozen, i64 promoted, u64 samples, errors,
#               missed, audits, audit_errors, budget, a model (classifier),
#               R x 32 x region (4 x f64 log wealth, u64 n, errors, proven
#               at), R x 32 x u64 paired, R x drift detector (u64 asked,
#               u64 errors, 3 x u32 wrong, 3 x u32 lost)
#   request     u64 seq, i64 created, u32 flags, i32 candidate label,
#               i32 serving label, f64 candidate score, f64 serving score,
#               u64 candidate generation, u64 serving generation,
#               i32 result, u64 spot, u32 n, n x u32 bucket, n x f32 value
#
# gp_blob_fields walks a serialized gate by that layout and returns where
# each field is: a dict, name -> {offset type}. It fails unless the walk
# ends where the gate ends. Types: c u8, s u16, i u32, l i32, w u64 (and
# i64), q f64, or a number of bytes. Names: the words above for the config
# (labels: label.0 ...; classalpha.0 ...; defer.0 ...; snapshot, timeout),
# the ledger and "next" and "count"; trainer.updates; cand.gen ...
# cand.budget, cand.updates, cand.region.<row>.<threshold>.<logw0..3 | n |
# errors | proven>, cand.paired.<row>.<threshold>, cand.drift.<row>.<asked |
# errors | wrong0..2 | lost0..2>, and the same with serv; req.<i>.<seq |
# created | flags | cand_label | serv_label | cand_score | serv_score |
# cand_gen | serv_gen | result | spot | clues>; stats.<counter>.

proc gp_take {name type} {
    upvar 1 fields fields at at
    dict set fields $name [list $at $type]
    incr at [dict get {c 1 s 2 i 4 l 4 w 8 q 8} $type]
}

# The value of a field of a serialized gate.
proc gp_blob_get {blob fields name} {
    lassign [dict get $fields $name] at type
    switch $type {
        c { binary scan $blob @${at}cu value }
        s { binary scan $blob @${at}su value }
        i { binary scan $blob @${at}iu value }
        l { binary scan $blob @${at}i value }
        w { binary scan $blob @${at}wu value }
        q { binary scan $blob @${at}q value }
        default { set value [string range $blob $at [expr {$at + $type - 1}]] }
    }
    return $value
}

# The serialized gate with other values for some of its fields (name value
# ...; for a field of bytes, as many bytes as it has).
proc gp_blob_set {blob fields args} {
    foreach {name value} $args {
        lassign [dict get $fields $name] at type
        switch $type {
            c { set bytes [binary format c $value] }
            s { set bytes [binary format s $value] }
            i - l { set bytes [binary format i $value] }
            w { set bytes [binary format w $value] }
            q { set bytes [binary format q $value] }
            default {
                if {[string length $value] != $type} { fail "gp_blob_set: $name takes $type bytes" }
                set bytes $value
            }
        }
        set blob [string replace $blob $at [expr {$at + [string length $bytes] - 1}] $bytes]
    }
    return $blob
}

proc gp_blob_fields {blob} {
    set fields {}
    if {[string range $blob 0 3] ne "GATE"} { fail "gp_blob_fields: not a serialized gate" }
    set at 4
    foreach {name type} {codec s features s kind c alpha q delta q audit q classes i} { gp_take $name $type }
    set k [gp_blob_get $blob $fields classes]
    if {$k > 64} { fail "gp_blob_fields: $k classes" }
    for {set i 0} {$i < $k} {incr i} {
        gp_take label.$i.length i
        set n [gp_blob_get $blob $fields label.$i.length]
        dict set fields label.$i [list $at $n]
        incr at $n
    }
    for {set i 0} {$i < $k} {incr i} { gp_take classalpha.$i q }
    for {set i 0} {$i < $k} {incr i} { gp_take defer.$i c }
    foreach {name type} {snapshot i timeout w
                         lineage w epoch w created w clock w generations w paused c
                         snapshot_updates w blind c missing_run i answer_mark w trainer c} {
        gp_take $name $type
    }
    set rows [expr {$k ? $k : 1}]
    set weights [expr {($k == 2 ? 1 : $k) * (8192 + 1) * 4}]
    if {[gp_blob_get $blob $fields trainer]} {
        gp_take trainer.updates w
        dict set fields trainer.weights [list $at $weights]
        incr at $weights
    }
    foreach g {cand serv} {
        gp_take $g c
        if {![gp_blob_get $blob $fields $g]} continue
        foreach name {gen frozen promoted samples errors missed audits audit_errors budget} { gp_take $g.$name w }
        if {$k} {
            gp_take $g.updates w
            dict set fields $g.weights [list $at $weights]
            incr at $weights
        }
        for {set row 0} {$row < $rows} {incr row} {
            for {set t 0} {$t < 32} {incr t} {
                foreach {name type} {logw0 q logw1 q logw2 q logw3 q n w errors w proven w} {
                    gp_take $g.region.$row.$t.$name $type
                }
            }
        }
        for {set row 0} {$row < $rows} {incr row} {
            for {set t 0} {$t < 32} {incr t} { gp_take $g.paired.$row.$t w }
        }
        for {set row 0} {$row < $rows} {incr row} {
            foreach {name type} {asked w errors w wrong0 i wrong1 i wrong2 i lost0 i lost1 i lost2 i} {
                gp_take $g.drift.$row.$name $type
            }
        }
    }
    gp_take next w
    gp_take count i
    set count [gp_blob_get $blob $fields count]
    if {$count > 4096 + 16384} { fail "gp_blob_fields: $count requests" }
    for {set i 0} {$i < $count} {incr i} {
        foreach {name type} {seq w created w flags i cand_label l serv_label l cand_score q serv_score q
                             cand_gen w serv_gen w result l spot w clues i} {
            gp_take req.$i.$name $type
        }
        incr at [expr {8 * [gp_blob_get $blob $fields req.$i.clues]}]
    }
    foreach name {requests accepted audited abstained full window} { gp_take stats.$name w }
    gp_take stats.coverage q
    foreach name {observations learned ignored missing dropped promotions revocations retired} {
        gp_take stats.$name w
    }
    if {$at != [string length $blob]} {
        fail "gp_blob_fields: the layout of gate_codec.c ends at byte $at of [string length $blob]"
    }
    return $fields
}

# The fields of a serialized gate that count something: every u64 but the
# two random numbers, the times (which have a range of their own) and what
# a request says of itself other than its number as a spot check.
proc gp_blob_counters {fields} {
    set counters {}
    dict for {name where} $fields {
        if {[lindex $where 1] ne "w"} continue
        if {$name in {timeout lineage epoch created clock}} continue
        if {[string match *.frozen $name] || [string match *.promoted $name]} continue
        if {[string match req.* $name] && ![string match req.*.spot $name]} continue
        lappend counters $name
    }
    return $counters
}

# The DUMP payload of a gate (taken with rdbcompression off) as a dict:
# head, blob (the serialized gate), version, fields (gp_blob_fields) and
# base (gp_dump_base: many payloads are made of one).
proc gp_payload_open {payload} {
    lassign [gp_dump_split $payload] head blob version
    dict create head $head blob $blob version $version fields [gp_blob_fields $blob] \
        base [gp_dump_base $head $blob]
}

# The value of a field of an opened payload.
proc gp_payload_get {opened name} {
    gp_blob_get [dict get $opened blob] [dict get $opened fields] $name
}

# The payload for that gate with other values for some of its fields (name
# value ...), with a valid checksum.
proc gp_payload_with {opened args} {
    set fields [dict get $opened fields]
    set from [string length [dict get $opened blob]]
    foreach {name value} $args { set from [expr {min($from, [lindex [dict get $fields $name] 0])}] }
    gp_dump_damage [dict get $opened base] [gp_blob_set [dict get $opened blob] $fields {*}$args] \
        [dict get $opened version] $from
}

# What every GINFO reply must be, whatever became of the gate: the fields of
# SPEC.md, a state and a kind that exist, a configuration GCREATE could
# have made. (That no counter is negative is the subject of a test of its
# own.)
proc gp_check_info {what info} {
    foreach field {kind state alpha delta audit classes alphas thresholds generation candidate learned
                   requests accepted audited abstained coverage audit-samples audit-errors
                   calibration-samples pending missing revocations memory} {
        if {![dict exists $info $field]} { fail "$what: GINFO has no $field: $info" }
    }
    set kind [dict get $info kind]
    if {$kind ni {classifier score}} { fail "$what: kind '$kind'" }
    if {[dict get $info state] ni {COLD CALIBRATING ACTIVE PAUSED}} { fail "$what: state '[dict get $info state]'" }
    foreach {field low high} {alpha 0 1 delta 0 1 audit 0 1.0000001} {
        set value [dict get $info $field]
        if {!($value > $low && $value < $high)} { fail "$what: $field is $value" }
    }
    set classes [llength [dict get $info classes]]
    if {$kind eq "score" ? $classes != 0 : ($classes < 2 || $classes > 64)} {
        fail "$what: a $kind gate with $classes classes"
    }
    set rows [expr {$kind eq "score" ? 1 : $classes}]
    if {[llength [dict get $info alphas]] != $rows || [llength [dict get $info thresholds]] != $rows} {
        fail "$what: $rows labels, but alphas [dict get $info alphas] and thresholds [dict get $info thresholds]"
    }
    foreach field {generation candidate pending memory} {
        set value [dict get $info $field]
        if {![string is wide -strict $value] || $value < 0} { fail "$what: $field is $value" }
    }
    if {[dict get $info pending] > 4096} { fail "$what: [dict get $info pending] requests wait" }
    set coverage [dict get $info coverage]
    if {!($coverage >= 0 && $coverage <= 1)} { fail "$what: coverage is $coverage" }
}

# A GPREDICT reply: an action and its argument.
proc gp_check_reply {what reply} {
    if {[llength $reply] != 2 || [lindex $reply 0] ni {ACCEPT ABSTAIN AUDIT}} {
        fail "$what: GPREDICT replied '$reply'"
    }
    if {[lindex $reply 0] eq "AUDIT" && [lindex $reply 1] eq {}} { fail "$what: AUDIT without an id" }
}

# Request number $n for a gate that came from a damaged payload: what
# gp_request gives, with an answer the gate knows (its labels may be damaged
# too). $info is its GINFO.
proc gp_damaged_request {info n} {
    lassign [gp_request [dict get $info kind] $n] question answer
    if {[dict get $info kind] eq "classifier"} {
        set classes [dict get $info classes]
        set answer [list LABEL [lindex $classes [expr {$n % [llength $classes]}]]]
    }
    list $question $answer
}

# RESTORE a damaged payload into gp:damaged. It must be refused with an
# error that leaves no key behind. If the damage happens to leave a
# well-formed gate, the gate must be a gate: one that came back without
# proof and can be used (asked, answered, taught, paused, reset), dumped and
# restored again, and deleted. Returns "refused" or "accepted".
#
# The settings of such a gate may be damaged too: a feedback timeout of one
# millisecond, an error rate of nearly 1. So only the first request is sure
# not to be answered by the gate, and feedback may come too late.
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
    gp_check_info $what $info
    set kind [dict get $info kind]
    # It came back without proof ...
    if {[dict get $info generation] != 0 || [dict get $info pending] != 0 ||
        [dict get $info state] eq "ACTIVE"} {
        fail "$what: restored with proof: $info"
    }
    foreach threshold [dict get $info thresholds] {
        if {$threshold ne {}} { fail "$what: restored with a threshold: $info" }
    }
    # ... and works: it takes requests, feedback and examples.
    for {set i 0} {$i < 3} {incr i} {
        lassign [gp_damaged_request $info $i] question answer
        set reply [r gpredict gp:damaged {*}$question]
        gp_check_reply $what $reply
        lassign $reply action id
        if {$i == 0 && $action ne "ABSTAIN"} { fail "$what: replied $action right after RESTORE" }
        if {$action ne "ACCEPT" && $id ne {}} {
            set used [r glearn gp:damaged $id {*}$answer]
            if {$used ni {0 1}} { fail "$what: GLEARN replied '$used'" }
            if {[r glearn gp:damaged $id {*}$answer] != 0} { fail "$what: feedback was used twice" }
        }
    }
    if {$kind eq "classifier"} {
        r gadd gp:damaged TEXT "an example" LABEL [lindex [dict get $info classes] 0]
    }
    r gpause gp:damaged
    if {[gp_info gp:damaged state] ne "PAUSED"} { fail "$what: GPAUSE did not pause it" }
    r gresume gp:damaged
    gp_check_info "$what, after it was used" [gp_info gp:damaged]
    # Whatever it is now, Redis must be able to save it and load it again.
    if {[catch {r restore gp:again 0 [r dump gp:damaged]} reply]} {
        fail "$what: after RESTORE and a few commands the gate cannot be restored from its own DUMP: $reply"
    }
    # A reset leaves the gate GCREATE would have made.
    r greset gp:damaged
    set info [gp_info gp:damaged]
    gp_check_info "$what, after GRESET" $info
    foreach field {learned generation pending requests missing revocations} {
        if {[dict get $info $field] != 0} { fail "$what: after GRESET $field is [dict get $info $field]" }
    }
    gp_check_reply "$what, after GRESET" [r gpredict gp:damaged {*}[lindex [gp_request $kind 1] 0]]
    if {[catch {r restore gp:again 0 [r dump gp:damaged] replace} reply]} {
        fail "$what: after RESTORE and GRESET the gate cannot be restored from its own DUMP: $reply"
    }
    if {[r del gp:damaged gp:again] != 2} { fail "$what: the gate and its copy could not be deleted" }
    return accepted
}

# The checksum of a payload is linear: one flipped bit changes it by a value
# that depends only on the bit and on how many bytes follow it. Given the
# change a flip makes, gp_crc64_zero says what the same flip makes one byte
# earlier.
proc gp_crc64_zero {delta} {
    expr {[lindex $::gp_crc64_table [expr {$delta & 0xff}]] ^ (($delta >> 8) & 0x00ffffffffffffff)}
}

# Run $body for every payload that differs from $payload in one bit of the
# last $bytes bytes of its serialized gate, with a valid checksum. $whatvar
# names the variable that says which bit it is, $payloadvar the one that
# holds the payload.
proc gp_each_bit {payload bytes whatvar payloadvar body} {
    upvar 1 $whatvar what $payloadvar damaged
    gp_crc64 ""
    lassign [gp_dump_split $payload] head blob version
    set length [string length $blob]
    set from [expr {max(0, $length - $bytes)}]
    set prefix $head
    append prefix [gp_rdb_length $length]
    binary scan [string range $payload end-7 end] w crc
    # What a flipped bit of the last byte of the gate does to the checksum:
    # three bytes follow it.
    set deltas {}
    for {set bit 0} {$bit < 8} {incr bit} {
        set delta [lindex $::gp_crc64_table [expr {1 << $bit}]]
        for {set k 0} {$k < 3} {incr k} { set delta [gp_crc64_zero $delta] }
        lappend deltas $delta
    }
    for {set at [expr {$length - 1}]} {$at >= $from} {incr at -1} {
        set old [scan [string index $blob $at] %c]
        set earlier {}
        for {set bit 0} {$bit < 8} {incr bit} {
            set delta [lindex $deltas $bit]
            lappend earlier [gp_crc64_zero $delta]
            set what "bit $bit of byte $at"
            set damaged $prefix
            append damaged [string replace $blob $at $at [binary format c [expr {$old ^ (1 << $bit)}]]] \
                "\x00" $version [binary format w [expr {$crc ^ $delta}]]
            uplevel 1 $body
        }
        set deltas $earlier
    }
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

# ----------------------- One snapshot behind another -----------------------
#
# A newer snapshot takes over when it has proven thresholds of its own and
# answers more of the same spot-checked requests than the one that answers
# now (SPEC.md section 3; CHANGES.md 9a for how that is counted). The
# classifiers of gp_behind get there in a way a test can count. They are
# taught spam and a little ham and then asked three kinds of text (gp_text):
# spam, ham and "odd". With AUDIT 1 every request goes to the teacher and
# nothing depends on a random draw.
#
#   - The first snapshot is taken before the gate has seen an odd text. It
#     takes one for spam, without being sure, and is wrong: it proves itself
#     for ham, and for spam only above the confidence it has in odd texts.
#     So it answers spam and ham and leaves odd texts to the teacher.
#   - The second snapshot is taken when the first starts to answer. By then
#     the model has learned from the teacher that odd texts are ham.
#
# From then on every request is proof for the second snapshot (and a spot
# check of the first where the first would answer). The second has proven a
# label after gp_need 2 2 right guesses of it, and takes over once the
# proof inside what it has proven outnumbers the requests among its proof
# that the first one answers: the spam and the ham.
#
# gp_behind returns a dict that keeps count: key; old, every request asked
# so far as {id answer} pairs; waiting, the requests not answered yet, as
# {id answer topic}; spam, ham and odd, how many answered requests of each
# topic are proof for the second snapshot; and audit.spam and audit.ham,
# how many spot checks the first snapshot was asked for each label.
proc gp_behind {key {timeout 60000}} {
    gp_classifier $key 20 1 $timeout
    for {set i 0} {$i < 20} {incr i} {
        lassign [gp_text [expr {$i < 2 ? "ham" : "spam"}] [incr ::gp_n]] question answer
        r gadd $key {*}$question {*}$answer
    }
    set b [dict create key $key old {} waiting {} spam 0 ham 0 odd 0 audit.spam 0 audit.ham 0]
    for {set n 0} {$n < 300} {incr n} {
        set info [gp_info $key]
        lassign [dict get $info thresholds] spam ham
        if {[dict get $info generation] == 1 && $spam ne {} && $ham ne {}} break
        set topic [lindex {spam ham odd} [expr {$n % 3}]]
        lassign [gp_text $topic [incr ::gp_n]] question answer
        lassign [r gpredict $key {*}$question] action id
        if {$id eq {}} { fail "$key did not ask for feedback: $action" }
        assert_equal 1 [r glearn $key $id {*}$answer] "feedback for a fresh id on $key"
        dict lappend b old [list $id $answer]
        if {$action eq "AUDIT"} { dict incr b audit.$topic }
        # Asked while the second snapshot was being tested: proof for it.
        if {[dict get $info candidate] == 2} { dict incr b $topic }
    }
    set info [gp_info $key]
    lassign [dict get $info thresholds] spam ham
    if {[dict get $info generation] != 1 || [dict get $info candidate] != 2 || [dict get $info revocations] != 0 ||
        $spam eq {} || $ham eq {} || $spam <= $ham} {
        fail "the first snapshot of $key does not answer ham, and spam it is sure of: the scenario of gp_behind needs new texts: $info"
    }
    return $b
}

# 1 if, by the count, the second snapshot must have taken over.
proc gp_behind_over {b} {
    set need [gp_need 2 2]
    set inside 0
    # What it guesses to be ham: ham and odd texts. To be spam: spam.
    set ham [expr {[dict get $b ham] + [dict get $b odd]}]
    if {$ham >= $need} { incr inside $ham }
    if {[dict get $b spam] >= $need} { incr inside [dict get $b spam] }
    expr {$inside > [dict get $b spam] + [dict get $b ham]}
}

# One more request about $topic, left unanswered.
proc gp_behind_ask {bvar topic} {
    upvar 1 $bvar b
    set key [dict get $b key]
    lassign [gp_text $topic [incr ::gp_n]] question answer
    lassign [r gpredict $key {*}$question] action id
    # What the first snapshot answers is a spot check (AUDIT 1).
    assert_equal [expr {$topic eq "odd" ? "ABSTAIN" : "AUDIT"}] $action "reply of $key to a text about $topic"
    if {$action eq "AUDIT"} { dict incr b audit.$topic }
    dict lappend b old [list $id $answer]
    dict lappend b waiting [list $id $answer $topic]
}

# The teacher's answer for the oldest request that waits. The snapshot that
# answers afterwards must be the one the count says.
proc gp_behind_answer {bvar} {
    upvar 1 $bvar b
    set key [dict get $b key]
    lassign [lindex [dict get $b waiting] 0] id answer topic
    dict set b waiting [lrange [dict get $b waiting] 1 end]
    assert_equal 1 [r glearn $key $id {*}$answer] "feedback for $key"
    dict incr b $topic
    set generation [gp_info $key generation]
    if {$generation != [expr {[gp_behind_over $b] ? 2 : 1}]} {
        fail "$key: snapshot $generation answers after proof for the second of [dict get $b spam] spam, [dict get $b ham] ham and [dict get $b odd] odd: [gp_info $key]"
    }
    return $generation
}

# The classifier of gp_behind with the takeover undecided: 30 times spam
# and ham and ten odd texts were proof for its second snapshot, three more
# odd texts wait. The second snapshot has not proven a label yet. Returns
# the count gp_behind keeps.
proc gp_behind_undecided {key} {
    set b [gp_behind $key]
    for {set i 0} {$i < 30} {incr i} {
        foreach topic {spam ham} {
            gp_behind_ask b $topic
            gp_behind_answer b
        }
    }
    for {set i 0} {$i < 10} {incr i} {
        gp_behind_ask b odd
        gp_behind_answer b
    }
    for {set i 0} {$i < 3} {incr i} { gp_behind_ask b odd }
    return $b
}

# A classifier whose answering snapshot took over from an older one: the
# classifier of gp_behind, asked odd texts until the second snapshot has
# proven ham and answers more than the first. Returns the key.
proc gp_takeover {key} {
    set b [gp_behind $key]
    for {set n 0} {$n < 100 && [gp_info $key generation] == 1} {incr n} {
        gp_behind_ask b odd
        gp_behind_answer b
    }
    set info [gp_info $key]
    if {[dict get $info generation] != 2 || [dict get $info revocations] != 0} {
        fail "no snapshot took over from an older one: $info"
    }
    return $key
}

# ------------------- A snapshot that leaves all to the teacher -------------
#
# A classifier with three labels, one of which it must never answer with
# (ALWAYSDEFER), whose first snapshot knows nothing but that label. Such a
# snapshot proves nothing, however long it is tested, and must not be tested
# for ever: the requests it leaves to the teacher although the teacher's
# label is one the gate may answer use up its budget as proof would (SPEC.md
# section 5, "snapshot dropped": 10 times the proof a flawless threshold
# needs, per label the gate may answer).
proc gp_deferring {key {timeout 60000}} {
    r config set gate.snapshot-examples 10
    r config set gate.feedback-timeout $timeout
    assert_equal OK [r gcreate $key ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 3 spam ham other ALWAYSDEFER 1 other]
    gp_defaults
    for {set i 0} {$i < 10} {incr i} {
        r gadd $key TEXT "weather forecast rain tomorrow [incr ::gp_n]" LABEL other
    }
    assert_equal CALIBRATING [gp_info $key state] "state of $key"
    assert_equal 1 [gp_info $key candidate] "candidate of $key"
}

# How many such requests the first snapshot of gp_deferring is tested on
# before it is dropped.
proc gp_deferring_budget {} {
    expr {10 * [gp_need 1 2] * 2}
}

# Ask the gate of gp_deferring $count requests of the kind its snapshot
# takes for the label it must not answer with. Returns them as {id answer}
# pairs: the teacher says spam.
proc gp_deferring_ask {key count} {
    set asked {}
    for {set i 0} {$i < $count} {incr i} {
        lassign [r gpredict $key TEXT "weather forecast rain tomorrow [incr ::gp_n]"] action id
        if {$action ne "ABSTAIN" || $id eq {}} { fail "$key did not leave the request to the teacher: $action $id" }
        lappend asked [list $id {LABEL spam}]
    }
    return $asked
}

# ------------------ The states late and lost answers lead to ---------------
#
# Proof is counted in request order, so an answer waits for the answers of
# older requests; not for long: after a quarter of the feedback timeout
# proof moves on without the unanswered request and counts it as a wrong
# guess until its answer arrives. After the feedback timeout the answer is
# lost. When 32 requests in a row end without an answer and no answer of any
# request has arrived since the first of them was asked, the gate stops
# answering and testing until an answer arrives again. An answering gate
# numbers its spot checks in the order they are asked and counts, for the
# blocks they fall in, those that disagreed and those whose answer was lost.
#
# gp_states builds twenty-three small gates in the states this leads to,
# for score gates and classifiers, being tested and answering, and paused:
#
#   wait    two answers wait behind an unanswered request;
#   prov    proof has moved on without a request: it counts as a wrong guess
#           and still waits for its answer;
#   lost    five answers were lost: not enough for the gate to stop;
#   mark    seven answers were lost, four of requests asked before the one
#           answer that did arrive and three of requests asked after it:
#           the three are a run under way, the four are not;
#   blind   32 answers in a row were lost: the gate has stopped;
#   res     the first answer after that has arrived: the gate has just
#           resumed, with the next snapshot number, and requests that were
#           asked while it had stopped still wait;
#   spot    an answering gate and ten spot checks of it (AUDIT 1): two
#           disagreed, four were lost, two were answered and wait behind
#           one that was not;
#   behind  a snapshot that is tested behind the one that answers, the
#           takeover undecided (gp_behind), three of its proof requests
#           waiting;
#   defer   a snapshot that leaves everything to the teacher, some of its
#           budget used up (gp_deferring).
#
# Returns a dict: key -> a dict with
#
#   state   one of the words above;
#   used    the largest snapshot number the gate has used;
#   old     the requests asked so far, as {id answer} pairs (for spot checks
#           a third element: an answer that disagrees). Whatever became of
#           them, none may be used once the gate has come back;
#   late    (prov) the request proof moved on without;
#   behind  (behind) the count gp_behind keeps.
#
# The two halves are for a caller that has more to do before and after the
# one sleep this takes.

# Send requests until $count wait for feedback, and answer those at the
# positions $answered at once, out of order: the others stay unanswered.
# Returns all of them as {id answer} pairs.
proc gp_some_answered {key count answered} {
    set waiting [gp_collect $key $count]
    foreach i $answered {
        assert_equal 1 [gp_answer $key [lindex $waiting $i]] "feedback for a fresh id on $key"
    }
    return $waiting
}

# Ask a gate that answers $count requests it would answer by itself. With
# AUDIT 1 each of them is a spot check: the reply is AUDIT (ABSTAIN while
# the gate is paused). Returns them as {id answer disagreeing-answer}.
proc gp_spots {key count} {
    set info [gp_info $key]
    set kind [dict get $info kind]
    set expected [expr {[dict get $info state] eq "PAUSED" ? "ABSTAIN" : "AUDIT"}]
    set spots {}
    for {set i 0} {$i < $count} {incr i} {
        if {$kind eq "score"} {
            set question {SCORE 0.9}
            set answer {CORRECT 1}
            set wrong {CORRECT 0}
        } else {
            lassign [gp_request classifier [incr ::gp_n]] question answer
            set wrong [list LABEL [expr {[lindex $answer 1] eq "spam" ? "ham" : "spam"}]]
        }
        lassign [r gpredict $key {*}$question] action id
        if {$action ne $expected || $id eq {}} {
            fail "$key: expected a spot check ($expected and an id) for $question, got $action $id: [gp_info $key]"
        }
        lappend spots [list $id $answer $wrong]
    }
    return $spots
}

# An answer for one {id answer disagreeing-answer} of gp_spots that
# disagrees with the gate. Returns the reply.
proc gp_disagree {key spot} {
    r glearn $key [lindex $spot 0] {*}[lindex $spot 2]
}

proc gp_states_ask {p} {
    # Name, kind, whether it answers already, whether it is paused, and its
    # feedback timeout. With AUDIT 1 a gate that answers sends every request
    # to the teacher: all of them are proof, and those it would have
    # answered by itself are spot checks.
    set plan [list \
        wait:s   score      0 0 60000 \
        wait:c   classifier 0 0 60000 \
        wait:a   score      1 0 60000 \
        wait:p   score      0 1 60000 \
        prov:s   score      0 0 $::gp_late \
        prov:c   classifier 0 0 $::gp_late \
        prov:a   score      1 0 $::gp_late \
        prov:p   score      0 1 $::gp_late \
        lost:s   score      0 0 $::gp_lost \
        mark:s   score      0 0 $::gp_lost \
        blind:s  score      0 0 $::gp_lost \
        blind:c  classifier 0 0 $::gp_lost \
        blind:a  score      1 0 $::gp_lost \
        blind:ca classifier 1 0 $::gp_lost \
        blind:p  score      1 1 $::gp_lost \
        res:s    score      0 0 $::gp_lost \
        res:c    classifier 0 0 $::gp_lost \
        res:p    score      0 1 $::gp_lost \
        spot:a   score      1 0 $::gp_lost \
        spot:c   classifier 1 0 $::gp_lost \
        spot:p   score      1 1 $::gp_lost]
    set z {}
    foreach {name kind answering paused timeout} $plan {
        set key $p:$name
        if {$kind eq "score"} {
            gp_score $key [expr {$answering ? 1 : 0.2}] $timeout
        } else {
            set step [expr {$answering ? 100 : 20}]
            gp_classifier $key $step [expr {$answering ? 1 : 0.2}] $timeout
            gp_teach $key $step
        }
        if {$paused} { assert_equal OK [r gpause $key] }
        if {$answering} { gp_prove $key }
        set before [gp_info $key]
        assert_equal [expr {$paused ? "PAUSED" : $answering ? "ACTIVE" : "CALIBRATING"}] [dict get $before state] "state of $key"
        switch -glob $name {
            wait:* { set asked [gp_some_answered $key 5 {1 3}] }
            prov:* { set asked [gp_some_answered $key 3 {1}] }
            lost:* { set asked [gp_collect $key 5] }
            mark:* {
                # Five requests, the last of them answered; three more.
                set asked [gp_some_answered $key 5 {4}]
                lappend asked {*}[gp_collect $key 3]
            }
            spot:* {
                # Six spot checks. The second is answered and disagrees, the
                # fourth is answered and agrees: the detector hears of an
                # answer the moment it arrives.
                assert_equal 0 [dict get $before candidate] "candidate of $key"
                set asked [gp_spots $key 6]
                assert_equal 1 [gp_disagree $key [lindex $asked 1]] "feedback for a fresh id on $key"
                assert_equal 1 [gp_answer $key [lindex $asked 3]] "feedback for a fresh id on $key"
                set info [gp_info $key]
                assert_equal 2 [dict get $info audit-samples] "audit-samples of $key"
                assert_equal 1 [dict get $info audit-errors] "audit-errors of $key"
                assert_equal 4 [dict get $info pending] "pending of $key"
            }
            blind:a - blind:p {
                # Requests below the threshold these gates answer at: proof
                # (with AUDIT 1 every request is), but no spot checks.
                set asked {}
                foreach id [gp_right $key 32 0.3] { lappend asked [list $id {CORRECT 0}] }
            }
            default { set asked [gp_collect $key 32] }
        }
        # An answer does not overtake the answer of an older request:
        # nothing was counted yet.
        assert_equal [dict get $before calibration-samples] [gp_info $key calibration-samples] "proof of $key"
        dict set z $key [dict create state [lindex [split $name :] 0] kind $kind answering $answering \
            used [gp_max_number $key] old $asked samples [dict get $before calibration-samples]]
    }
    # A snapshot tested behind the one that answers, the takeover undecided.
    set key $p:behind:c
    set b [gp_behind_undecided $key]
    dict set z $key [dict create state behind kind classifier answering 1 used 2 old [dict get $b old] behind $b]
    # A snapshot that leaves everything to the teacher: ten such requests
    # were answered, two wait.
    set key $p:defer:c
    gp_deferring $key
    set asked [gp_deferring_ask $key 12]
    for {set i 0} {$i < 10} {incr i} {
        assert_equal 1 [gp_answer $key [lindex $asked $i]] "feedback for a fresh id on $key"
    }
    dict set z $key [dict create state defer kind classifier answering 0 used 1 old $asked]
    return $z
}

# The second half, at least ::gp_wait milliseconds after the first.
proc gp_states_settle {z} {
    dict for {key meta} $z {
        set old [dict get $meta old]
        set state [dict get $meta state]
        switch $state {
            wait {
                # Their feedback timeout is a minute: the answers still
                # wait. Three of five requests are unanswered.
                assert_equal 3 [gp_info $key pending] "pending of $key"
            }
            prov {
                # The answer of the youngest request arrives. The oldest is
                # still unanswered and a quarter of the feedback timeout has
                # passed: proof moves on without it.
                assert_equal 1 [gp_answer $key [lindex $old 2]] "feedback for $key"
                set info [gp_info $key]
                if {[dict get $info missing] != 0} {
                    fail "$key: the machine stood still for seconds, this test cannot work: $info"
                }
                assert_equal 1 [dict get $info pending] "pending of $key"
                assert_equal [expr {[dict get $meta samples] + 3}] [dict get $info calibration-samples] "proof of $key"
                dict set z $key late [lindex $old 0]
            }
            lost {
                # Feedback that comes too late is not used. The same goes
                # for the four other requests.
                assert_equal 0 [gp_answer $key [lindex $old 0]] "late feedback for $key"
                set info [gp_info $key]
                assert_equal 5 [dict get $info missing] "missing of $key"
                assert_equal 0 [dict get $info pending] "pending of $key"
                assert_equal CALIBRATING [dict get $info state] "state of $key"
                assert_equal 5 [dict get $info calibration-samples] "proof of $key"
            }
            mark {
                # Seven lost answers and the one that was given. (That only
                # three of the seven are a run is in what the gate saves:
                # see the test of its ledger.)
                assert_equal 0 [gp_answer $key [lindex $old 0]] "late feedback for $key"
                set info [gp_info $key]
                assert_equal 7 [dict get $info missing] "missing of $key"
                assert_equal 0 [dict get $info pending] "pending of $key"
                assert_equal CALIBRATING [dict get $info state] "state of $key"
                assert_equal 8 [dict get $info calibration-samples] "proof of $key"
                assert_equal [dict get $meta used] [dict get $info candidate] "candidate of $key"
                # Two requests wait.
                dict set z $key old [concat $old [gp_collect $key 2]]
            }
            blind - res {
                assert_equal 0 [gp_answer $key [lindex $old 0]] "late feedback for $key"
                set info [gp_info $key]
                assert_equal 32 [dict get $info missing] "missing of $key"
                assert_equal 0 [dict get $info pending] "pending of $key"
                # It has stopped: no snapshot answers and none is tested.
                assert_equal 0 [dict get $info generation] "generation of $key"
                assert_equal 0 [dict get $info candidate] "candidate of $key"
                assert {[dict get $info state] in {COLD PAUSED}}
                assert_equal [dict get $meta answering] [dict get $info revocations] "revocations of $key"
                foreach threshold [dict get $info thresholds] { assert_equal {} $threshold }
            }
            spot {
                # Four more spot checks. The first of them finds that the
                # four that were not answered are lost: each is a wrong
                # guess for good, and the two answers that waited behind
                # them are counted.
                set more [gp_spots $key 4]
                set info [gp_info $key]
                assert_equal 4 [dict get $info missing] "missing of $key"
                assert_equal 4 [dict get $info pending] "pending of $key"
                assert_equal [expr {[dict get $meta samples] + 6}] [dict get $info calibration-samples] "proof of $key"
                # The second disagrees, the third agrees; they wait for the
                # first.
                assert_equal 1 [gp_disagree $key [lindex $more 1]] "feedback for a fresh id on $key"
                assert_equal 1 [gp_answer $key [lindex $more 2]] "feedback for a fresh id on $key"
                set info [gp_info $key]
                assert_equal 4 [dict get $info audit-samples] "audit-samples of $key"
                assert_equal 2 [dict get $info audit-errors] "audit-errors of $key"
                assert_equal 4 [dict get $info missing] "missing of $key"
                assert_equal 2 [dict get $info pending] "pending of $key"
                assert_equal [expr {[dict get $meta samples] + 6}] [dict get $info calibration-samples] "proof of $key"
                # Two disagreements and four lost answers among ten spot
                # checks (five of each label for the classifier) are far
                # from what stops a gate.
                assert_equal 0 [dict get $info revocations] "revocations of $key"
                assert_equal [dict get $meta used] [dict get $info generation] "generation of $key"
                assert_equal 0 [dict get $info candidate] "candidate of $key"
                dict set z $key old [concat $old $more]
                dict set z $key spots $more
            }
            behind {
                set b [dict get $meta behind]
                set info [gp_info $key]
                assert_equal ACTIVE [dict get $info state] "state of $key"
                assert_equal 1 [dict get $info generation] "generation of $key"
                assert_equal 2 [dict get $info candidate] "candidate of $key"
                assert_equal 3 [dict get $info pending] "pending of $key"
                assert_equal [expr {[dict get $b spam] + [dict get $b ham] + [dict get $b odd]}] \
                    [dict get $info calibration-samples] "proof of the second snapshot of $key"
            }
            defer {
                set info [gp_info $key]
                assert_equal CALIBRATING [dict get $info state] "state of $key"
                assert_equal 1 [dict get $info candidate] "candidate of $key"
                assert_equal 2 [dict get $info pending] "pending of $key"
                assert_equal 20 [dict get $info learned] "learned of $key"
                # Nothing it was asked is proof.
                assert_equal 0 [dict get $info calibration-samples] "proof of $key"
            }
        }
        if {$state eq "res"} {
            # While it has stopped every request gets an id, and the first
            # answer starts the next snapshot.
            set asked [gp_collect $key 3]
            assert_equal 0 [gp_info $key candidate]
            assert_equal 1 [gp_answer $key [lindex $asked 1]] "feedback for $key"
            set info [gp_info $key]
            assert_equal [expr {[dict get $meta used] + 1}] [dict get $info candidate] "candidate of $key"
            assert_equal 0 [dict get $info generation] "generation of $key"
            assert_equal 2 [dict get $info pending] "pending of $key"
            assert_equal 0 [dict get $info calibration-samples] "proof of $key"
            dict set z $key used [dict get $info candidate]
            dict set z $key old [concat $old $asked]
        }
    }
    return $z
}

proc gp_states {p} {
    set z [gp_states_ask $p]
    after $::gp_wait
    gp_states_settle $z
}

# A gate of gp_states that came back (from an RDB file, the AOF or a DUMP
# payload) and was not written to since. Its first write makes it drop its
# proof and its waiting requests, which GINFO was showing already; then it
# proves itself again, with a snapshot number it has not used before. $meta
# is what gp_states returned for the gate (for a copy: for its original).
proc gp_recovers {key meta} {
    set shown [gp_info $key]
    # The first write: feedback for the requests from before. None is used.
    foreach pair [dict get $meta old] {
        assert_equal 0 [gp_answer $key $pair] "feedback for a request $key was asked before it came back"
    }
    set now [gp_info $key]
    foreach info {shown now} { dict unset $info memory }
    assert_equal $shown $now "GINFO of $key before and after its first write"
    assert_equal 0 [dict get $now generation] "generation of $key"
    assert_equal 0 [dict get $now pending] "pending of $key"
    # Every gate of gp_states has what it takes for a snapshot.
    set used [dict get $meta used]
    assert {[dict get $now candidate] > $used}
    set paused [expr {[dict get $now state] eq "PAUSED"}]
    if {!$paused} { assert_equal CALIBRATING [dict get $now state] "state of $key" }
    # (The gate with a label it must never answer with has no threshold for
    # that label to prove: for it, this is all.)
    if {[dict get $meta state] eq "defer"} return
    for {set n 0} {![gp_proven $key]} {incr n} {
        if {$n >= 600} { fail "$key did not prove itself again in $n requests: [gp_info $key]" }
        set action [gp_ask $key]
        if {$paused && $action ne "ABSTAIN"} { fail "$key is paused and replied $action" }
    }
    if {$paused} {
        assert_equal PAUSED [gp_info $key state] "state of $key"
        assert_equal OK [r gresume $key]
    }
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    assert {[gp_info $key generation] > $used}
}

# ----------------------- A story in which the clock matters ----------------
#
# Sixteen gates and what late answers, lost answers and an outage do to
# them, told in four parts so that the caller can put the two sleeps, and a
# rewrite of the AOF, between them:
#
#   gp_story_ask      requests are asked, some are answered out of order;
#   gp_story_late     (::gp_wait later) answers arrive that find proof has
#                     moved on without an older request, and feedback that
#                     finds its request is lost; two gates have lost the
#                     answers of their spot checks and stop;
#   gp_story_answers  the late answers arrive after all, right and wrong;
#                     the gates that stopped get an answer and resume;
#   gp_story_lost     (::gp_wait2 later) what is still unanswered is lost.
#
# Six of the gates are there for what the final round of the core made a
# gate hold. What each of them does in the second half of the story follows
# from a count it holds at the end of the first, where a rewrite of the AOF
# may come in between: the spot checks of a block and how many of them
# disagreed (drift:s) or were lost (unch:s), and where one block ends and
# the next begins (block:s); the proof requests of a snapshot that the
# snapshot before it answers (behind:c); the requests a snapshot left to
# the teacher for nothing (defer:c); and the request number at the last
# answer (mark:s). A file that lost one of these counts gives another gate
# than the server holds.
#
# The score gates are only asked requests their cheap answer is right for,
# at one score, so the story can say exactly when each is proven.
#
# The story is a dict: the prefix of the keys, and for each key the ids (or
# {id answer} pairs) of its requests.

# Ask a score gate $count requests. Returns their ids.
proc gp_right {key count {score 0.9}} {
    set ids {}
    for {set i 0} {$i < $count} {incr i} {
        lassign [r gpredict $key SCORE $score] action id
        if {$action eq "ACCEPT" || $id eq {}} { fail "$key did not ask for feedback: $action $id" }
        lappend ids $id
    }
    return $ids
}

# Ask a score gate and answer "right" until $count answers were given or the
# gate answers by itself. Returns the number of answers.
proc gp_right_answers {key count} {
    for {set n 0} {$n < $count} {incr n} {
        lassign [r gpredict $key SCORE 0.9] action id
        if {$action eq "ACCEPT"} break
        assert_equal 1 [r glearn $key $id CORRECT 1] "feedback for a fresh id on $key"
    }
    return $n
}

# A score gate whose snapshot is being tested, and has $given right answers
# so far, gets more of them, all at one score. It must be proven by the last
# of the $need answers the snapshot needs, and not by the one before.
proc gp_proven_by {key need {given 0}} {
    set more [expr {$need - $given - 1}]
    assert_equal $more [gp_right_answers $key $more] "answers $key took"
    assert_equal 0 [gp_info $key generation] "the snapshot of $key that answers, one answer short of $need"
    assert_equal 1 [gp_right_answers $key 1]
    if {[gp_info $key generation] == 0} { fail "$key is not proven after $need right answers: [gp_info $key]" }
}

proc gp_story_keys {story} {
    set keys {}
    foreach name {flip:s wrong:s flip:c lost:s lost:c wait:s run:s out:s out:c tick:s
                  drift:s block:s unch:s behind:c defer:c mark:s} {
        lappend keys [dict get $story prefix]:$name
    }
    return $keys
}

proc gp_story_ask {p} {
    set story [dict create prefix $p]
    # The gates whose second half follows from a count (the ones that take
    # the most commands come first: nothing here depends on the clock).
    # A snapshot that leaves everything to the teacher: 400 requests of
    # that kind are answered, more than half of what it is tested on.
    set key $p:defer:c
    gp_deferring $key
    assert {[gp_deferring_budget] > 400}
    foreach pair [gp_deferring_ask $key 400] {
        assert_equal 1 [gp_answer $key $pair] "feedback for a fresh id on $key"
    }
    # (25 more for which the teacher, too, says the label the gate must
    # not answer with. The snapshot could not have been tested on them:
    # they use up nothing.)
    foreach pair [gp_deferring_ask $key 25] {
        assert_equal 1 [r glearn $key [lindex $pair 0] LABEL other] "feedback for a fresh id on $key"
    }
    assert_equal 1 [gp_info $key candidate] "candidate of $key"
    # A snapshot that is tested behind the one that answers.
    dict set story $p:behind:c [gp_behind_undecided $p:behind:c]
    # A gate that answers and gets worse (AUDIT 1: every request it would
    # answer is a spot check). Of twelve spot checks the first five agree,
    # the next four disagree, and three are not answered yet.
    set key $p:drift:s
    gp_score $key 1
    gp_proven_by $key [gp_need 1]
    set spots [gp_spots $key 12]
    for {set i 0} {$i < 5} {incr i} { assert_equal 1 [gp_answer $key [lindex $spots $i]] }
    for {set i 5} {$i < 9} {incr i} { assert_equal 1 [gp_disagree $key [lindex $spots $i]] }
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    assert_equal 9 [gp_info $key audit-samples] "audit-samples of $key"
    assert_equal 4 [gp_info $key audit-errors] "audit-errors of $key"
    dict set story $key [lrange $spots 9 end]
    # A gate whose spot checks fill a block of 64: 58 agree, five disagree,
    # and the last is not answered yet.
    set key $p:block:s
    gp_score $key 1
    gp_proven_by $key [gp_need 1]
    set spots [gp_spots $key 64]
    for {set i 0} {$i < 58} {incr i} { assert_equal 1 [gp_answer $key [lindex $spots $i]] }
    for {set i 58} {$i < 63} {incr i} { assert_equal 1 [gp_disagree $key [lindex $spots $i]] }
    assert {![gp_block_stops 64 5 1000]}
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    dict set story $key [lindex $spots 63]
    # A gate that answers and whose spot checks nobody answers: twelve.
    set key $p:unch:s
    gp_score $key 1 $::gp_lost
    gp_proven_by $key [gp_need 1]
    dict set story $key [gp_spots $key 12]
    # Answers that will still wait when the others have long moved on: this
    # gate waits a quarter of a minute.
    gp_score $p:wait:s
    set ids [gp_right $p:wait:s 4]
    assert_equal 1 [r glearn $p:wait:s [lindex $ids 1] CORRECT 1]
    assert_equal 1 [r glearn $p:wait:s [lindex $ids 2] CORRECT 1]
    dict set story $p:wait:s $ids
    # Twenty requests nobody will answer: 12 short of what stops a gate.
    gp_score $p:run:s 0.2 $::gp_lost
    dict set story $p:run:s [gp_right $p:run:s 20]
    # Two gates that answer, and whose spot checks nobody will answer: with
    # AUDIT 1 every request is one.
    gp_score $p:out:s 1 $::gp_lost
    gp_proven_by $p:out:s [gp_need 1]
    assert_equal ACTIVE [gp_info $p:out:s state]
    dict set story $p:out:s [gp_right $p:out:s 32]
    gp_classifier $p:out:c 100 1 $::gp_lost
    gp_teach $p:out:c 100
    gp_prove $p:out:c
    dict set story $p:out:c:used [gp_max_number $p:out:c]
    dict set story $p:out:c:learned [gp_info $p:out:c learned]
    dict set story $p:out:c [gp_collect $p:out:c 32]
    # 33 requests, the last but one answered: 31 of them were asked before
    # an answer arrived, one after it.
    set key $p:mark:s
    gp_score $key 0.2 $::gp_mid
    set ids [gp_right $key 32]
    assert_equal 1 [r glearn $key [lindex $ids 31] CORRECT 1]
    dict set story $key [concat $ids [gp_right $key 1]]
    # Four requests each, the second answered: it waits for the first. The
    # gates whose clock is the tightest come last (and first after the
    # sleep): those with the feedback timeout in the middle.
    foreach {name timeout} [list flip:s $::gp_late wrong:s $::gp_late lost:s $::gp_mid] {
        gp_score $p:$name 0.2 $timeout
        set ids [gp_right $p:$name 4]
        assert_equal 1 [r glearn $p:$name [lindex $ids 1] CORRECT 1]
        dict set story $p:$name $ids
    }
    foreach {name timeout} [list flip:c $::gp_late lost:c $::gp_mid] {
        gp_classifier $p:$name 20 0.2 $timeout
        gp_teach $p:$name 20
        dict set story $p:$name [gp_some_answered $p:$name 4 {1}]
    }
    # A gate that answers nearly everything by itself (one request in a
    # billion is a spot check) while requests from before still wait: 45
    # were asked, the proof took the answers of the first 35.
    assert_equal OK [r config set gate.feedback-timeout $::gp_mid]
    assert_equal OK [r gcreate $p:tick:s ALPHA 0.2 DELTA 0.2 AUDIT 0.000000001 SCORE]
    gp_defaults
    set ids [gp_right $p:tick:s 45]
    for {set i 0} {$i < [gp_need 1]} {incr i} {
        assert_equal CALIBRATING [gp_info $p:tick:s state]
        assert_equal 1 [r glearn $p:tick:s [lindex $ids $i] CORRECT 1]
    }
    assert_equal ACTIVE [gp_info $p:tick:s state]
    # The third of the ten that still wait is answered: it waits for two.
    assert_equal 1 [r glearn $p:tick:s [lindex $ids 37] CORRECT 1]
    assert_equal 9 [gp_info $p:tick:s pending]
    assert_equal 35 [gp_info $p:tick:s calibration-samples]
    dict set story $p:tick:s $ids
    return $story
}

proc gp_story_late {story} {
    set p [dict get $story prefix]
    # This gate answers by itself, which writes nothing about the request;
    # but proof moves on without the two requests that hold up an answer.
    set key $p:tick:s
    assert_equal {ACCEPT {}} [r gpredict $key SCORE 0.9]
    set info [gp_info $key]
    if {[dict get $info missing] != 0} {
        fail "$key: the machine stood still for a second, this test cannot work: $info"
    }
    assert_equal 9 [dict get $info pending] "pending of $key"
    assert_equal 38 [dict get $info calibration-samples] "proof of $key"
    assert_equal ACTIVE [dict get $info state] "state of $key"
    # The answer of the third request arrives. A quarter of the feedback
    # timeout has passed: proof moves on without the first, takes the
    # second and the third, and waits for the fourth.
    foreach name {lost:s lost:c flip:s wrong:s flip:c} {
        set key $p:$name
        set third [lindex [dict get $story $key] 2]
        if {[gp_info $key kind] eq "score"} {
            assert_equal 1 [r glearn $key $third CORRECT 1] "feedback for $key"
        } else {
            assert_equal 1 [gp_answer $key $third] "feedback for $key"
        }
        set info [gp_info $key]
        if {[dict get $info missing] != 0} {
            fail "$key: the machine stood still for seconds, this test cannot work: $info"
        }
        assert_equal 2 [dict get $info pending] "pending of $key"
        assert_equal 3 [dict get $info calibration-samples] "proof of $key"
    }
    assert_equal 0 [gp_info $p:wait:s calibration-samples]
    # Feedback for the first of the twenty comes too late, and so it would
    # for the others. The gate is still being tested; twelve more requests
    # that nobody will answer.
    set key $p:run:s
    assert_equal 0 [r glearn $key [lindex [dict get $story $key] 0] CORRECT 1] "late feedback for $key"
    assert_equal 20 [gp_info $key missing]
    assert_equal CALIBRATING [gp_info $key state]
    assert_equal 1 [gp_info $key candidate]
    dict set story $key [gp_right $key 12]
    assert_equal 12 [gp_info $key pending]
    # The next request finds that the 32 spot checks were lost, and the
    # gate stops. The score gate, whose spot checks all fall in one block,
    # stops with the 25th: so many lost answers among 32 spot checks could
    # hide that it got worse, and the seven that are left are proof for
    # nothing any more (not missed). The classifier has 16 spot checks for
    # each label, too few for that: it stops with the 32nd lost answer in a
    # row. Either gate still wants feedback, so that it can start again.
    assert_equal 25 [gp_lost_stop 32] "lost answers that stop a gate with 32 spot checks in a block"
    assert_equal 0 [gp_lost_stop 16] "lost answers that stop a gate with 16 spot checks in a block"
    foreach {name missing} {out:s 25 out:c 32} {
        set key $p:$name
        set learned [gp_info $key learned]
        if {$name eq "out:s"} {
            lassign [r gpredict $key SCORE 0.9] action id
        } else {
            lassign [r gpredict $key TEXT "buy cheap pills now offer 1"] action id
        }
        assert_equal ABSTAIN $action "reply of $key"
        assert {$id ne {}}
        set info [gp_info $key]
        assert_equal COLD [dict get $info state] "state of $key"
        assert_equal 0 [dict get $info generation] "generation of $key"
        assert_equal 0 [dict get $info candidate] "candidate of $key"
        assert_equal $missing [dict get $info missing] "missing of $key"
        assert_equal 1 [dict get $info revocations] "revocations of $key"
        assert_equal 1 [dict get $info pending] "pending of $key"
        # Lost answers say nothing about the model: it is kept.
        assert_equal $learned [dict get $info learned] "learned of $key"
        dict set story $key:stopped $id
        dict set story $key:missing $missing
    }
    # Twelve lost spot checks, all there are in their block, do not stop a
    # gate. Thirteen more that nobody will answer.
    set key $p:unch:s
    assert {![gp_block_stops 12 12 1e9]}
    assert_equal 0 [gp_answer $key [lindex [dict get $story $key] 0]] "late feedback for $key"
    set info [gp_info $key]
    assert_equal 12 [dict get $info missing] "missing of $key"
    assert_equal 0 [dict get $info pending] "pending of $key"
    assert_equal ACTIVE [dict get $info state] "state of $key"
    dict set story $key [gp_spots $key 13]
    assert_equal 13 [gp_info $key pending] "pending of $key"
    return $story
}

proc gp_story_answers {story} {
    set p [dict get $story prefix]
    # The answer of the first request arrives after all, and says the cheap
    # answer was right: the wrong guess that was counted is taken back, and
    # the gate is proven after the 35 right answers snapshot 1 needs.
    set key $p:flip:s
    set ids [dict get $story $key]
    assert_equal 1 [r glearn $key [lindex $ids 0] CORRECT 1] "late feedback for $key"
    assert_equal 1 [r glearn $key [lindex $ids 3] CORRECT 1]
    assert_equal 0 [gp_info $key pending]
    assert_equal 4 [gp_info $key calibration-samples]
    gp_proven_by $key [gp_need 1] 4
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    assert_equal [gp_need 1] [gp_info $key calibration-samples] "proof of $key"
    # Here the late answer says the cheap answer was wrong: the wrong guess
    # stays, and it takes more right answers to make up for it.
    set key $p:wrong:s
    set ids [dict get $story $key]
    assert_equal 1 [r glearn $key [lindex $ids 0] CORRECT 0] "late feedback for $key"
    assert_equal 1 [r glearn $key [lindex $ids 3] CORRECT 1]
    assert {[gp_need 1 1 1] > [gp_need 1]}
    gp_proven_by $key [gp_need 1 1 1] 3
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    assert_equal [expr {[gp_need 1 1 1] + 1}] [gp_info $key calibration-samples] "proof of $key"
    # The same for a classifier: late, and used.
    set key $p:flip:c
    set asked [dict get $story $key]
    assert_equal 1 [gp_answer $key [lindex $asked 0]] "late feedback for $key"
    assert_equal 1 [gp_answer $key [lindex $asked 3]]
    assert_equal 0 [gp_info $key pending]
    assert_equal 24 [gp_info $key learned]
    # The answer everything waited for.
    set key $p:wait:s
    set ids [dict get $story $key]
    assert_equal 0 [gp_info $key calibration-samples]
    assert_equal 1 [r glearn $key [lindex $ids 0] CORRECT 1]
    assert_equal 3 [gp_info $key calibration-samples]
    assert_equal 1 [r glearn $key [lindex $ids 3] CORRECT 1]
    assert_equal 4 [gp_info $key calibration-samples]
    # The feedback path works again: the first answer makes the gate take
    # its next snapshot, and it proves itself again, which for snapshot 2
    # takes more than it took for snapshot 1.
    set key $p:out:s
    lassign [r gpredict $key SCORE 0.9] action id
    assert_equal ABSTAIN $action
    assert_equal 0 [gp_info $key candidate]
    assert_equal 1 [r glearn $key $id CORRECT 1] "the first answer after the outage of $key"
    assert_equal CALIBRATING [gp_info $key state]
    assert_equal 2 [gp_info $key candidate]
    gp_proven_by $key [gp_need 2]
    assert_equal ACTIVE [gp_info $key state]
    assert_equal 2 [gp_info $key generation]
    set key $p:out:c
    lassign [r gpredict $key TEXT "meeting notes project schedule 2"] action id
    assert_equal ABSTAIN $action
    assert_equal 1 [r glearn $key $id LABEL ham] "the first answer after the outage of $key"
    assert_equal [expr {[dict get $story $key:learned] + 1}] [gp_info $key learned]
    assert_equal CALIBRATING [gp_info $key state]
    assert_equal [expr {[dict get $story $key:used] + 1}] [gp_info $key candidate]
    gp_prove $key
    assert_equal ACTIVE [gp_info $key state]
    assert {[gp_info $key generation] > [dict get $story $key:used]}
    assert_equal 1 [gp_info $key revocations]
    # The gate that gets worse. Its three spot checks from before disagree
    # too, and so does every one from now on. A block counts a spot check
    # as right until its answer says otherwise, and the gate stops with the
    # disagreement that takes the block to 1,000: after five spot checks
    # that agreed that is the tenth, not the seventh.
    set key $p:drift:s
    set asked 12
    set wrong 4
    foreach spot [dict get $story $key] {
        assert_equal 1 [gp_disagree $key $spot] "feedback for $key"
        incr wrong
        assert {![gp_block_stops $asked $wrong 1000]}
        assert_equal ACTIVE [gp_info $key state] "state of $key after $wrong disagreements among $asked spot checks"
    }
    while {1} {
        set spot [lindex [gp_spots $key 1] 0]
        incr asked
        assert_equal 1 [gp_disagree $key $spot] "feedback for $key"
        incr wrong
        if {[gp_block_stops $asked $wrong 1000]} break
        assert_equal ACTIVE [gp_info $key state] "state of $key after $wrong disagreements among $asked spot checks"
        if {$asked >= 60} { fail "$key: no number of disagreements stops the gate" }
    }
    assert_equal 10 $wrong "disagreements that stop a gate after five spot checks that agreed"
    assert {[gp_block_stops 7 7 1000] && ![gp_block_stops 6 6 1000]}
    set info [gp_info $key]
    assert_equal 1 [dict get $info revocations] "revocations of $key after $wrong disagreements among $asked spot checks"
    assert_equal 0 [dict get $info generation] "generation of $key"
    # A score gate starts over at once.
    assert_equal CALIBRATING [dict get $info state] "state of $key"
    assert_equal 2 [dict get $info candidate] "candidate of $key"
    # The gate whose spot checks filled a block of 64. Two more open the
    # next block. Then the answer of the last spot check of the first
    # block arrives and disagrees: its block has ended, so it counts in the
    # blocks of 512 and of 4,096 alone, where six disagreements among 66
    # spot checks are nothing. The first spot check of the new block
    # agrees; from there on every answer disagrees. The gate stops with
    # the eighth disagreement of the new block, the 73rd spot check, as it
    # would in a first block after one spot check that agreed: not sooner
    # (the disagreements of the block that ended, and the late one, are
    # not counted here), and not at once.
    set key $p:block:s
    set spots [gp_spots $key 2]
    assert_equal 1 [gp_disagree $key [dict get $story $key]] "late feedback for $key"
    assert_equal ACTIVE [gp_info $key state] "state of $key"
    set spots [lassign $spots agrees]
    assert_equal 1 [gp_answer $key $agrees] "feedback for $key"
    set asked 66
    set wrong 6
    set stopped 0
    while {!$stopped} {
        if {$asked >= 100} { fail "$key: no number of disagreements stops the gate" }
        if {[llength $spots]} {
            set spots [lassign $spots spot]
        } else {
            set spot [lindex [gp_spots $key 1] 0]
            incr asked
        }
        assert_equal 1 [gp_disagree $key $spot] "feedback for $key"
        incr wrong
        # The block of 64 that is open holds the spot checks after the
        # 64th, and all the disagreements but six.
        set stopped [expr {[gp_block_stops [expr {$asked - 64}] [expr {$wrong - 6}] 1000] ||
                           [gp_block_stops $asked $wrong 1000]}]
        if {!$stopped} {
            assert_equal ACTIVE [gp_info $key state] "state of $key after $wrong disagreements among $asked spot checks"
        }
    }
    assert_equal {73 14} [list $asked $wrong] "spot checks and disagreements that stop a gate in its second block of 64"
    set info [gp_info $key]
    assert_equal 1 [dict get $info revocations] "revocations of $key after $wrong disagreements among $asked spot checks"
    assert_equal CALIBRATING [dict get $info state] "state of $key"
    assert_equal 2 [dict get $info candidate] "candidate of $key"
    # The snapshot behind another. The three answers arrive: now it has
    # proven ham, and still it does not take over, because the first
    # snapshot answers more of its proof requests than it does itself. It
    # takes over when it answers more, some odd texts later. (Each answer
    # is held against the count by gp_behind_answer.)
    set key $p:behind:c
    set b [dict get $story $key]
    while {[llength [dict get $b waiting]]} { gp_behind_answer b }
    assert {[dict get $b ham] + [dict get $b odd] >= [gp_need 2 2]}
    assert_equal 1 [gp_info $key generation] "the snapshot of $key that answers"
    for {set n 0} {$n < 100 && [gp_info $key generation] == 1} {incr n} {
        gp_behind_ask b odd
        gp_behind_answer b
    }
    assert {$n >= 10}
    set info [gp_info $key]
    assert_equal 2 [dict get $info generation] "the snapshot of $key that answers"
    assert_equal 3 [dict get $info candidate] "candidate of $key"
    assert_equal 0 [dict get $info revocations] "revocations of $key"
    dict set story $key $b
    # The snapshot that leaves everything to the teacher is dropped when it
    # has used up its budget, 780 such requests, and not one before; the
    # next snapshot is tested at once.
    set key $p:defer:c
    assert_equal 780 [gp_deferring_budget]
    foreach pair [gp_deferring_ask $key [expr {[gp_deferring_budget] - 401}]] {
        assert_equal 1 [gp_answer $key $pair] "feedback for a fresh id on $key"
    }
    assert_equal 1 [gp_info $key candidate] "candidate of $key, one request short of [gp_deferring_budget]"
    assert_equal 1 [gp_answer $key [lindex [gp_deferring_ask $key 1] 0]]
    assert_equal 2 [gp_info $key candidate] "candidate of $key after [gp_deferring_budget] requests it left to the teacher"
    assert_equal 0 [gp_info $key generation] "generation of $key"
    return $story
}

proc gp_story_lost {story} {
    set p [dict get $story prefix]
    # The first request of these two never got its answer in time: it stays
    # a wrong guess, and its id is forgotten.
    foreach name {lost:s lost:c} {
        set key $p:$name
        set first [lindex [dict get $story $key] 0]
        assert_equal 0 [gp_info $key missing]
        if {$name eq "lost:s"} {
            assert_equal 0 [r glearn $key $first CORRECT 1] "feedback for $key after the feedback timeout"
        } else {
            assert_equal 0 [gp_answer $key $first] "feedback for $key after the feedback timeout"
        }
        set info [gp_info $key]
        # So is its fourth request, which was never answered either.
        assert_equal 2 [dict get $info missing] "missing of $key"
        assert_equal 0 [dict get $info pending] "pending of $key"
        assert_equal 4 [dict get $info calibration-samples] "proof of $key"
        assert_equal CALIBRATING [dict get $info state] "state of $key"
    }
    # Twelve more lost answers make 32 in a row: the gate stops.
    set key $p:run:s
    assert_equal CALIBRATING [gp_info $key state]
    assert_equal 0 [r glearn $key [lindex [dict get $story $key] 0] CORRECT 1] "late feedback for $key"
    set info [gp_info $key]
    assert_equal COLD [dict get $info state] "state of $key"
    assert_equal 32 [dict get $info missing] "missing of $key"
    assert_equal 0 [dict get $info candidate] "candidate of $key"
    assert_equal 0 [dict get $info revocations] "revocations of $key"
    # The requests that were asked when the two gates had stopped were not
    # answered either. They were proof for nothing: not missed.
    foreach name {out:s out:c} {
        set key $p:$name
        set id [dict get $story $key:stopped]
        if {$name eq "out:s"} {
            assert_equal 0 [r glearn $key $id CORRECT 1] "late feedback for $key"
        } else {
            assert_equal 0 [r glearn $key $id LABEL spam] "late feedback for $key"
        }
        assert_equal [dict get $story $key:missing] [gp_info $key missing] "missing of $key"
        assert_equal ACTIVE [gp_info $key state] "state of $key"
    }
    # The thirteen spot checks are lost one after the other. With the tenth
    # of them 22 of the 25 spot checks of the block are lost: the gate can
    # no longer be checked and stops. It keeps nothing to test, and waits
    # for an answer. The three that are left were proof for nothing any
    # more: not missed.
    set key $p:unch:s
    assert_equal 22 [gp_lost_stop 25] "lost answers that stop a gate with 25 spot checks in a block"
    assert_equal 0 [gp_answer $key [lindex [dict get $story $key] 0]] "late feedback for $key"
    set info [gp_info $key]
    assert_equal COLD [dict get $info state] "state of $key"
    assert_equal 0 [dict get $info generation] "generation of $key"
    assert_equal 0 [dict get $info candidate] "candidate of $key"
    assert_equal 22 [dict get $info missing] "missing of $key"
    assert_equal 1 [dict get $info revocations] "revocations of $key"
    assert_equal 0 [dict get $info pending] "pending of $key"
    # 32 lost answers, and no outage: an answer had arrived after 31 of the
    # requests were asked. Only the last one starts a run. The gate is
    # still being tested.
    set key $p:mark:s
    assert_equal 0 [r glearn $key [lindex [dict get $story $key] 0] CORRECT 1] "late feedback for $key"
    set info [gp_info $key]
    assert_equal 32 [dict get $info missing] "missing of $key"
    assert_equal 0 [dict get $info pending] "pending of $key"
    assert_equal 33 [dict get $info calibration-samples] "proof of $key"
    assert_equal CALIBRATING [dict get $info state] "state of $key"
    assert_equal 1 [dict get $info candidate] "candidate of $key"
    assert_equal 0 [dict get $info revocations] "revocations of $key"
    # The gate that answers by itself: nine requests are lost now, the two
    # that proof had moved on without and the seven behind them.
    set key $p:tick:s
    assert_equal {ACCEPT {}} [r gpredict $key SCORE 0.9]
    set info [gp_info $key]
    assert_equal 9 [dict get $info missing] "missing of $key"
    assert_equal 0 [dict get $info pending] "pending of $key"
    assert_equal 45 [dict get $info calibration-samples] "proof of $key"
    assert_equal ACTIVE [dict get $info state] "state of $key"
    return $story
}

# --------------------------- Gates at their limits -------------------------
#
# No counter, request number or snapshot number of a gate goes above 2^62,
# and its clock stops a week before the largest number there is
# (gate_defs.h). No gate gets there by counting, but a payload can put it
# there; and a gate that loads is one that can be saved and loaded again,
# whatever happens to it in between (CHANGES.md 9d).
#
# gp_limits restores five such gates. Each is a score gate that answers
# (AUDIT 1) and holds four spot checks, the second answered and
# disagreeing, the third answered and agreeing, with something at its
# limit:
#
#   all    the clock and every time the gate holds at the last moment there
#          is; every count of its snapshot (proof, the takeover count, the
#          spot checks and their blocks, each as full as a block gets), the
#          number of every waiting spot check and every counter of the gate
#          at 2^62; the run of missing answers one short of what stops a
#          gate. All but what the four other gates have:
#   snap   2^62 snapshots were taken, and the last of them answers;
#   next   one request number is left, the last there is;
#   count  2^62 requests were asked, and all but four of them left to the
#          teacher;
#   own    2^62 requests were asked, and all but four of them answered by
#          the gate itself (this one has AUDIT 0.000000001).
#
# Returns a dict: gates, key -> the DUMP payload it was restored from; ids,
# the ids of the four requests; and, for other gates like these, opened,
# the payload of the gate they were made of (gp_payload_open), and values,
# name -> the fields that were given other values, and the values.
proc gp_limits {p {timeout 60000}} {
    set compression [lindex [r config get rdbcompression] 1]
    r config set rdbcompression no
    set key $p:template
    gp_score $key 1 $timeout
    gp_proven_by $key [gp_need 1]
    set spots [gp_spots $key 4]
    assert_equal 1 [gp_disagree $key [lindex $spots 1]]
    assert_equal 1 [gp_answer $key [lindex $spots 2]]
    set o [gp_payload_open [r dump $key]]
    assert_equal 1 [r del $key]
    r config set rdbcompression $compression
    set ids {}
    foreach spot $spots { lappend ids [lindex $spot 0] }
    set max $::gp_count_max
    set last $::gp_clock_max
    set all [list created $last clock $last snapshot_updates $max missing_run 31 \
                  serv.frozen $last serv.promoted $last]
    foreach name {samples errors missed audits audit_errors budget} { lappend all serv.$name $max }
    for {set t 0} {$t < 32} {incr t} {
        lappend all serv.region.0.$t.n $max serv.region.0.$t.errors $max serv.paired.0.$t $max
        if {[gp_payload_get $o serv.region.0.$t.proven] != 0} { lappend all serv.region.0.$t.proven $max }
    }
    lappend all serv.drift.0.asked $max serv.drift.0.errors $max
    # The 2^62th spot check is the last of a block of each length.
    foreach scale {0 1 2} length {64 512 4096} {
        lappend all serv.drift.0.wrong$scale [expr {$length - 4}] serv.drift.0.lost$scale 4
    }
    for {set i 0} {$i < 4} {incr i} { lappend all req.$i.created $last req.$i.spot $max }
    foreach name {full observations learned ignored missing dropped promotions revocations retired} {
        lappend all stats.$name $max
    }
    lappend all stats.window 1024 stats.coverage 1.0
    set snap [list generations $max serv.gen $max]
    for {set i 0} {$i < 4} {incr i} { lappend snap req.$i.serv_gen $max }
    set next [list next [expr {$max - 1}]]
    # All of its requests were left to the teacher but the four spot checks.
    assert_equal 4 [gp_payload_get $o stats.audited]
    set count [list stats.requests $max stats.abstained [expr {$max - 4}]]
    set own [list audit 0.000000001 stats.requests $max stats.accepted [expr {$max - 4}] stats.abstained 0]
    set gates {}
    foreach {name values} [list all $all snap $snap next $next count $count own $own] {
        set payload [gp_payload_with $o {*}$values]
        assert_equal OK [r restore $p:$name 0 $payload] "RESTORE of a gate at its limits ($name)"
        dict set gates $p:$name $payload
    }
    dict create gates $gates ids $ids opened $o \
        values [dict create all $all snap $snap next $next count $count own $own]
}

# DUMP of a key as the module wrote it (without RDB compression).
proc gp_dump_plain {key} {
    set compression [lindex [r config get rdbcompression] 1]
    r config set rdbcompression no
    set payload [r dump $key]
    r config set rdbcompression $compression
    return $payload
}

# The gates of gp_limits, not written to since they came in or came back:
# each is still the payload it was restored from, and GINFO shows counts.
proc gp_limits_intact {lim} {
    dict for {key payload} [dict get $lim gates] {
        gp_assert_same_bytes $payload [gp_dump_plain $key] "the payload $key was restored from and its DUMP"
        set info [gp_info $key]
        gp_check_info $key $info
        foreach field {learned requests accepted audited abstained audit-samples audit-errors
                       calibration-samples pending missing revocations generation candidate memory} {
            if {[dict get $info $field] < 0} { fail "$key: GINFO shows $field [dict get $info $field]" }
        }
    }
}

# Whether Redis could still load a gate: asked of RESTORE, with the gate's
# own DUMP, and not of DEBUG RELOAD or DEBUG LOADAOF (a server that cannot
# load its own file has lost every key in it, or does not come back).
# Returns the problem, or an empty string.
proc gp_unloadable {key} {
    if {[catch {r restore gp:again 0 [r dump $key] replace} reply]} { return $reply }
    r del gp:again
    return {}
}

# Restart the server. One that cannot load what it has saved does not come
# back, and restart_server would wait for it for ever. So it is tried first:
# a server with an AOF loads its files (if it cannot, it is gone, and the
# test fails for that), one without is asked of each gate whether Redis
# could load it.
proc gp_restart {} {
    if {[lindex [r config get appendonly] 1] eq "yes"} {
        assert_equal OK [r debug loadaof]
    } else {
        foreach key [r keys *] {
            if {[r type $key] ne "gate-type"} continue
            set problem [gp_unloadable $key]
            if {$problem ne {}} {
                fail "the server is not restarted: it could not load $key, which cannot be restored from its own DUMP: $problem"
            }
        }
    }
    restart_server 0 true false
}

# Use a score gate that was restored at some limit, as a client would, and
# ask after every step whether Redis could still load it. What a gate does
# when it has no numbers left to count with is its own business (it may
# refuse to go on), so nothing is asked of its replies but that they are
# replies, and of the numbers GINFO shows but that none is beyond the
# limit. Returns the problems, one line each.
proc gp_use_for_good {key what} {
    set problems {}
    set check {
        set problem [gp_unloadable $key]
        if {$problem ne {}} { lappend problems "$what: $when the gate cannot be restored from its own DUMP: $problem" }
        set info [gp_info $key]
        gp_check_info "$what, $when" $info
        foreach field {generation candidate} {
            if {[dict get $info $field] > $::gp_count_max} {
                lappend problems "$what: $when GINFO shows $field [dict get $info $field]"
            }
        }
    }
    set ask {
        set reply [r gpredict $key SCORE 0.9]
        gp_check_reply $what $reply
        set id [lindex $reply 1]
    }
    # Its first request: the gate drops what it held, tests a snapshot if
    # it has a number for one, and records the request if it has a number
    # for it.
    eval $ask
    set when "with its first request waiting"
    eval $check
    if {$id ne {}} { r glearn $key $id CORRECT 1 }
    set when "after its first answer"
    eval $check
    # It proves itself and answers ...
    for {set i 0} {$i < 100 && [gp_info $key generation] == 0} {incr i} {
        eval $ask
        if {$id ne {}} { r glearn $key $id CORRECT 1 }
    }
    set when "after it was asked and answered 100 times"
    eval $check
    # ... has a spot check waiting, and stops when the others disagree,
    # which starts the next snapshot.
    eval $ask
    set when "with a second request waiting"
    eval $check
    set revocations [gp_info $key revocations]
    for {set i 0} {$i < 40 && [gp_info $key generation] != 0 && [gp_info $key revocations] == $revocations} {incr i} {
        eval $ask
        if {$id ne {}} { r glearn $key $id CORRECT 0 }
    }
    set when "after its answers disagreed"
    eval $check
    assert_equal OK [r gpause $key]
    assert_equal OK [r gresume $key]
    # A request nobody answers.
    eval $ask
    set when "at the end"
    eval $check
    return $problems
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
        # Proof is counted in request order: these two answers have to wait
        # for the answers of the older requests (for a quarter of the
        # feedback timeout, which is a minute here). The model learns from
        # them at once; "pending" is what still waits for its GLEARN.
        assert_equal 1 [gp_answer g:cal [lindex $waiting 2]]
        assert_equal 1 [gp_answer g:cal [lindex $waiting 4]]
        assert_equal CALIBRATING [gp_info g:cal state]
        assert_equal 22 [gp_info g:cal learned]
        assert_equal 0 [gp_info g:cal calibration-samples]
        assert_equal 4 [gp_info g:cal pending]
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
        gp_begin 130
        set key [gp_takeover g:took]
        assert_equal ACTIVE [gp_info $key state]
        assert_equal 2 [gp_info $key generation]
        # The count the takeover was decided on is for a snapshot that is
        # tested behind another: it starts from nothing for the one that
        # answers now (CHANGES.md 9a), and for the third, which is tested
        # behind it since.
        assert_equal 3 [gp_info $key candidate]
        set o [gp_payload_open [gp_dump_plain $key]]
        assert_equal 2 [gp_payload_get $o serv.gen]
        foreach row {0 1} {
            for {set t 0} {$t < 32} {incr t} {
                assert_equal 0 [gp_payload_get $o serv.paired.$row.$t] "the takeover count of the snapshot of $key that answers"
                assert_equal 0 [gp_payload_get $o cand.paired.$row.$t] "the takeover count of the snapshot of $key that is tested"
            }
        }
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
        assert_equal 0 [gp_info s:cal calibration-samples]
        assert_equal 6 [gp_info s:cal pending]
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
        # Disagreements alone, from its first spot check on: the seventh
        # stops a gate (SPEC.md section 5, gp_block).
        assert {[gp_block_stops 7 7 1000] && ![gp_block_stops 6 6 1000]}
        assert_equal 7 $i "disagreements that stop a gate"
        assert_equal 0 [gp_info s:revoked generation]
        # It starts over by itself, with the next snapshot number.
        assert_equal CALIBRATING [gp_info s:revoked state]
        assert_equal 2 [gp_info s:revoked candidate]
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
        # Each label has spot checks of its own, and the requests take
        # turns: the seventh disagreement for a label is the thirteenth
        # in all.
        assert_equal 13 $i "disagreements, for two labels in turn, that stop a gate"
        assert_equal 0 [gp_info g:revoked generation]
        assert_equal 0 [gp_info g:revoked learned]
        assert_equal COLD [gp_info g:revoked state]
        gp_teach g:revoked 3
        gp_collect g:revoked 2
        # Requests whose feedback never came. (More than three are lost if
        # the machine pauses between the last two requests.)
        gp_score s:late 0.2 40
        gp_collect s:late 3
        after 80
        gp_collect s:late 2
        assert {[gp_info s:late missing] >= 3}
        gp_reload_check {g:reset s:revoked g:revoked s:late}
    }

    gp_test {DEBUG RELOAD keeps the digest: requests older than the feedback timeout that nothing has noticed yet} {
        gp_begin 110
        gp_score s:stale 0.2 40
        gp_classifier g:stale 20 0.2 40
        gp_teach g:stale 20
        set old [gp_collect s:stale 3]
        set oldc [gp_collect g:stale 3]
        # A gate does its bookkeeping when it is written to: time alone
        # changes nothing, neither what GINFO shows nor what is saved.
        foreach key {s:stale g:stale} {
            set stale_info($key) [gp_info $key]
            set stale_digest($key) [debug_digest_value $key]
            set stale_dump($key) [r dump $key]
        }
        after 80
        foreach key {s:stale g:stale} {
            assert_equal $stale_info($key) [gp_info $key] "GINFO of $key before and after doing nothing"
            assert_equal $stale_digest($key) [debug_digest_value $key] "digest of $key before and after doing nothing"
            gp_assert_same_bytes $stale_dump($key) [r dump $key] "DUMP of $key before and after doing nothing"
        }
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

    gp_test {DEBUG RELOAD keeps the digest: as many requests as a gate can hold, 16,384 answers that wait and 4,096 unanswered} {
        gp_begin 115
        gp_score s:most
        # The first request is never answered, so the answer of every
        # request after it has to wait for it: 16,384 of them, the most a
        # gate lets wait (CHANGES.md section 2). Answered requests do not
        # count against the 4,096 a gate holds for feedback.
        lassign [r gpredict s:most SCORE 0.9] action first
        assert {$first ne {}}
        set rd [redis_deferring_client]
        fconfigure [$rd channel] -buffersize 1000000
        for {set batch 0} {$batch < 8} {incr batch} {
            for {set i 0} {$i < 2048} {incr i} { $rd gpredict s:most SCORE 0.9 }
            set ids {}
            for {set i 0} {$i < 2048} {incr i} { lappend ids [lindex [$rd read] 1] }
            foreach id $ids { $rd glearn s:most $id CORRECT 1 }
            set used 0
            foreach id $ids { incr used [$rd read] }
            assert_equal 2048 $used "answers used in batch $batch"
        }
        set info [gp_info s:most]
        assert_equal 1 [dict get $info pending]
        assert_equal 0 [dict get $info calibration-samples]
        assert_equal CALIBRATING [dict get $info state]
        assert_equal 16385 [dict get $info requests]
        # 4,095 unanswered requests more, and it is full.
        for {set i 0} {$i < 4095} {incr i} { $rd gpredict s:most SCORE 0.9 }
        for {set i 0} {$i < 4095} {incr i} {
            lassign [$rd read] action id
            if {$action ne "ABSTAIN" || $id eq {}} { fail "request $i of the last 4,095: $action $id" }
        }
        $rd close
        assert_equal 4096 [gp_info s:most pending]
        assert_equal 0 [gp_info s:most calibration-samples]
        assert_equal {ABSTAIN {}} [r gpredict s:most SCORE 0.9]
        # Through DUMP and RESTORE ...
        assert_equal OK [r restore s:most:copy 0 [r dump s:most]]
        assert_equal [debug_digest_value s:most] [debug_digest_value s:most:copy]
        assert_equal [gp_info s:most memory] [gp_info s:most:copy memory]
        gp_assert_came_back s:most:copy [gp_info s:most]
        assert_equal OK [r rename s:most:copy s:most:big]
        # ... and through an RDB file.
        gp_reload_check s:most
        # Nothing waits any more: neither the unanswered request ...
        assert_equal 0 [r glearn s:most $first CORRECT 1]
        # ... nor the answers. The gate starts again from nothing.
        assert_equal 0 [gp_info s:most calibration-samples]
        gp_proven_by s:most [gp_need 2]
        assert_equal ACTIVE [gp_info s:most state]
        assert_equal 2 [gp_info s:most generation]
        r del s:most
        # A gate that holds as much again, given up in the background.
        assert_equal OK [r restore s:most 0 [r dump s:most:big]]
        assert_equal 2 [r unlink s:most s:most:big]
        wait_lazyfree_done r
        assert_equal PONG [r ping]
    }

    # ---------------------- Late and lost answers --------------------------

    gp_test {DEBUG RELOAD keeps the digest: answers that wait, proof that moved on, lost answers, gates that stopped for want of answers and gates that resumed, spot checks that disagreed or were lost, a takeover undecided, paused or not} {
        gp_begin 116
        set ::gp_z1 [gp_states n]
        set zkeys [dict keys $::gp_z1]
        assert_equal 23 [llength $zkeys]
        # All four states of GINFO are among them.
        set states {}
        foreach key $zkeys { dict incr states [gp_info $key state] }
        foreach state {COLD CALIBRATING ACTIVE PAUSED} { assert {[dict exists $states $state]} }
        gp_reload_check $zkeys
        # Loaded and saved once more, without a write in between.
        gp_reload_check $zkeys
    }

    gp_test {BGSAVE and DEBUG RELOAD NOSAVE: the same gates still have their digests} {
        set zkeys [dict keys $::gp_z1]
        foreach key $zkeys { set z_info($key) [gp_info $key] }
        set before [gp_digests $zkeys]
        r bgsave
        waitForBgsave r
        assert_equal ok [s rdb_last_bgsave_status]
        assert_equal $before [gp_digests $zkeys]
        assert_equal OK [r debug reload nosave]
        assert_equal $before [gp_digests $zkeys]
        foreach key $zkeys { assert_equal $z_info($key) [gp_info $key] "GINFO of $key" }
    }

    gp_test {After DEBUG RELOAD each of these gates drops what it held at its first write and proves itself again with a new snapshot number} {
        gp_begin 117
        dict for {key meta} $::gp_z1 { gp_recovers $key $meta }
        # What they are now is saved and loaded like anything else.
        gp_reload_check [dict keys $::gp_z1]
    }

    gp_test {DEBUG RELOAD keeps the digest: gates at their limits (every count at 2^62, the clock at its last moment, the last snapshot number, the last request number)} {
        gp_begin 118
        set lim [gp_limits g:lim]
        set lkeys [dict keys [dict get $lim gates]]
        assert_equal 5 [llength $lkeys]
        gp_limits_intact $lim
        foreach key $lkeys {
            set l_digest($key) [debug_digest_value $key]
            set l_info($key) [gp_info $key]
            set l_memory($key) [memory_usage $key]
        }
        assert_equal OK [r debug reload]
        foreach key $lkeys {
            assert_equal $l_digest($key) [debug_digest_value $key] "digest of $key after DEBUG RELOAD"
            assert_equal gate-type [r type $key]
            assert_equal $l_memory($key) [memory_usage $key] "MEMORY USAGE of $key after DEBUG RELOAD"
            # They were not written to, before or since: GINFO shows what
            # it showed. (What that is for a gate without a snapshot number
            # left has a test of its own.)
            assert_equal $l_info($key) [gp_info $key] "GINFO of $key after DEBUG RELOAD"
        }
        gp_limits_intact $lim
        # Through a file written in the background, too.
        set before [gp_digests $lkeys]
        r bgsave
        waitForBgsave r
        assert_equal ok [s rdb_last_bgsave_status]
        assert_equal OK [r debug reload nosave]
        assert_equal $before [gp_digests $lkeys]
        gp_limits_intact $lim
        assert_equal 5 [r del {*}$lkeys]
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

    gp_test {After DEBUG RELOAD and after RESTORE a score gate needs the proof of its next snapshot number: 35, 41, 44 and 47 right answers} {
        gp_begin 211
        # With AUDIT 1 the gate never answers by itself: every request is
        # asked, and answered "right".
        gp_score b:exact 1
        set number 0
        foreach how {GCREATE {DEBUG RELOAD} RESTORE {DEBUG RELOAD}} need {35 41 44 47} {
            if {$how eq "DEBUG RELOAD"} { assert_equal OK [r debug reload] }
            if {$how eq "RESTORE"} { assert_equal OK [r restore b:exact 0 [r dump b:exact] replace] }
            incr number
            # SPEC.md section 5 with ALPHA 0.2 and DELTA 0.2: the wealth of
            # a threshold must reach 32 x g (g + 1) / 0.2.
            assert_equal $need [gp_need $number] "right answers snapshot $number needs"
            assert_equal $number [gp_info b:exact candidate] "the snapshot that is tested after $how"
            assert_equal 0 [gp_info b:exact calibration-samples]
            gp_proven_by b:exact $need
            set info [gp_info b:exact]
            assert_equal ACTIVE [dict get $info state]
            assert_equal $number [dict get $info generation]
            assert_equal $need [dict get $info calibration-samples]
            assert_equal 0 [dict get $info missing]
            # Some spot checks, so that there is proof of both kinds to lose.
            assert_equal 5 [gp_right_answers b:exact 5]
            assert_equal 5 [gp_info b:exact audit-samples]
        }
    }

    gp_test {After DEBUG RELOAD a classifier needs the proof of its next snapshot number, for as many labels as it may answer} {
        gp_begin 212
        # Three labels, one of which the gate must never answer with: the
        # proof is that of two labels (32 x 2 x g (g + 1) / DELTA).
        r config set gate.snapshot-examples 90
        assert_equal OK [r gcreate b:exactc ALPHA 0.2 DELTA 0.2 AUDIT 1 CLASSES 3 spam ham other ALWAYSDEFER 1 other]
        gp_defaults
        for {set i 0} {$i < 30} {incr i} {
            r gadd b:exactc TEXT "buy cheap pills now offer [incr ::gp_n]" LABEL spam
            r gadd b:exactc TEXT "meeting notes project schedule [incr ::gp_n]" LABEL ham
            r gadd b:exactc TEXT "weather forecast rain tomorrow [incr ::gp_n]" LABEL other
        }
        assert_equal CALIBRATING [gp_info b:exactc state]
        assert {[gp_need 1 2] > [gp_need 1] && [gp_need 1 3] > [gp_need 1 2]}
        foreach number {1 2} {
            if {$number == 2} { assert_equal OK [r debug reload] }
            assert_equal $number [gp_info b:exactc candidate]
            # Only one topic is asked about, and the model knows it: every
            # guess is right and is proof for that label.
            set need [gp_need $number 2]
            for {set i 1} {$i <= $need} {incr i} {
                assert_equal 0 [gp_info b:exactc generation] "the snapshot that answers after [expr {$i - 1}] of $need right answers"
                lassign [r gpredict b:exactc TEXT "buy cheap pills now offer [incr ::gp_n]"] action id
                assert_equal ABSTAIN $action
                assert_equal 1 [r glearn b:exactc $id LABEL spam]
            }
            set info [gp_info b:exactc]
            assert_equal ACTIVE [dict get $info state]
            assert_equal $number [dict get $info generation] "the snapshot that answers after $need right answers"
            # It answers for that label alone; the label it must never
            # answer with is still marked so.
            lassign [dict get $info thresholds] spam ham other
            assert {$spam ne {} && $ham eq {} && $other eq {}}
            assert {[gp_same_numbers {0.2 0.2 0} [dict get $info alphas]]}
        }
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
        gp_restart
        assert_equal PONG [r ping]
        assert_equal 0 [r dbsize]
        assert_error "ERR no such key" {r ginfo nothing}
    }

    gp_test {SAVE and restart: every gate comes back with the digest it had} {
        gp_begin 301
        # Settings that are not the defaults: after the restart the server
        # has the defaults again, the gates must not.
        gp_classifier r:cold 20 0.2 500
        gp_score r:late 0.2 500
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
        # A gate that is only given right answers, and never answers by
        # itself (AUDIT 1): its first snapshot is proven by the 35th.
        gp_score r:exact 1
        gp_proven_by r:exact [gp_need 1]
        assert_equal 1 [gp_info r:exact generation]
        r set r:string "not a gate"
        set keys {r:cold r:late r:cal r:act r:score r:paused}
        foreach key [concat $keys r:exact] {
            set r_info($key) [gp_info $key]
            set r_used($key) [gp_max_number $key]
        }
        set before [gp_digests [concat $keys r:exact]]
        assert_equal OK [r save]
        gp_restart
        assert_equal $before [gp_digests [concat $keys r:exact]]
        assert_equal "not a gate" [r get r:string]
        foreach key [concat $keys r:exact] {
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
        # The file kept count of the snapshots: the next one is number 2,
        # and number 2 needs more proof than number 1 did.
        assert_equal 2 [gp_info r:exact candidate]
        assert {[gp_need 2] > [gp_need 1]}
        gp_proven_by r:exact [gp_need 2]
        assert_equal 2 [gp_info r:exact generation]
    }

    gp_test {After a restart a gate still has the snapshot step and the feedback timeout it was created with} {
        gp_begin 303
        # The server is back to its defaults.
        assert_equal 1000 [lindex [r config get gate.snapshot-examples] 1]
        assert_equal 60000 [lindex [r config get gate.feedback-timeout] 1]
        # Created with a snapshot step of 20: 10 examples before the
        # restart, 10 more now.
        assert_equal COLD [gp_info r:cold state]
        assert_equal 10 [gp_info r:cold learned]
        gp_teach r:cold 9
        assert_equal COLD [gp_info r:cold state]
        gp_teach r:cold 1
        assert_equal CALIBRATING [gp_info r:cold state]
        assert {[gp_info r:cold candidate] > 0}
        # Created with a feedback timeout of half a second.
        set waiting [gp_collect r:late 2]
        set waitingc [gp_collect r:cold 2]
        assert_equal 1 [gp_answer r:late [lindex $waiting 0]]
        assert_equal 1 [gp_answer r:cold [lindex $waitingc 0]]
        after 650
        assert_equal 0 [gp_answer r:late [lindex $waiting 1]] "late feedback for r:late"
        assert_equal 0 [gp_answer r:cold [lindex $waitingc 1]] "late feedback for r:cold"
        # Each lost the answer of one request that was proof.
        assert_equal 1 [gp_info r:late missing]
        assert_equal 1 [gp_info r:cold missing]
    }

    gp_test {SAVE and restart: gates with late and lost answers, gates that stopped and gates that resumed, gates with spot checks and with a takeover undecided come back with the digests they had} {
        gp_begin 310
        set ::gp_z3 [gp_states q]
        set zkeys [dict keys $::gp_z3]
        foreach key $zkeys { set z_info($key) [gp_info $key] }
        set before [gp_digests $zkeys]
        assert_equal OK [r save]
        gp_restart
        assert_equal $before [gp_digests $zkeys]
        foreach key $zkeys {
            assert_equal gate-type [r type $key]
            gp_assert_came_back $key $z_info($key)
        }
    }

    gp_test {After a restart each of these gates drops what it held at its first write and proves itself again with a new snapshot number} {
        gp_begin 311
        dict for {key meta} $::gp_z3 { gp_recovers $key $meta }
    }

    gp_test {SAVE and restart: gates at their limits come back with the digests they had} {
        gp_begin 312
        set lim [gp_limits q:lim]
        set lkeys [dict keys [dict get $lim gates]]
        set before [gp_digests $lkeys]
        assert_equal OK [r save]
        gp_restart
        assert_equal $before [gp_digests $lkeys]
        gp_limits_intact $lim
        assert_equal 5 [r del {*}$lkeys]
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
        gp_restart
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
        gp_defaults
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
        gp_assert_all_fit
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
            gp_assert_came_back $key $a_info($key) aof
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
        gp_assert_all_fit
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
        gp_defaults
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
        assert_equal [gp_need 1] $answered "answers the proof took"
        set waiting [expr {45 - $answered}]
        assert_equal $waiting [gp_info a:tick pending]
        after 1100
        # This request is answered by the gate: nothing about it goes to
        # the file. That the waiting requests are now too old must.
        assert_equal {ACCEPT {}} [r gpredict a:tick SCORE 0.9]
        assert_equal $waiting [gp_info a:tick missing]
        assert_equal 0 [gp_info a:tick pending]
        # It is there as what happened: time passed.
        gp_aof_flush
        assert_equal [concat CREATE [lrepeat 45 PREDICT] [lrepeat $answered LEARN] TICK] \
            [gp_aof_ops [get_last_incr_aof_path r] a:tick]
        # Requests the gate answers when nothing is due write nothing.
        set size [file size [get_last_incr_aof_path r]]
        for {set i 0} {$i < 10} {incr i} { assert_equal {ACCEPT {}} [r gpredict a:tick SCORE 0.9] }
        gp_aof_flush
        assert {[file size [get_last_incr_aof_path r]] - $size < 100}
        set before [debug_digest_value a:tick]
        assert_equal OK [r debug loadaof]
        assert_equal $before [debug_digest_value a:tick]
        assert_equal 0 [r glearn a:tick [lindex $ids end] CORRECT 1]
        assert_equal $waiting [gp_info a:tick missing]
        assert_equal 0 [gp_info a:tick pending]
        gp_assert_all_fit
    }

    gp_test {AOF: of a text of a megabyte the file gets the 4,096 bytes that are read, and DEBUG LOADAOF gives the same gate} {
        gp_begin 412
        gp_classifier a:long
        gp_teach a:long 10
        # The longest text a request may have.
        set text [string range [string repeat "buy cheap pills now offer " 40330] 0 1048575]
        assert_equal 1048576 [string length $text]
        gp_aof_flush
        set path [get_last_incr_aof_path r]
        set size [file size $path]
        assert_equal 11 [r gadd a:long TEXT $text LABEL spam]
        lassign [r gpredict a:long TEXT $text FEATURES 1 depth 0.5] action answered
        assert_equal 1 [r glearn a:long $answered LABEL spam]
        lassign [r gpredict a:long TEXT $text] action waiting
        assert {$waiting ne {}}
        gp_aof_flush
        # An example and two requests, each with what is read of its text.
        set grown [expr {[file size $path] - $size}]
        assert {$grown > 3 * 4096 && $grown < 4 * 4096}
        # One byte more is refused, and changes nothing.
        set digest [debug_digest_value a:long]
        assert_error "ERR*" {r gadd a:long TEXT "${text}!" LABEL spam}
        assert_error "ERR*" {r gpredict a:long TEXT "${text}!"}
        assert_equal $digest [debug_digest_value a:long]
        # The file gives the same gate: the same model, and the same
        # request waiting with the same clues.
        set memory [gp_info a:long memory]
        assert_equal OK [r debug loadaof]
        assert_equal $digest [debug_digest_value a:long]
        assert_equal $memory [gp_info a:long memory]
        assert_equal 12 [gp_info a:long learned]
        gp_assert_all_fit
    }

    gp_test {AOF: a gate loaded from the AOF needs the proof of its next snapshot number} {
        gp_begin 413
        gp_score a:exact 1
        gp_proven_by a:exact [gp_need 1]
        assert_equal 1 [gp_info a:exact generation]
        assert_equal OK [r debug loadaof]
        assert_equal 2 [gp_info a:exact candidate]
        gp_proven_by a:exact [gp_need 2]
        assert_equal 2 [gp_info a:exact generation]
        set digest [debug_digest_value a:exact]
        assert_equal OK [r debug loadaof]
        assert_equal $digest [debug_digest_value a:exact]
        assert_equal 3 [gp_info a:exact candidate]
        gp_assert_all_fit
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
            gp_assert_all_fit
            gp_assert_realized [gp_aof_commands [get_last_incr_aof_path r]]
        }
    }

    # The clock in the file. Each round follows 23 gates into the states
    # late and lost answers lead to (gp_states) and sixteen more through a
    # story of late answers, lost answers and an outage (gp_story_*), and
    # then loads the file. In two rounds the file is rewritten in the middle
    # of the story, so that what the gates were doing at that moment goes
    # through the snapshot a rewrite makes of them.
    foreach {rewrite seed} {none 430 yes 431 no 432} {
        set title [expr {$rewrite eq "none" ? "no rewrite" : "a rewrite in the middle with aof-use-rdb-preamble $rewrite"}]
        gp_test "AOF: late answers, lost answers, an outage, spot checks and a takeover, $title: DEBUG LOADAOF gives every gate the digest it had" {
            gp_begin $seed
            if {$rewrite eq "none"} {
                # Start from an empty history: what the tests did so far
                # goes into a new base file, the story into a new log.
                r config set aof-use-rdb-preamble yes
                r bgrewriteaof
                waitForBgrewriteaof r
                assert_equal ok [s aof_last_bgrewrite_status]
            }
            set p t$rewrite
            set z [gp_states_ask $p:z]
            set story [gp_story_ask $p]
            after $::gp_wait
            set story [gp_story_late $story]
            set z [gp_states_settle $z]
            set keys [concat [dict keys $z] [gp_story_keys $story]]
            if {$rewrite ne "none"} {
                r config set aof-use-rdb-preamble $rewrite
                r bgrewriteaof
                waitForBgrewriteaof r
                assert_equal ok [s aof_last_bgrewrite_status]
                set base [get_base_aof_path r]
                if {$rewrite eq "yes"} {
                    assert_match "*.base.rdb" $base
                } else {
                    assert_match "*.base.aof" $base
                    gp_assert_realized [gp_aof_commands $base]
                    # A gate is one operation there: the gate as it is.
                    set ops [gp_aof_read $base]
                    foreach key $keys {
                        assert_equal 1 [llength [dict get $ops $key]] "operations the base file holds for $key"
                        assert_equal RESTORE [lindex [dict get $ops $key] 0 2]
                    }
                }
            }
            set story [gp_story_answers $story]
            after $::gp_wait2
            set story [gp_story_lost $story]
            # What the file says happened since it was last rewritten.
            gp_aof_flush
            set path [get_last_incr_aof_path r]
            gp_assert_realized [gp_aof_commands $path]
            set ops [gp_aof_read $path]
            foreach key $keys {
                if {![dict exists $ops $key]} continue
                set last 0
                foreach op [dict get $ops $key] {
                    set name [lindex $op 2]
                    set clock [lindex $op 3]
                    assert {$name in {CREATE PREDICT LEARN ADD PAUSE RESUME TICK RESET RECOVER}}
                    # The clock of a gate does not go back.
                    assert {[string is wide -strict $clock] && $clock >= $last}
                    set last $clock
                }
            }
            # The gate that answered two requests by itself wrote nothing
            # about them but that time had passed, twice.
            set ticks {}
            foreach op [dict get $ops $p:tick:s] { lappend ticks [lindex $op 2] }
            if {$rewrite eq "none"} {
                assert_equal [concat CREATE [lrepeat 45 PREDICT] [lrepeat 36 LEARN] TICK TICK] $ticks
            } else {
                assert_equal TICK $ticks
            }
            foreach key $keys { set t_info($key) [gp_info $key] }
            set before [gp_digests $keys]
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $keys]
            gp_assert_all_fit
            foreach key $keys { gp_assert_came_back $key $t_info($key) aof }
            # The gates of gp_states recover at their first write and prove
            # themselves again, and the file holds that too.
            dict for {key meta} $z { gp_recovers $key $meta }
            set before [gp_digests $keys]
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $keys]
            gp_assert_all_fit
        }
    }

    gp_test {AOF: gates at their limits have the digests they had after DEBUG LOADAOF, after a rewrite with and without an RDB preamble, and after they were used} {
        gp_begin 434
        set lim [gp_limits al]
        set lkeys [dict keys [dict get $lim gates]]
        set before [gp_digests $lkeys]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests $lkeys]
        gp_limits_intact $lim
        foreach preamble {yes no} {
            r config set aof-use-rdb-preamble $preamble
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            if {$preamble eq "no"} {
                # A gate is one operation there: the gate as it is.
                set ops [gp_aof_read [get_base_aof_path r]]
                foreach key $lkeys {
                    assert_equal 1 [llength [dict get $ops $key]] "operations the base file holds for $key"
                    assert_equal RESTORE [lindex [dict get $ops $key] 0 2]
                }
            }
            assert_equal OK [r debug loadaof]
            assert_equal $before [gp_digests $lkeys]
            gp_limits_intact $lim
        }
        gp_assert_all_fit
        # Two of them are used from here on (the others have tests of their
        # own): the first write of each, and whatever it does then, is in
        # the file as it happened.
        assert_equal 3 [r del al:next al:count al:own]
        set problems {}
        foreach key {al:all al:snap} { lappend problems {*}[gp_use_for_good $key $key] }
        if {[llength $problems]} { fail [join $problems "\n"] }
        # For the gate whose clock is at its last moment, with that clock.
        gp_aof_flush
        set ops [gp_aof_ops [get_last_incr_aof_path r] al:all clocks]
        assert {[llength $ops] > 50}
        assert_equal RECOVER [lindex $ops 0 0]
        foreach op $ops {
            assert_equal $::gp_clock_max [lindex $op 1] "the clock of [lindex $op 0] of al:all in the file"
        }
        set before [gp_digests {al:all al:snap}]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests {al:all al:snap}]
        gp_assert_all_fit
        # And through the snapshot a rewrite makes of them.
        r config set aof-use-rdb-preamble yes
        r bgrewriteaof
        waitForBgrewriteaof r
        assert_equal ok [s aof_last_bgrewrite_status]
        assert_equal OK [r debug loadaof]
        assert_equal $before [gp_digests {al:all al:snap}]
        assert_equal 2 [r del al:all al:snap]
    }

    gp_test {AOF: what a gate does that was restored from where the clock is ahead is in the file as it happened} {
        gp_begin 433
        r config set rdbcompression no
        gp_score a:skew 0.2 200
        set ids [gp_right a:skew 1]
        lassign [gp_dump_split [r dump a:skew]] head blob version
        # The clock of the gate as of its last write: in its ledger, after
        # the random number its ids start with and the time it was created.
        set epoch [binary format w 0x[lindex [split [lindex $ids 0] -] 0]]
        set at [expr {[string first $epoch $blob] + 16}]
        assert {$at > 16}
        binary scan [string range $blob $at [expr {$at + 7}]] w clock
        set now [clock milliseconds]
        assert {$clock <= $now + 1000 && $clock > $now - 60000}
        # The same gate as a server would have dumped it whose clock is an
        # hour ahead of this one's.
        set payload [gp_dump_join $head [string replace $blob $at [expr {$at + 7}] [binary format w [expr {$clock + 3600000}]]] $version]
        assert_equal OK [r restore a:skew 0 $payload replace]
        # One answer in time and one too late for a feedback timeout of a
        # fifth of a second: the gate's clock goes on from where it was,
        # at the pace of this server.
        set ids [gp_right a:skew 2]
        assert_equal 1 [r glearn a:skew [lindex $ids 1] CORRECT 1]
        after 300
        assert_equal 0 [r glearn a:skew [lindex $ids 0] CORRECT 1]
        assert_equal 1 [gp_info a:skew missing]
        assert_equal 2 [gp_info a:skew calibration-samples]
        # The file says so with the clock of the gate, an hour ahead: when
        # it came back, its two requests, the answer and the late answer.
        gp_aof_flush
        set ops [gp_aof_ops [get_last_incr_aof_path r] a:skew clocks]
        set ops [lrange $ops [lsearch -index 0 $ops RECOVER] end]
        set names {}
        set last 0
        foreach op $ops {
            lassign $op name stamp
            lappend names $name
            assert {$stamp >= $clock + 3600000 && $stamp < $clock + 3600000 + 60000 && $stamp >= $last}
            set last $stamp
        }
        assert_equal {RECOVER PREDICT PREDICT LEARN LEARN} $names
        assert {$last >= [lindex $ops 0 1] + 290}
        set digest [debug_digest_value a:skew]
        assert_equal OK [r debug loadaof]
        assert_equal $digest [debug_digest_value a:skew]
        gp_assert_all_fit
    }

    gp_test {AOF: a snapshot that takes over, and gates that stop by themselves, are the same after DEBUG LOADAOF} {
        gp_begin 420
        set took [gp_takeover a:took]
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
        gp_assert_all_fit
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
        gp_assert_all_fit
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
        gp_restart
        assert_equal $before [gp_digests $keys]
        assert_equal "not a gate" [r get z:string]
        gp_assert_all_fit
        foreach key $keys { gp_assert_came_back $key $a_info($key) aof }
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
        gp_assert_all_fit
    }

    gp_test {redis-check-aof accepts the files} {
        gp_aof_flush
        set manifest [file join [lindex [r config get dir] 1] [lindex [r config get appenddirname] 1] \
                          [lindex [r config get appendfilename] 1]$::manifest_suffix]
        catch {exec src/redis-check-aof $manifest} output
        assert_match "*All AOF files and manifest are valid*" $output
    }
}

# ==================== AOF: a file no gate has written ======================

start_server [list tags {"modules external:skip"} overrides [list loadmodule $gatemodule appendonly yes]] {

    gp_test {AOF: operations in the file that do not fit their gate are skipped and counted: the gates stay as they were and can be saved and loaded} {
        gp_begin 440
        set ::gp_file_misfits 0
        gp_score k:s
        set ids [gp_right k:s 3]
        assert_equal 1 [r glearn k:s [lindex $ids 1] CORRECT 1]
        gp_classifier k:c
        gp_teach k:c 20
        set asked [gp_collect k:c 3]
        r set k:string "not a gate"
        # What the server wrote for the last request of each gate: the
        # clock, the draw, what the snapshot that answers and the one that
        # is tested made of it, the number of the request, and for a
        # classifier its text.
        gp_aof_flush
        set path [get_last_incr_aof_path r]
        set k_request [lindex [gp_aof_ops $path k:s commands] end-1]
        set k_requestc [lindex [gp_aof_ops $path k:c commands] end]
        assert_equal PREDICT [lindex $k_request 2]
        assert_equal PREDICT [lindex $k_requestc 2]
        assert_equal 12 [llength $k_request] "words of the operation a score gate's request is"
        assert_equal 14 [llength $k_requestc] "words of the operation a classifier's request is"
        assert_equal 3 [lindex $k_request 11] "the number of the third request"
        assert_equal TEXT [lindex $k_requestc 12]
        set now [lindex $k_requestc 3]
        set u [lindex $k_request 4]
        set id [lindex $ids 0]
        set idc [lindex $asked 0 0]
        # Bits of doubles: 0, 0.5, 1, 2, a NaN and infinity.
        set zero 0000000000000000
        set half 3fe0000000000000
        set one 3ff0000000000000
        set two 4000000000000000
        set nan 7ff8000000000000
        set inf 7ff0000000000000
        set lineage 0123456789abcdef
        set epoch fedcba9876543210
        set misfits [list \
            [list _GATE.APPLY k:s FLY $now] \
            [list _GATE.APPLY k:s TICK] \
            [list _GATE.APPLY k:s TICK soon] \
            [list _GATE.APPLY k:s TICK -1] \
            [list _GATE.APPLY k:s TICK 9223372036854775807] \
            [list _GATE.APPLY k:s TICK [expr {$::gp_clock_max + 1}]] \
            [list _GATE.APPLY k:s PAUSE [expr {$::gp_clock_max + 1}]] \
            [list _GATE.APPLY k:s RESET [expr {$::gp_clock_max + 1}] $epoch] \
            [list _GATE.APPLY k:s TICK $now more] \
            [list _GATE.APPLY k:s PAUSE] \
            [list _GATE.APPLY k:s RESUME $now more] \
            [list _GATE.APPLY k:s LEARN $now $id] \
            [list _GATE.APPLY k:s LEARN $now not-an-id 1] \
            [list _GATE.APPLY k:s LEARN $now $id 2] \
            [list _GATE.APPLY k:s LEARN $now $id -1] \
            [list _GATE.APPLY k:s LEARN $now $id yes] \
            [list _GATE.APPLY k:c LEARN $now $idc 2] \
            [list _GATE.APPLY k:c LEARN $now $idc 64] \
            [list _GATE.APPLY k:s ADD $now TEXT hello LABEL spam] \
            [list _GATE.APPLY k:c ADD $now TEXT hello LABEL eggs] \
            [list _GATE.APPLY k:c ADD $now TEXT hello] \
            [list _GATE.APPLY k:c ADD $now TEXT hello FEATURES 1 depth nan LABEL spam] \
            [list _GATE.APPLY k:s RESET $now] \
            [list _GATE.APPLY k:s RESET $now not-hex] \
            [list _GATE.APPLY k:s RECOVER $now 123] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half] \
            [list _GATE.APPLY k:s PREDICT $now $one 0 -1 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $nan 0 -1 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $nan 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $inf 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $two 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 0 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 0 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 64 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -2 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:s PREDICT soon $u 0 -1 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half 0] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half 3] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half next] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half $::gp_count_max] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half 9223372036854775807] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half -100] \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 1 -1 $half 100 TEXT "hello world"] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 0 $half 100] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 0 $half 100 TEXT hello FEATURES 1 depth] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 0 $half 100 TEXT hello EXTRA] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 0 $half 2 TEXT hello] \
            [list _GATE.APPLY k:string TICK $now] \
            [list _GATE.APPLY k:string LEARN $now $id 1] \
            [list _GATE.APPLY k:string PREDICT $now $u 0 -1 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:missing TICK $now] \
            [list _GATE.APPLY k:missing PAUSE $now] \
            [list _GATE.APPLY k:missing RESET $now $epoch] \
            [list _GATE.APPLY k:missing LEARN $now $id 1] \
            [list _GATE.APPLY k:missing PREDICT $now $u 0 -1 $zero 1 -1 $half 100] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 0 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 4294967296 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 4294967297 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 0 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 -1 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 604800001 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 9223372036854775807 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000000001 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE [expr {$::gp_clock_max + 1}] $lineage $epoch 1000 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 60000 ALPHA 2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 60000 ALPHA 0.2] \
            [list _GATE.APPLY k:new CREATE $now $lineage not-hex 1000 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE -5 $lineage $epoch 1000 60000 ALPHA 0.2 SCORE] \
            [list _GATE.APPLY k:new CREATE $now $lineage $epoch 1000 60000] \
            [list _GATE.APPLY k:new RESTORE] \
            [list _GATE.APPLY k:new RESTORE garbage] \
            [list _GATE.APPLY k:new RESTORE GATE] \
        ]
        set before [gp_digests {k:s k:c}]
        assert_equal 0 [gp_mismatches]
        assert_equal OK [gp_aof_load_counting $misfits [llength $misfits]]
        assert_equal PONG [r ping]
        # Nothing happened to the gates, nothing was created or replaced.
        assert_equal $before [gp_digests {k:s k:c}]
        assert_equal "not a gate" [r get k:string]
        assert_equal 0 [r exists k:missing k:new]
        # The server has counted them, said so in its log for the first
        # ten, and said for each gate of a rewritten file that it is lost.
        assert_equal [llength $misfits] [gp_mismatches] "operations that did not fit their gate"
        assert_equal 10 [count_log_message 0 "did not fit"]
        assert_equal 2 [count_log_message 0 "cannot be restored and is lost"]
        # The gates are gates: Redis can save and load them, and they work.
        foreach key {k:s k:c} {
            assert_equal OK [r restore $key:copy 0 [r dump $key]]
            assert_equal 1 [r del $key:copy]
        }
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests {k:s k:c}]
        # Three more look right until the gate is asked: a request that
        # says no snapshot was being tested, and requests of the classifier
        # whose guess is a label the gate does not have, or none. Nothing
        # changes: the gate gives back the number such a request came with
        # when it finds that it cannot record it (CHANGES.md section 9).
        set tried [list \
            [list _GATE.APPLY k:s PREDICT $now $u 0 -1 $zero 0 -1 $half 100] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 2 $half 100 TEXT hello] \
            [list _GATE.APPLY k:c PREDICT $now $u 0 -1 $zero 1 -1 $half 100 TEXT hello] \
        ]
        foreach key {k:s k:c} { set k_info($key) [gp_info $key] }
        assert_equal OK [gp_aof_load_counting $tried [llength $tried]]
        assert_equal $before [gp_digests {k:s k:c}]
        foreach key {k:s k:c} {
            assert_equal $k_info($key) [gp_info $key] "GINFO of $key"
            assert_equal OK [r restore $key:copy 0 [r dump $key]]
            assert_equal 1 [r del $key:copy]
        }
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests {k:s k:c}]
        # The gates work, and number their next request as if nothing had
        # been in the file: each was asked three requests, this is the
        # fourth.
        foreach key {k:s k:c} {
            lassign [gp_request [gp_info $key kind] 1] question answer
            lassign [r gpredict $key {*}$question] action next
            assert_equal ABSTAIN $action
            assert_equal 4 [lindex [split $next -] 1] "number of the next request of $key"
            assert_equal 1 [r glearn $key $next {*}$answer]
        }
        assert_equal 0 [r glearn k:s $id CORRECT 1]
    }

    gp_test {AOF: operations written by hand that do fit are applied} {
        # The test before means something only if the server reads what a
        # test appends to its file. Follows from it: k:s and k:c.
        gp_begin 441
        lassign [r gpredict k:s SCORE 0.9] action id
        lassign [split $id -] epoch number
        gp_aof_flush
        set path [get_last_incr_aof_path r]
        set k_request [lindex [gp_aof_ops $path k:s commands] end]
        assert_equal PREDICT [lindex $k_request 2]
        assert_equal $number [lindex $k_request 11]
        set now [lindex $k_request 3]
        set info [gp_info k:s]
        # The same request once more, as request number 200; an answer for
        # it; a pause; an example for the classifier; and a gate with the
        # largest snapshot step and the longest feedback timeout there are
        # (a thousand million examples, a week).
        set learned [gp_info k:c learned]
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY k:s {*}[lrange $k_request 2 10] 200] \
            [list _GATE.APPLY k:s LEARN $now $epoch-200 1] \
            [list _GATE.APPLY k:s PAUSE $now] \
            [list _GATE.APPLY k:c ADD $now TEXT "buy cheap pills now" LABEL spam] \
            [list _GATE.APPLY k:max CREATE $now 0123456789abcdef fedcba9876543210 1000000000 604800000 ALPHA 0.2 SCORE] \
        ] 0]
        # (The file was read again from its start: what did not fit then
        # does not fit now, and nothing else.)
        assert_equal [expr {$learned + 1}] [gp_info k:c learned]
        assert_equal score [gp_info k:max kind]
        # The gate is shown as it will be after its first write, so of what
        # the operations did only the pause is to be seen ...
        assert_equal PAUSED [gp_info k:s state]
        assert_equal [expr {[dict get $info requests] + 1}] [gp_info k:s requests]
        # ... and that the next request has a number after 200.
        assert_equal OK [r gresume k:s]
        lassign [r gpredict k:s SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 201 [lindex [split $id -] 1]
        assert_equal OK [r debug reload]
    }

    gp_test {AOF: a request number or a clock in the file beyond the end of its range is skipped and counted; the last moment of the clock is applied, and the gate can still be saved and loaded} {
        # Follows from the tests before.
        gp_begin 442
        gp_score k:far
        gp_right k:far 2
        gp_score k:end
        set ends [gp_right k:end 2]
        gp_aof_flush
        set k_request [lindex [gp_aof_ops [get_last_incr_aof_path r] k:far commands] end]
        assert_equal 2 [lindex $k_request 11]
        set far [debug_digest_value k:far]
        set far_requests [gp_info k:far requests]
        # For one gate two requests with numbers that do not exist: the
        # requests of a gate are numbered from 1 to 2^62 - 1. For the other
        # the passing of time, up to the last moment a gate's clock has (a
        # week before the largest number there is), and a millisecond more.
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY k:far {*}[lrange $k_request 2 10] $::gp_count_max] \
            [list _GATE.APPLY k:far {*}[lrange $k_request 2 10] 9223372036854775807] \
            [list _GATE.APPLY k:end TICK $::gp_clock_max] \
            [list _GATE.APPLY k:end TICK [expr {$::gp_clock_max + 1}]] \
        ] 3]
        assert_equal PONG [r ping]
        # The requests were skipped: the gate is as it was, and has not
        # counted them either.
        assert_equal $far [debug_digest_value k:far]
        assert_equal $far_requests [gp_info k:far requests] "requests of k:far"
        assert_equal $far_requests [gp_info k:far abstained] "abstained of k:far"
        # Time has passed for the other: its two requests are lost.
        assert_equal 2 [gp_info k:end missing] "missing of k:end"
        # What is in memory are gates Redis can save and load, now and
        # after they were used.
        foreach key {k:far k:end} {
            assert_equal {} [gp_unloadable $key] "$key, as the file left it"
        }
        assert_equal OK [r debug reload]
        assert_equal $far [debug_digest_value k:far]
        # The gate that was sent requests without a number numbers its
        # next request 3.
        lassign [r gpredict k:far SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal 3 [lindex [split $id -] 1] "number of the next request of k:far"
        # The clock of the other stays at its last moment, whatever time
        # it is: its requests are in the file with that clock, and it can
        # be saved and loaded for good.
        assert_equal 0 [r glearn k:end [lindex $ends 0] CORRECT 1]
        lassign [r gpredict k:end SCORE 0.9] action id
        assert_equal ABSTAIN $action
        assert_equal {} [gp_unloadable k:end] "k:end with a request waiting"
        after 20
        assert_equal 1 [r glearn k:end $id CORRECT 1] "feedback for a fresh id on k:end"
        gp_right k:end 2
        assert_equal {} [gp_unloadable k:end] "k:end, 20 ms later"
        gp_aof_flush
        set ops [gp_aof_ops [get_last_incr_aof_path r] k:end clocks]
        set ops [lrange $ops [lsearch -index 0 $ops RECOVER] end]
        set names {}
        foreach op $ops {
            lappend names [lindex $op 0]
            assert_equal $::gp_clock_max [lindex $op 1] "the clock of k:end in the file"
        }
        assert_equal {RECOVER LEARN PREDICT LEARN PREDICT PREDICT} $names
        set before [gp_digests {k:far k:end}]
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests {k:far k:end}]
        assert_equal OK [gp_aof_load_counting {} 0]
        assert_equal $before [gp_digests {k:far k:end}]
    }

    gp_test {AOF: what the file does to a gate at its limits leaves a gate that can be saved and loaded} {
        # Follows from the tests before. A file can hold a gate that no
        # gate has written, as a rewrite would have written it, and go on
        # from there: the gate is not written to by a client first, so it
        # does not start over, and what the file says happens to counts
        # that are at 2^62 already (CHANGES.md 9d).
        gp_begin 443
        set lim [gp_limits kl]
        assert_equal 5 [r del {*}[dict keys [dict get $lim gates]]]
        set o [dict get $lim opened]
        # The gate "all" of gp_limits, a hundred seconds before the last
        # moment of its clock, and with one disagreement in each of its
        # blocks of spot checks instead of nearly as many as a block holds
        # (one more disagreement must not stop it).
        set early [expr {$::gp_clock_max - 100000}]
        set values [dict get $lim values all]
        lappend values created $early clock $early serv.frozen $early serv.promoted $early
        for {set i 0} {$i < 4} {incr i} { lappend values req.$i.created $early }
        foreach scale {0 1 2} { lappend values serv.drift.0.wrong$scale 1 serv.drift.0.lost$scale 0 }
        lassign [gp_dump_split [gp_payload_with $o {*}$values]] head blob version
        lassign [dict get $lim ids] first second third fourth
        set next [gp_payload_get $o next]
        set max $::gp_count_max
        # One more spot check, as the server writes a request: the clock,
        # the draw (0.5), the guess of the snapshot that answers (no label,
        # score 0.9), no snapshot that is tested, and the request number.
        set request [list PREDICT $early 3fe0000000000000 1 -1 3feccccccccccccd 0 -1 0000000000000000 $next]
        # For one such gate the file goes on with the answers of the first
        # spot check, which agrees, and of the fourth, which disagrees (so
        # all four are counted), and one more spot check, the first of a
        # new block. For another, with one more spot check and the last
        # moment of the clock: its three spot checks without an answer are
        # lost; the last was asked after the last answer and is the 32nd
        # lost answer in a row (the gate came with 31): the gate stops.
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY kl:work RESTORE $blob] \
            [list _GATE.APPLY kl:work LEARN $early $first 1] \
            [list _GATE.APPLY kl:work LEARN $early $fourth 0] \
            [list _GATE.APPLY kl:work {*}$request] \
            [list _GATE.APPLY kl:run RESTORE $blob] \
            [list _GATE.APPLY kl:run {*}$request] \
            [list _GATE.APPLY kl:run TICK $::gp_clock_max] \
        ] 0]
        assert_equal PONG [r ping]
        set problems {}
        foreach key {kl:work kl:run} {
            assert_equal gate-type [r type $key]
            gp_check_info $key [gp_info $key]
            set problem [gp_unloadable $key]
            if {$problem ne {}} { lappend problems "after a gate at its limits and operations on it in the AOF $key cannot be restored from its own DUMP: $problem" }
        }
        if {[llength $problems]} {
            r del kl:work kl:run
            fail [join $problems "\n"]
        }
        # The first gate: what it has counted beyond 2^62 is saved as 2^62
        # (the proof and the answered spot checks of its snapshot, its spot
        # checks and their disagreements, the number of the spot check that
        # waits), and that spot check is alone in its blocks.
        set w [gp_payload_open [gp_dump_plain kl:work]]
        assert_equal 1 [gp_payload_get $w count] "requests kl:work holds"
        assert_equal $next [gp_payload_get $w req.0.seq]
        foreach name {serv.samples serv.errors serv.audits serv.audit_errors serv.region.0.0.n serv.region.0.0.errors
                      serv.drift.0.asked serv.drift.0.errors req.0.spot stats.learned stats.observations} {
            assert_equal $max [gp_payload_get $w $name] "$name kl:work is saved with"
        }
        foreach name {wrong0 wrong1 wrong2 lost0 lost1 lost2} {
            assert_equal 0 [gp_payload_get $w serv.drift.0.$name] "$name of the new block of kl:work"
        }
        assert_equal 1 [gp_payload_get $w serv]
        # The second gate has stopped, having lost three answers more and
        # stopped once more than 2^62 times.
        # (What GINFO shows of a count that is beyond 2^62, until the gate
        # is saved and loaded, is its own business: at least 2^62.)
        set info [gp_info kl:run]
        assert {[dict get $info missing] >= $max && [dict get $info missing] <= $max + 3}
        assert {[dict get $info revocations] >= $max && [dict get $info revocations] <= $max + 1}
        set w [gp_payload_open [gp_dump_plain kl:run]]
        assert_equal {0 0 1 0 0} [list [gp_payload_get $w serv] [gp_payload_get $w cand] [gp_payload_get $w blind] \
                                       [gp_payload_get $w missing_run] [gp_payload_get $w count]] \
            "kl:run: whether a snapshot answers or is tested, whether the gate has stopped, its run of missing answers, its requests"
        assert_equal $::gp_clock_max [gp_payload_get $w clock] "the clock kl:run is saved with"
        # Saved and loaded: the same gates, and no count beyond its limit.
        set before [gp_digests {kl:work kl:run}]
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests {kl:work kl:run}]
        set info [gp_info kl:run]
        assert_equal $max [dict get $info missing] "missing of kl:run"
        assert_equal $max [dict get $info revocations] "revocations of kl:run"
        foreach key {kl:work kl:run} {
            set w [gp_payload_open [gp_dump_plain $key]]
            foreach name [gp_blob_counters [dict get $w fields]] {
                if {[gp_payload_get $w $name] > $max} { fail "$key is saved with $name [gp_payload_get $w $name]" }
            }
        }
        # The file goes on for the first gate too: at the last moment of
        # the clock its spot check is lost. That is one lost answer, no
        # run: the gate goes on answering.
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY kl:work TICK $::gp_clock_max] \
        ] 0]
        assert_equal PONG [r ping]
        assert {[gp_info kl:work missing] >= $max && [gp_info kl:work missing] <= $max + 1}
        set w [gp_payload_open [gp_dump_plain kl:work]]
        assert_equal {1 0 1 0} [list [gp_payload_get $w serv] [gp_payload_get $w blind] [gp_payload_get $w missing_run] \
                                     [gp_payload_get $w count]] \
            "kl:work: whether a snapshot answers, whether the gate has stopped, its run of missing answers, its requests"
        assert_equal 1 [gp_payload_get $w serv.drift.0.lost0] "lost answers of the new block of kl:work"
        # They can be used, and saved and loaded for good.
        foreach key {kl:work kl:run} {
            lappend problems {*}[gp_use_for_good $key "$key, a gate the AOF left at its limits"]
        }
        if {[llength $problems]} {
            r del kl:work kl:run
            fail [join $problems "\n"]
        }
        set before [gp_digests {kl:work kl:run}]
        assert_equal OK [r debug reload]
        assert_equal $before [gp_digests {kl:work kl:run}]
        assert_equal OK [gp_aof_load_counting {} 0]
        assert_equal $before [gp_digests {kl:work kl:run}]
    }

    gp_test {AOF: an operation of fewer than three words in the file is skipped and counted, and the server goes on} {
        # Follows from the tests before. The loader of the AOF does not
        # check how many words a command has, and a server that is started
        # the way the tests start it stops when a command of its AOF is
        # answered with an error.
        assert_equal panic [lindex [r config get propagation-error-behavior] 1]
        set keys {k:s k:c k:far k:end}
        set before [gp_digests $keys]
        set errors [s unexpected_error_replies]
        set said [count_log_message 0 "did not fit"]
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY] \
            [list _GATE.APPLY k:s] \
        ] 2]
        assert_equal PONG [r ping]
        assert {[is_alive [srv pid]]}
        # No command of the file was answered with an error ...
        assert_equal $errors [s unexpected_error_replies] "error replies to commands of the AOF"
        assert_equal 0 [count_log_message 0 "sending an error to its"]
        # ... and nothing happened to the gates.
        assert_equal $before [gp_digests $keys]
        # The file is read to its end: a gate that is created after the two
        # operations is there.
        set now [clock milliseconds]
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY k:after CREATE $now 0123456789abcdef fedcba9876543210 1000 60000 ALPHA 0.2 SCORE] \
        ] 0]
        assert_equal score [gp_info k:after kind]
        assert_equal $before [gp_digests $keys]
        assert_equal ABSTAIN [gp_ask k:s score]
        assert_equal 0 [count_log_message 0 "REDIS BUG REPORT"]
    }

    gp_test {AOF: a request in the file with the last request number there is leaves a gate that can be saved and loaded} {
        # Follows from the tests before, and is the last to use this file:
        # what the file holds now cannot be undone.
        gp_begin 444
        gp_score k:near
        gp_right k:near 2
        gp_aof_flush
        set k_request [lindex [gp_aof_ops [get_last_incr_aof_path r] k:near commands] end]
        assert_equal 2 [lindex $k_request 11]
        # The same request once more, as request number 2^62 - 1. It fits:
        # the gate records it, and has no request number left.
        assert_equal OK [gp_aof_load_counting [list \
            [list _GATE.APPLY k:near {*}[lrange $k_request 2 10] [expr {$::gp_count_max - 1}]] \
        ] 0]
        assert_equal PONG [r ping]
        set problem [gp_unloadable k:near]
        # (So that nothing trips over it: a server that saves such a gate
        # cannot load its own file.)
        r del k:near
        if {$problem ne {}} {
            fail "after a request numbered 2^62 - 1 in the AOF, which is applied, the gate cannot be restored from its own DUMP: $problem"
        }
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

    gp_test {DUMP / RESTORE: gates with late and lost answers, gates that stopped and gates that resumed, gates with spot checks and with a takeover undecided: the copy has the digest of the original and starts over, the original goes on} {
        gp_begin 505
        r flushall
        r config set rdbcompression no
        set z [gp_states e]
        # The payloads are kept for the tests of damaged payloads below.
        set ::gp_dumped {}
        dict for {key meta} $z {
            set info [gp_info $key]
            set digest [debug_digest_value $key]
            set payload [r dump $key]
            dict set ::gp_dumped $key [dict create payload $payload info $info meta $meta]
            assert_equal OK [r restore $key:copy 0 $payload]
            assert_equal gate-type [r type $key:copy]
            assert_equal $digest [debug_digest_value $key:copy] "digest of the copy of $key"
            assert_equal [dict get $info memory] [gp_info $key:copy memory] "memory of the copy of $key"
            gp_assert_same_bytes $payload [r dump $key:copy] "DUMP of $key and of its copy"
            gp_assert_came_back $key:copy $info
            # Neither DUMP nor the copy touched the original.
            assert_equal $info [gp_info $key] "GINFO of $key"
            assert_equal $digest [debug_digest_value $key] "digest of $key"
        }
        # The original goes on where it was.
        dict for {key meta} $z {
            set old [dict get $meta old]
            switch [dict get $meta state] {
                wait {
                    # The answer the others waited for arrives: it is
                    # counted, and so is the one behind it.
                    set samples [gp_info $key calibration-samples]
                    assert_equal 1 [gp_answer $key [lindex $old 0]] "feedback for $key"
                    assert_equal [expr {$samples + 2}] [gp_info $key calibration-samples] "proof of $key"
                    assert_equal 2 [gp_info $key pending] "pending of $key"
                }
                prov {
                    # The late answer is still wanted there.
                    assert_equal 1 [gp_answer $key [dict get $meta late]] "late feedback for $key"
                    assert_equal 0 [gp_info $key pending] "pending of $key"
                    assert_equal 0 [gp_info $key missing] "missing of $key"
                }
                blind {
                    # It has stopped, and wants feedback for whatever it is
                    # asked. Its first answer starts the next snapshot.
                    set asked [gp_collect $key 1]
                    if {$key eq "e:blind:s"} {
                        # A gate without a snapshot and with one request:
                        # the smallest payload there is that has both.
                        set ::gp_tiny [r dump $key]
                    }
                    set asked [concat $asked [gp_collect $key 2]]
                    assert_equal 0 [gp_info $key candidate] "candidate of $key"
                    assert_equal 1 [gp_answer $key [lindex $asked 1]] "feedback for $key"
                    assert_equal [expr {[dict get $meta used] + 1}] [gp_info $key candidate] "candidate of $key"
                }
                spot {
                    # It is not written to here (its feedback timeout is
                    # shorter than a busy machine may take to get this
                    # far): the snapshot that answers is still the one,
                    # with its spot checks.
                    set info [gp_info $key]
                    assert_equal [dict get $meta used] [dict get $info generation] "generation of $key"
                    assert_equal 4 [dict get $info audit-samples] "audit-samples of $key"
                    assert_equal 2 [dict get $info audit-errors] "audit-errors of $key"
                    assert_equal 2 [dict get $info pending] "pending of $key"
                }
                behind {
                    # The three answers arrive: the second snapshot has
                    # proven a label now, and the first still answers more.
                    set b [dict get $meta behind]
                    while {[llength [dict get $b waiting]]} { gp_behind_answer b }
                    assert {[dict get $b ham] + [dict get $b odd] >= [gp_need 2 2]}
                    assert_equal 1 [gp_info $key generation] "the snapshot of $key that answers"
                    assert_equal 2 [gp_info $key candidate] "candidate of $key"
                }
                default {
                    # The snapshot that is being tested is still the one.
                    assert_equal [dict get $meta used] [gp_info $key candidate] "candidate of $key"
                }
            }
        }
        # The copies start over: none of the requests is theirs, and they
        # prove themselves with a snapshot number the original never used.
        dict for {key meta} $z { gp_recovers $key:copy $meta }
        # The original that had just resumed is proven with the number it
        # had; its copy took the next one.
        gp_prove e:res:s
        assert_equal [dict get $z e:res:s used] [gp_info e:res:s generation]
        assert_equal [expr {[dict get $z e:res:s used] + 1}] [gp_info e:res:s:copy generation]
    }

    gp_test {DUMP: a serialized gate is laid out as gate_codec.c says, and holds what the gate holds: spot checks and their blocks, the count a takeover is decided on, what a snapshot left to the teacher, the run of missing answers and the mark of the last answer} {
        # Follows from the test before: the payloads it has kept, of gates
        # whose story is known. (The tests of damaged payloads below find
        # the fields of a serialized gate by this layout.)
        assert_equal 23 [dict size $::gp_dumped]
        set number {{pair} { lindex [split [lindex $pair 0] -] 1 }}
        set now [clock milliseconds]
        dict for {key dumped} $::gp_dumped {
            set info [dict get $dumped info]
            set meta [dict get $dumped meta]
            set old [dict get $meta old]
            # (This fails unless the layout ends where the gate ends.)
            set o [gp_payload_open [dict get $dumped payload]]
            set lay_open($key) $o
            # The configuration.
            assert_equal 1 [gp_payload_get $o codec] "format version of $key"
            assert_equal [expr {[dict get $info kind] eq "score"}] [gp_payload_get $o kind] "kind of $key"
            foreach field {alpha delta audit} {
                assert {[gp_payload_get $o $field] == [dict get $info $field]}
            }
            set classes [dict get $info classes]
            assert_equal [llength $classes] [gp_payload_get $o classes] "classes of $key"
            for {set i 0} {$i < [llength $classes]} {incr i} {
                assert_equal [lindex $classes $i] [gp_payload_get $o label.$i] "label $i of $key"
                # GINFO shows 0 for a label the gate never answers with.
                set alpha [lindex [dict get $info alphas] $i]
                assert_equal [expr {$alpha == 0}] [gp_payload_get $o defer.$i] "whether $key never answers with label $i"
                if {$alpha != 0} { assert {[gp_payload_get $o classalpha.$i] == $alpha} }
            }
            # The ledger: the number its ids start with, its clock, the
            # snapshots it has taken, its pause.
            assert_equal [lindex [split [lindex $old 0 0] -] 0] [format %016x [gp_payload_get $o epoch]] "the number the ids of $key start with"
            set clock [gp_payload_get $o clock]
            assert {$clock <= $now + 1000 && $clock > $now - 120000 && [gp_payload_get $o created] <= $clock}
            assert_equal [dict get $meta used] [gp_payload_get $o generations] "snapshots $key has taken"
            assert_equal [expr {[dict get $info state] eq "PAUSED"}] [gp_payload_get $o paused] "the pause of $key"
            # The model and the snapshots.
            assert_equal [expr {[llength $classes] > 0}] [gp_payload_get $o trainer] "whether $key has a model"
            if {[llength $classes]} {
                assert_equal [dict get $info learned] [gp_payload_get $o trainer.updates] "learned of $key"
            }
            foreach {g field} {serv generation cand candidate} {
                set snapshot [dict get $info $field]
                assert_equal [expr {$snapshot != 0}] [gp_payload_get $o $g] "whether $key has a snapshot ($field)"
                if {$snapshot} { assert_equal $snapshot [gp_payload_get $o $g.gen] "$field of $key" }
            }
            if {[dict get $info generation]} {
                assert_equal [dict get $info audit-samples] [gp_payload_get $o serv.audits] "audit-samples of $key"
                assert_equal [dict get $info audit-errors] [gp_payload_get $o serv.audit_errors] "audit-errors of $key"
            }
            set tested [expr {[dict get $info candidate] ? "cand" : [dict get $info generation] ? "serv" : ""}]
            if {$tested ne ""} {
                assert_equal [dict get $info calibration-samples] [gp_payload_get $o $tested.samples] "calibration-samples of $key"
            }
            # The requests, oldest first, and the number of the next one.
            set next [gp_payload_get $o next]
            assert_equal [expr {[apply $number [lindex $old end]] + 1}] $next "the next request number of $key"
            set unanswered 0
            set last 0
            for {set i 0} {$i < [gp_payload_get $o count]} {incr i} {
                set seq [gp_payload_get $o req.$i.seq]
                assert {$seq > $last && $seq < $next}
                set last $seq
                assert {[gp_payload_get $o req.$i.created] <= $clock}
                if {!([gp_payload_get $o req.$i.flags] & 16)} { incr unanswered }
            }
            assert_equal [dict get $info pending] $unanswered "pending of $key"
            # The counters.
            foreach field {requests accepted audited abstained missing revocations} {
                assert_equal [dict get $info $field] [gp_payload_get $o stats.$field] "$field of $key"
            }
        }

        # What the final round of the core added (CHANGES.md section 9).
        # Whether the gate has stopped, its run of missing answers, and the
        # mark of the last answer: the number of the next request when it
        # arrived, 0 if none ever did.
        foreach {name stopped run mark} {
            wait:s  0 0 next
            lost:s  0 5 0
            mark:s  0 3 fifth
            blind:s 1 0 0
            blind:a 1 0 first
            res:s   0 0 next
            spot:a  0 0 next
        } {
            set o $lay_open(e:$name)
            set old [dict get $::gp_dumped e:$name meta old]
            switch $mark {
                next { set mark [gp_payload_get $o next] }
                first { set mark [apply $number [lindex $old 0]] }
                fifth { set mark [expr {[apply $number [lindex $old 4]] + 1}] }
            }
            assert_equal [list $stopped $run $mark] \
                [list [gp_payload_get $o blind] [gp_payload_get $o missing_run] [gp_payload_get $o answer_mark]] \
                "whether e:$name has stopped, its run of missing answers and the mark of its last answer"
        }

        # An answering gate numbers the spot checks of each label in the
        # order they are asked, and counts those that disagreed and those
        # whose answer was lost, for the open block of each length (all in
        # the first block: there are ten). What became of the ten, in the
        # order they were asked:
        set fates {lost wrong lost right lost lost waits wrong right waits}
        foreach name {spot:a spot:p spot:c} {
            set o $lay_open(e:$name)
            set dumped [dict get $::gp_dumped e:$name]
            set classes [dict get $dumped info classes]
            set old [dict get $dumped meta old]
            assert_equal 10 [llength $old]
            set rows [expr {max(1, [llength $classes])}]
            for {set row 0} {$row < $rows} {incr row} {
                set lay_asked($row) 0
                set lay_wrong($row) 0
                set lay_lost($row) 0
            }
            set waiting {}
            foreach spot $old fate $fates i {0 1 2 3 4 5 6 7 8 9} {
                # The label of a spot check: the one its snapshot guessed.
                set right [expr {$rows == 1 ? 1 : [lsearch -exact $classes [lindex $spot 1 1]]}]
                set other [expr {$rows == 1 ? 0 : [lsearch -exact $classes [lindex $spot 2 1]]}]
                set row [expr {$rows == 1 ? 0 : $right}]
                incr lay_asked($row)
                if {$fate eq "wrong"} { incr lay_wrong($row) }
                if {$fate eq "lost"} { incr lay_lost($row) }
                if {$i < 6} continue
                # The four asked last are still there: flags 1 (sent to the
                # teacher by the draw), 2 (the gate would have answered), 8
                # (proof for the snapshot that answers) and, once answered,
                # 16; the answer; the number as a spot check.
                lappend waiting [list $row $lay_asked($row) \
                    [expr {$fate eq "waits" ? 11 : 27}] [expr {$fate eq "waits" ? -1 : $fate eq "right" ? $right : $other}]]
            }
            assert_equal 4 [gp_payload_get $o count] "requests e:$name holds"
            set i 0
            foreach request $waiting {
                lassign $request row spot flags result
                assert_equal [list $spot $flags $result [expr {$rows == 1 ? -1 : $row}] [dict get $dumped info generation] 0] \
                    [list [gp_payload_get $o req.$i.spot] [gp_payload_get $o req.$i.flags] [gp_payload_get $o req.$i.result] \
                          [gp_payload_get $o req.$i.serv_label] [gp_payload_get $o req.$i.serv_gen] [gp_payload_get $o req.$i.cand_gen]] \
                    "request $i of e:$name: its number as a spot check, flags, answer, label, and the snapshots it is proof for"
                incr i
            }
            for {set row 0} {$row < $rows} {incr row} {
                set expected [list $lay_asked($row) $lay_wrong($row)]
                set stored [list [gp_payload_get $o serv.drift.$row.asked] [gp_payload_get $o serv.drift.$row.errors]]
                foreach scale {0 1 2} {
                    lappend expected $lay_wrong($row) $lay_lost($row)
                    lappend stored [gp_payload_get $o serv.drift.$row.wrong$scale] [gp_payload_get $o serv.drift.$row.lost$scale]
                }
                assert_equal $expected $stored "spot checks of label $row of e:$name: asked, disagreed, and disagreed and lost in the open block of each length"
            }
        }

        # A snapshot that is tested behind another keeps count of its proof
        # requests that the other one would answer: for each label and
        # threshold, those the other snapshot guessed that label for, at
        # least that sure.
        set o $lay_open(e:behind:c)
        set b [dict get $::gp_dumped e:behind:c meta behind]
        set proof [expr {[dict get $b spam] + [dict get $b ham] + [dict get $b odd]}]
        assert_equal $proof [gp_payload_get $o cand.samples] "proof of the second snapshot of e:behind:c"
        assert_equal 0 [gp_payload_get $o cand.missed]
        # It is dropped after 10 times the proof a flawless threshold of
        # snapshot 2 needs, for each of two labels.
        assert_equal [expr {10 * [gp_need 2 2] * 2}] [gp_payload_get $o cand.budget] "the budget of the second snapshot of e:behind:c"
        # The first snapshot answers ham at the lowest of the 32 thresholds
        # and spam at a higher one. It took the spam and the odd texts for
        # spam, and is sure enough only of the spam; it took the ham for ham.
        lassign [dict get $::gp_dumped e:behind:c info thresholds] spam ham
        assert_equal 0 [gp_grid_index 2 $ham]
        set at [gp_grid_index 2 $spam]
        assert {$at > 0}
        assert_equal [expr {[dict get $b spam] + [dict get $b odd]}] [gp_payload_get $o cand.paired.0.0] "proof requests the first snapshot took for spam"
        assert_equal [dict get $b spam] [gp_payload_get $o cand.paired.0.$at] "proof requests the first snapshot answers spam for"
        assert_equal [dict get $b ham] [gp_payload_get $o cand.paired.1.0] "proof requests the first snapshot answers ham for"
        foreach row {0 1} {
            for {set t 1} {$t < 32} {incr t} {
                assert {[gp_payload_get $o cand.paired.$row.$t] <= [gp_payload_get $o cand.paired.$row.[expr {$t - 1}]]}
            }
            for {set t 0} {$t < 32} {incr t} { assert_equal 0 [gp_payload_get $o serv.paired.$row.$t] }
        }
        # The spot checks of the first snapshot: every spam and ham text
        # since it answers that label; all were answered and agreed.
        foreach {row topic} {0 spam 1 ham} {
            assert {[dict get $b audit.$topic] >= 30}
            set stored {}
            foreach name {asked errors wrong0 wrong1 wrong2 lost0 lost1 lost2} { lappend stored [gp_payload_get $o serv.drift.$row.$name] }
            assert_equal [list [dict get $b audit.$topic] 0 0 0 0 0 0 0] $stored "spot checks for $topic of e:behind:c"
        }
        # The three odd texts that wait are proof for both snapshots (flags
        # 1, 4 and 8) and no spot checks: the first snapshot took them for
        # spam (label 0) and leaves them to the teacher, the second takes
        # them for ham (label 1).
        assert_equal 3 [gp_payload_get $o count]
        for {set i 0} {$i < 3} {incr i} {
            set stored {}
            foreach name {flags spot cand_gen cand_label serv_gen serv_label result} { lappend stored [gp_payload_get $o req.$i.$name] }
            assert_equal {13 0 2 1 1 0 -1} $stored "request $i of e:behind:c: flags, number as a spot check, and what each snapshot guessed"
        }

        # A snapshot that leaves everything to the teacher keeps count of
        # the requests it could have been tested on.
        set o $lay_open(e:defer:c)
        assert_equal 10 [gp_payload_get $o cand.missed] "requests the snapshot of e:defer:c left to the teacher for nothing"
        assert_equal 0 [gp_payload_get $o cand.samples]
        assert_equal [gp_deferring_budget] [gp_payload_get $o cand.budget] "the budget of the snapshot of e:defer:c"
        assert_equal 2 [gp_payload_get $o count]
        for {set i 0} {$i < 2} {incr i} {
            # Proof for nothing (flag 1: the draw sent it to the teacher),
            # kept with its clues for the model to learn from: the snapshot
            # guessed the label it must not answer with (label 2).
            set stored {}
            foreach name {flags spot cand_gen cand_label serv_gen serv_label result} { lappend stored [gp_payload_get $o req.$i.$name] }
            assert_equal {1 0 1 2 0 -1 -1} $stored "request $i of e:defer:c"
            assert {[gp_payload_get $o req.$i.clues] > 0}
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
        gp_defaults
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
            "the snapshot step, at most a thousand million" [binary format i 439041101] [binary format i 1000000000] {} \
                [list "one more" [binary format i 1000000001] "the largest number" [binary format i 4294967295]] \
            "the feedback timeout" [binary format w 123456789] [binary format w 50] {} \
                [list zero [binary format w 0] negative [binary format w -1]] \
            "the feedback timeout, at most a week" [binary format w 123456789] [binary format w 604800000] {} \
                [list "one millisecond more" [binary format w 604800001] "the largest number" [binary format w 9223372036854775807]] \
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
        assert {$refused >= 29}
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
        set base [gp_dump_base $head $blob]
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
            set result [gp_restore_damaged [gp_dump_damage $base $damaged $version $at] "x:c: byte $at from $old to $new"]
            dict incr count $result
        }
        assert_equal 200 [expr {[dict get $count refused] + [dict get $count accepted]}]
        assert_equal PONG [r ping]
        if {$::verbose} { puts "damaged classifier payloads: $count" }
    }

    gp_test {RESTORE of a damaged payload: random damage to gates with late and lost answers, gates that stopped and gates that resumed, gates with spot checks and with a takeover undecided is refused or leaves a working gate} {
        gp_begin 518
        expr {srand(518)}
        r flushall
        r config set rdbcompression no
        # The 23 payloads the DUMP / RESTORE test above has kept.
        assert_equal 23 [dict size $::gp_dumped]
        set count {refused 0 accepted 0}
        set ::gp_kept {}
        dict for {key dumped} $::gp_dumped {
            lassign [gp_dump_split [dict get $dumped payload]] head blob version
            set base [gp_dump_base $head $blob]
            set length [string length $blob]
            set kind [dict get $dumped info kind]
            set kept 0
            for {set i 0} {$i < ($kind eq "classifier" ? 16 : 30)} {incr i} {
                # One bit, one byte, or a few bytes.
                set damaged $blob
                set what "$key:"
                set first $length
                set changes [lindex {1 1 1 2 4} [expr {int(rand() * 5)}]]
                for {set j 0} {$j < $changes} {incr j} {
                    # A classifier is mostly weights, where a changed byte
                    # is only another weight: two times out of three the
                    # damage goes to its first bytes or to its end, where
                    # the proof, the requests and the counters are.
                    if {$kind eq "classifier" && $i % 3 == 0} {
                        set at [expr {int(rand() * 256)}]
                    } elseif {$kind eq "classifier" && $i % 3 == 1} {
                        set at [expr {$length - 1 - int(rand() * 4500)}]
                    } else {
                        set at [expr {int(rand() * $length)}]
                    }
                    set old [scan [string index $damaged $at] %c]
                    if {$i % 2} {
                        set new [expr {$old ^ (1 << int(rand() * 8))}]
                    } else {
                        set new [expr {($old + 1 + int(rand() * 255)) & 0xff}]
                    }
                    set damaged [string replace $damaged $at $at [binary format c $new]]
                    append what " byte $at from $old to $new"
                    set first [expr {min($first, $at)}]
                }
                set payload [gp_dump_damage $base $damaged $version $first]
                set result [gp_restore_damaged $payload $what]
                dict incr count $result
                if {$result eq "accepted" && $kept < 3} {
                    # Three of each are kept for the next test, with three
                    # requests each, the second of them answered.
                    incr kept
                    set name x:kept[llength $::gp_kept]
                    assert_equal OK [r restore $name 0 $payload]
                    set asked {}
                    for {set n 1} {$n <= 3} {incr n} {
                        lassign [gp_damaged_request [gp_info $name] $n] question answer
                        set reply [r gpredict $name {*}$question]
                        gp_check_reply $what $reply
                        lappend asked [list [lindex $reply 1] $answer]
                    }
                    lassign [lindex $asked 1] id answer
                    if {$id ne {}} { r glearn $name $id {*}$answer }
                    lappend ::gp_kept [list $name $kind $what $asked]
                }
            }
        }
        assert_equal 578 [expr {[dict get $count refused] + [dict get $count accepted]}]
        # A good share of the damage is noticed, and a good share is not.
        assert {[dict get $count refused] >= 100}
        assert {[llength $::gp_kept] >= 30}
        assert_equal PONG [r ping]
        if {$::verbose} { puts "damaged payloads of gates with late and lost answers: $count" }
    }

    gp_test {RESTORE of a damaged payload: the gates that got in are still gates when time has passed: answers come late or are lost, the gates are reset, saved and deleted} {
        # Follows from the test before, which left gates with unanswered
        # requests. Their feedback timeouts are those of the gates they
        # were made from (unless that is what was damaged): for some the
        # answers are lost by now, for some proof has to move on without
        # one, for some nothing is due.
        after $::gp_wait
        set answers {0 0 1 0}
        foreach kept $::gp_kept {
            lassign $kept name kind what asked
            foreach i {0 2} {
                lassign [lindex $asked $i] id answer
                if {$id eq {}} continue
                set used [r glearn $name $id {*}$answer]
                if {$used ni {0 1}} { fail "$what: GLEARN replied '$used'" }
                dict incr answers $used
                if {[r glearn $name $id {*}$answer] != 0} { fail "$what: late feedback was used twice" }
            }
            gp_check_reply "$what, later" [r gpredict $name {*}[lindex [gp_request $kind 1] 0]]
            gp_check_info "$what, later" [gp_info $name]
            if {[catch {r restore x:again 0 [r dump $name] replace} reply]} {
                fail "$what: when time has passed the gate cannot be restored from its own DUMP: $reply"
            }
            assert_equal OK [r greset $name]
            set info [gp_info $name]
            gp_check_info "$what, later, after GRESET" $info
            foreach field {learned generation pending requests missing revocations} {
                if {[dict get $info $field] != 0} { fail "$what: after GRESET $field is [dict get $info $field]" }
            }
            assert_equal 2 [r del $name x:again]
        }
        # Both happened: answers that were lost, and answers that were late
        # and still used.
        assert {[dict get $answers 0] > 0}
        assert {[dict get $answers 1] > 0}
        assert_equal PONG [r ping]
        if {$::verbose} { puts "late answers for [llength $::gp_kept] damaged gates, by reply: $answers" }
    }

    gp_test {RESTORE of a damaged payload: every single bit of a gate that has stopped, of the requests of gates with late answers, and of the spot checks of a gate that answers, is refused or leaves a working gate} {
        gp_begin 520
        r flushall
        r config set rdbcompression no
        # A gate with an unanswered request and an answer that waits for it.
        # How many bytes a waiting request of a score gate takes is asked
        # of the module.
        gp_score x:two
        set size [string length [r dump x:two]]
        set ids [gp_right x:two 1]
        set record [expr {[string length [r dump x:two]] - $size}]
        assert {$record >= 40 && $record <= 200}
        lappend ids {*}[gp_right x:two 1]
        assert_equal 1 [r glearn x:two [lindex $ids 1] CORRECT 1]
        assert_equal [expr {$size + 2 * $record}] [string length [r dump x:two]]
        # The gate that has stopped is small (it has no snapshot): all of
        # it, with its one request. Of the others their end: the requests
        # that wait, and the counters that follow them; of the gate with
        # spot checks also what comes before its four requests, the count
        # of its spot checks and of their blocks (40 bytes) and the number
        # of the next request.
        set targets [list \
            "a gate that has stopped" $::gp_tiny 100000 \
            "a gate proof has moved on without a request of" [dict get $::gp_dumped e:prov:s payload] [expr {$record + 160}] \
            "a gate with an answer that waits" [r dump x:two] [expr {2 * $record + 160}] \
            "a gate with spot checks" [dict get $::gp_dumped e:spot:a payload] [expr {4 * $record + 172}]]
        # (The 172 bytes are where the layout puts them.)
        set o [gp_payload_open [dict get $::gp_dumped e:spot:a payload]]
        assert_equal [expr {[string length [dict get $o blob]] - 4 * $record - 172}] [lindex [dict get $o fields serv.drift.0.asked] 0]
        # Each damaged payload is restored and, if it gets in, shown, asked,
        # dumped, deleted and restored from that dump, in one script: the
        # payloads are thousands.
        set script [r script load {
            local restored = redis.pcall('RESTORE', KEYS[1], 0, ARGV[1])
            if type(restored) == 'table' and restored.err then return {0, restored.err} end
            local info = redis.call('GINFO', KEYS[1])
            local asked = redis.call('GPREDICT', KEYS[1], 'SCORE', '0.9')
            local dump = redis.call('DUMP', KEYS[1])
            local deleted = redis.call('DEL', KEYS[1])
            local again = redis.pcall('RESTORE', KEYS[1], 0, dump)
            if type(again) == 'table' and again.err then return {1, info, asked, deleted, again.err} end
            redis.call('DEL', KEYS[1])
            return {1, info, asked, deleted, ''}
        }]
        gp_crc64 ""
        set count {refused 0 accepted 0}
        foreach {name payload bytes} $targets {
            lassign [gp_dump_split $payload] head blob version
            set length [string length $blob]
            set from [expr {max(0, $length - $bytes)}]
            set prefix $head
            append prefix [gp_rdb_length $length]
            binary scan [string range $payload end-7 end] w crc
            # What a flipped bit of the last byte of the gate does to the
            # checksum: three bytes follow it.
            set deltas {}
            for {set bit 0} {$bit < 8} {incr bit} {
                set delta [lindex $::gp_crc64_table [expr {1 << $bit}]]
                for {set k 0} {$k < 3} {incr k} { set delta [gp_crc64_zero $delta] }
                lappend deltas $delta
            }
            for {set at [expr {$length - 1}]} {$at >= $from} {incr at -1} {
                set old [scan [string index $blob $at] %c]
                set earlier {}
                for {set bit 0} {$bit < 8} {incr bit} {
                    set delta [lindex $deltas $bit]
                    lappend earlier [gp_crc64_zero $delta]
                    set what "$name: bit $bit of byte $at"
                    set damaged $prefix
                    append damaged [string replace $blob $at $at [binary format c [expr {$old ^ (1 << $bit)}]]] \
                        "\x00" $version [binary format w [expr {$crc ^ $delta}]]
                    lassign [r evalsha $script 1 gp:bit $damaged] restored info asked deleted problem
                    if {!$restored} {
                        if {![string match "*Bad data format*" $info]} { fail "$what: RESTORE replied '$info'" }
                        dict incr count refused
                        continue
                    }
                    dict incr count accepted
                    gp_check_info $what $info
                    if {[dict get $info generation] != 0 || [dict get $info pending] != 0} {
                        fail "$what: restored with proof: $info"
                    }
                    gp_check_reply $what $asked
                    if {[lindex $asked 0] ne "ABSTAIN"} { fail "$what: replied [lindex $asked 0] right after RESTORE" }
                    if {$deleted != 1} { fail "$what: DEL replied $deleted" }
                    if {$problem ne ""} {
                        fail "$what: after RESTORE and one request the gate cannot be restored from its own DUMP: $problem"
                    }
                }
                set deltas $earlier
            }
        }
        assert_equal 0 [r exists gp:bit]
        # Thousands of payloads, a good share of each kind.
        assert {[dict get $count refused] > 2500}
        assert {[dict get $count accepted] > 2500}
        assert_equal PONG [r ping]
        if {$::verbose} { puts "single bits: $count" }
    }

    gp_test {RESTORE of a damaged payload: whether a gate has stopped, its run of missing answers and the mark of its last answer are checked (CHANGES.md sections 5 and 9)} {
        gp_begin 521
        r flushall
        r config set rdbcompression no
        # The ledger of a gate: after the random number its ids start with
        # come two clocks, the number of snapshots taken, whether the gate
        # is paused, the number of examples at the last snapshot, whether
        # it has stopped (one byte), how many answers were lost in a row
        # (four bytes) and, new in this form, the number of the next
        # request when the last answer arrived. In the payloads the DUMP /
        # RESTORE test has kept, the test of the layout has found there
        # what the gates were.
        foreach name {lost:s mark:s blind:s blind:p res:s wait:c blind:c} {
            set ledger($name) [gp_payload_open [dict get $::gp_dumped e:$name payload]]
        }
        set next [gp_payload_get $ledger(mark:s) next]
        assert {[gp_payload_get $ledger(mark:s) answer_mark] > 0 && [gp_payload_get $ledger(mark:s) answer_mark] < $next}
        assert_equal 0 [gp_payload_get $ledger(lost:s) answer_mark]
        # A gate stops at the 32nd lost answer, and a gate that has stopped
        # holds no snapshot. One that neither tests a snapshot nor has
        # stopped is a gate that will take one at its first write, as every
        # gate that comes back does. The mark is a number the next request
        # once had: at most the one it has now.
        foreach {name field value expected} [list \
            lost:s  missing_run 31          accepted \
            lost:s  missing_run 32          refused \
            lost:s  missing_run 4294967295  refused \
            lost:s  blind       1           refused \
            lost:s  blind       2           refused \
            blind:s missing_run 31          accepted \
            blind:s missing_run 32          refused \
            blind:s blind       0           accepted \
            blind:s blind       2           refused \
            blind:s blind       255         refused \
            blind:c blind       0           accepted \
            blind:c missing_run 32          refused \
            blind:p blind       2           refused \
            res:s   blind       1           refused \
            wait:c  blind       1           refused \
            wait:c  missing_run 33          refused \
            mark:s  answer_mark 0           accepted \
            mark:s  answer_mark 1           accepted \
            mark:s  answer_mark $next       accepted \
            mark:s  answer_mark [expr {$next + 1}] refused \
            mark:s  answer_mark [expr {$::gp_count_max + 1}] refused \
            mark:s  answer_mark 18446744073709551615 refused \
            lost:s  answer_mark [gp_payload_get $ledger(lost:s) next] accepted \
            lost:s  answer_mark [expr {[gp_payload_get $ledger(lost:s) next] + 1}] refused \
        ] {
            set payload [gp_payload_with $ledger($name) $field $value]
            assert_equal $expected [gp_restore_damaged $payload "e:$name with $field $value"] "e:$name with $field $value"
        }
        # A payload in the form of the round before, which did not have
        # the mark, or of the first core, which had none of the three
        # fields, is refused, not taken for something else.
        foreach name [array names ledger] {
            set o $ledger($name)
            set blob [dict get $o blob]
            set stopped [lindex [dict get $o fields blind] 0]
            set mark [lindex [dict get $o fields answer_mark] 0]
            assert_equal [expr {$stopped + 5}] $mark
            foreach {form first last} [list "the round before" $mark [expr {$mark + 7}] "the first core" $stopped [expr {$mark + 7}]] {
                set payload [gp_dump_join [dict get $o head] [string replace $blob $first $last] [dict get $o version]]
                assert_equal refused [gp_restore_damaged $payload "e:$name in the form of $form"] "e:$name in the form of $form"
            }
        }
    }

    gp_test {RESTORE of a damaged payload: the counts of a snapshot are held against each other: its spot checks and their blocks, the numbers of the spot checks that wait, the count a takeover is decided on (CHANGES.md section 9)} {
        gp_begin 525
        r flushall
        r config set rdbcompression no
        # A gate that answers: ten spot checks, two disagreed, four were
        # lost; the spot checks number 7 to 10 wait (requests 0 to 3), the
        # last of them unanswered.
        set o [gp_payload_open [dict get $::gp_dumped e:spot:a payload]]
        set d serv.drift.0
        foreach {values expected} [list \
            "$d.wrong0 6" accepted \
            "$d.wrong0 7" refused \
            "$d.lost1 8" accepted \
            "$d.lost1 9" refused \
            "$d.wrong2 10 $d.lost2 0" accepted \
            "$d.wrong2 10 $d.lost2 1" refused \
            "$d.wrong0 4294967295" refused \
            "$d.lost2 4294967295" refused \
            "$d.wrong1 2147483648 $d.lost1 2147483648" refused \
            "$d.errors 10" accepted \
            "$d.errors 11" refused \
            "$d.asked 64" accepted \
            "$d.asked 65" refused \
            "$d.asked 65 $d.wrong0 1 $d.lost0 0" accepted \
            "$d.asked 65 $d.wrong0 1 $d.lost0 1" refused \
            "$d.asked 512 $d.wrong0 0 $d.lost0 0" accepted \
            "$d.asked 513 $d.wrong0 0 $d.lost0 0" refused \
            "$d.asked 513 $d.wrong0 0 $d.lost0 0 $d.wrong1 0 $d.lost1 1" accepted \
            "$d.asked 4097 $d.wrong0 0 $d.lost0 0 $d.wrong1 0 $d.lost1 0" refused \
            "$d.asked 4097 $d.wrong0 0 $d.lost0 0 $d.wrong1 0 $d.lost1 0 $d.wrong2 1 $d.lost2 0" accepted \
            "$d.asked 9" refused \
            "$d.asked 10 req.3.spot 10" accepted \
            "req.3.spot 11" refused \
            "req.0.spot 11" refused \
            "req.3.spot 1" accepted \
            "req.3.spot 0" accepted \
            "req.3.flags 9" refused \
            "req.3.flags 10" refused \
            "req.3.flags 3" accepted \
            "req.3.flags 8 req.3.spot 0" accepted \
            "req.3.serv_gen 0" refused \
            "req.1.flags 19 req.1.spot 8" refused \
        ] {
            # Why each: a block holds no more disagreements and lost
            # answers than spot checks were asked in it so far, and a block
            # of 64 (512, 4,096) ends with its 64th: the next spot check is
            # the first of a new one; there are no more disagreements than
            # spot checks; the number of a spot check that waits was handed
            # out by its detector; and only a request the gate would have
            # answered and the draw sent to the teacher has such a number
            # (flags 1 and 2), asked of a snapshot that answered. (An
            # answered request is proof for something: flag 16 needs 4 or 8.)
            set payload [gp_payload_with $o {*}$values]
            assert_equal $expected [gp_restore_damaged $payload "e:spot:a with $values"] "e:spot:a with $values"
        }
        # No request of a gate that does not answer yet is a spot check.
        set o [gp_payload_open [dict get $::gp_dumped e:wait:s payload]]
        assert_equal 0 [gp_payload_get $o req.0.spot]
        assert_equal refused [gp_restore_damaged [gp_payload_with $o req.0.spot 1] "e:wait:s with a spot check"]
        # The count a takeover is decided on: of the proof of the snapshot
        # that is tested, so never more than its proof, and never more for
        # a higher threshold than for a lower one.
        set o [gp_payload_open [dict get $::gp_dumped e:behind:c payload]]
        set proof [gp_payload_get $o cand.samples]
        set low [gp_payload_get $o cand.paired.1.0]
        assert {$low > 0 && $low < $proof}
        foreach {values expected} [list \
            "cand.paired.0.0 $proof" accepted \
            "cand.paired.0.0 [expr {$proof + 1}]" refused \
            "cand.paired.1.1 [expr {$low + 1}]" refused \
            "cand.paired.0.0 $proof cand.paired.0.1 $proof" accepted \
            "cand.paired.1.31 $low cand.paired.1.30 [expr {$low - 1}]" refused \
        ] {
            set payload [gp_payload_with $o {*}$values]
            assert_equal $expected [gp_restore_damaged $payload "e:behind:c with $values"] "e:behind:c with $values"
        }
        # The number of a spot check is held against the detector of the
        # snapshot the request names also when that snapshot is the one
        # being tested, number 2 here: it may come to answer (gate_codec.c).
        # Its detectors have handed out no number yet.
        assert_equal {0 0} [list [gp_payload_get $o cand.drift.0.asked] [gp_payload_get $o cand.drift.1.asked]]
        foreach {values expected} {
            "req.0.serv_gen 2 req.0.flags 15 req.0.spot 1" refused
            "req.0.serv_gen 2 req.0.flags 15 req.0.spot 0" accepted
        } {
            set payload [gp_payload_with $o {*}$values]
            assert_equal $expected [gp_restore_damaged $payload "e:behind:c with $values"] "e:behind:c with $values"
        }
        # A request is not proof for the snapshot that is tested (flag 4)
        # through a guess of a label the gate never answers with: the count
        # a takeover is decided on would run ahead of the proof
        # (gate_codec.c). The two requests of this gate are such guesses
        # (label 2), and proof for nothing.
        set o [gp_payload_open [dict get $::gp_dumped e:defer:c payload]]
        assert_equal {1 2 1} [list [gp_payload_get $o req.0.flags] [gp_payload_get $o req.0.cand_label] [gp_payload_get $o defer.2]]
        foreach {values expected} {
            "req.0.flags 5" refused
            "req.0.flags 5 req.0.cand_label 0" accepted
        } {
            set payload [gp_payload_with $o {*}$values]
            assert_equal $expected [gp_restore_damaged $payload "e:defer:c with $values"] "e:defer:c with $values"
        }
    }

    gp_test {RESTORE of a damaged payload: no count above 2^62 gets in, whichever count it is} {
        gp_begin 526
        r flushall
        r config set rdbcompression no
        # A score gate that answers and holds four spot checks: every count
        # a gate without a model has. (2^62 itself gets in everywhere: the
        # gates of gp_limits.)
        set lim [gp_limits x:over]
        assert_equal 5 [r del {*}[dict keys [dict get $lim gates]]]
        set o [dict get $lim opened]
        set counters [gp_blob_counters [dict get $o fields]]
        # Its ledger (3), the snapshot (7, three for each of 32 thresholds,
        # the takeover count of each, two of its spot checks), the next
        # request number, the number of each of four spot checks, and 14
        # counters.
        assert_equal [expr {3 + 7 + 3 * 32 + 32 + 2 + 1 + 4 + 14}] [llength $counters]
        set said [count_log_message 0 "cannot load: counter out of range"]
        set refused 0
        foreach name $counters {
            foreach value [list [expr {$::gp_count_max + 1}] 9223372036854775808 18446744073709551615] {
                if {![catch {r restore x:over 0 [gp_payload_with $o $name $value]} reply]} {
                    fail "a gate with $name = $value was restored"
                }
                assert_match "*Bad data format*" $reply
                incr refused
            }
        }
        # What a classifier has more: how many examples each of its models
        # has learned from, and the counts of a snapshot that is tested
        # behind another.
        set o [gp_payload_open [dict get $::gp_dumped e:behind:c payload]]
        foreach name {trainer.updates cand.updates serv.updates cand.gen cand.missed cand.budget cand.paired.0.0
                      cand.paired.1.31 serv.drift.1.asked serv.drift.1.errors serv.region.1.31.n} {
            if {![catch {r restore x:over 0 [gp_payload_with $o $name [expr {$::gp_count_max + 1}]]} reply]} {
                fail "a gate with $name = 2^62 + 1 was restored"
            }
            assert_match "*Bad data format*" $reply
            incr refused
        }
        assert_equal 0 [r exists x:over]
        # Each was refused for that count, whatever else is wrong with it.
        assert_equal [expr {$said + $refused}] [count_log_message 0 "cannot load: counter out of range"] "payloads refused for a count out of range"
    }

    gp_test {RESTORE of a damaged payload: a gate that got in can still be saved and loaded when an AOF or a primary goes on from it: all its spot checks disagreed, says the payload, and one of them still waits for its answer} {
        # A client's first write makes a restored gate start over; an
        # operation of an AOF or of a primary does not (a test connection
        # may send one: DEBUG MARK-INTERNAL-CLIENT). gate_codec.c: "a gate
        # that decodes must still decode after the core has worked on it".
        gp_begin 531
        r config set rdbcompression no
        set dumped [dict get $::gp_dumped e:spot:a]
        set o [gp_payload_open [dict get $dumped payload]]
        # Ten spot checks, two of which disagreed; the seventh and the
        # tenth wait for their answers. A payload that says all ten
        # disagreed gets in (eleven would not).
        assert_equal {10 2} [list [gp_payload_get $o serv.drift.0.asked] [gp_payload_get $o serv.drift.0.errors]]
        assert_equal OK [r restore x:rep 0 [gp_payload_with $o serv.drift.0.errors 10]]
        # The answer of the seventh arrives, and disagrees.
        set seventh [lindex [dict get $dumped meta spots] 0 0]
        assert_equal OK [r debug mark-internal-client]
        catch {r _gate.apply x:rep LEARN [gp_payload_get $o clock] $seventh 0} applied
        r debug mark-internal-client unmark
        assert_equal OK $applied
        assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
        set problem [gp_unloadable x:rep]
        r del x:rep
        if {$problem ne {}} {
            fail "a gate restored with as many disagreements as spot checks, after the answer of a spot check that waited was applied to it, cannot be restored from its own DUMP: $problem"
        }
    }

    gp_test {RESTORE of a damaged payload: every single bit of the spot checks of a gate that answers is refused or leaves a gate that can be saved and loaded when an AOF or a primary goes on from it} {
        # As in the test before, for every payload that differs in one bit
        # from that of a gate with spot checks, in its count of them and of
        # their blocks, its requests or its counters. What an AOF or a
        # primary goes on with: an answer that disagrees for each of the
        # four spot checks that wait (two of them have their answer, so
        # this one is not used), and then enough time for whatever still
        # waits to be lost.
        gp_begin 535
        r config set rdbcompression no
        set dumped [dict get $::gp_dumped e:spot:a]
        set o [gp_payload_open [dict get $dumped payload]]
        set clock [gp_payload_get $o clock]
        set ops {}
        foreach spot [dict get $dumped meta spots] {
            lappend ops [list LEARN $clock [lindex $spot 0] 0]
        }
        lappend ops [list TICK [expr {$clock + [gp_payload_get $o timeout] + 1}]]
        set restore [r script load {
            local restored = redis.pcall('RESTORE', KEYS[1], 0, ARGV[1])
            if type(restored) == 'table' and restored.err then return 0 end
            return 1
        }]
        set check [r script load {
            local dump = redis.call('DUMP', KEYS[1])
            redis.call('DEL', KEYS[1])
            local again = redis.pcall('RESTORE', KEYS[1], 0, dump)
            if type(again) == 'table' and again.err then return again.err end
            redis.call('DEL', KEYS[1])
            return ''
        }]
        set bytes [expr {[string length [dict get $o blob]] - [lindex [dict get $o fields serv.drift.0.asked] 0]}]
        set count {refused 0 accepted 0}
        set problems {}
        assert_equal OK [r debug mark-internal-client]
        gp_each_bit [dict get $dumped payload] $bytes what damaged {
            if {[r evalsha $restore 1 gp:bit $damaged]} {
                dict incr count accepted
                foreach op $ops { r _gate.apply gp:bit {*}$op }
                set problem [r evalsha $check 1 gp:bit]
                if {$problem ne ""} {
                    lappend problems "$what: after the answers of its spot checks and the passing of time the gate cannot be restored from its own DUMP: $problem"
                }
            } else {
                dict incr count refused
            }
        }
        r debug mark-internal-client unmark
        r del gp:bit
        if {[llength $problems]} { fail [join [lrange $problems 0 9] "\n"] }
        assert {[dict get $count refused] > 500 && [dict get $count accepted] > 1000}
        assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
        if {$::verbose} { puts "single bits, with operations applied: $count" }
    }

    gp_test {RESTORE: proof for a snapshot the gate no longer has is given up as such when its time is up, not counted as a lost answer} {
        # CHANGES.md 9e. A payload may hold requests that are proof for a
        # snapshot that is gone (gate_codec.c: the core copes). Seen as an
        # AOF or a primary leaves the gate, as in the test before.
        gp_begin 532
        r config set rdbcompression no
        gp_score x:gone 1 500
        gp_right x:gone 3
        set o [gp_payload_open [r dump x:gone]]
        assert_equal {1 1 5 5 5} [list [gp_payload_get $o generations] [gp_payload_get $o cand.gen] \
            [gp_payload_get $o req.0.flags] [gp_payload_get $o req.1.flags] [gp_payload_get $o req.2.flags]]
        set later [expr {[gp_payload_get $o clock] + 500}]
        # The three requests are proof for snapshot 1 (flag 4; flag 1: the
        # draw, with AUDIT 1). In the first two payloads the snapshot that
        # is tested is number 2; in the second and the last, proof has
        # moved on without the requests (flag 32), so they are counted
        # as wrong guesses already, where there is a snapshot to count
        # them for.
        foreach {values expected} {
            {generations 2 cand.gen 2} {0 3 0}
            {generations 2 cand.gen 2 req.0.flags 37 req.1.flags 37 req.2.flags 37} {0 3 0}
            {} {3 0 3}
            {req.0.flags 37 req.1.flags 37 req.2.flags 37} {3 0 0}
        } {
            assert_equal OK [r restore x:gone 0 [gp_payload_with $o {*}$values] replace]
            assert_equal OK [r debug mark-internal-client]
            catch {r _gate.apply x:gone TICK $later} applied
            r debug mark-internal-client unmark
            assert_equal OK $applied
            set w [gp_payload_open [r dump x:gone]]
            assert_equal [list 0 {*}$expected] \
                [list [gp_payload_get $w count] [gp_payload_get $w stats.missing] [gp_payload_get $w stats.dropped] \
                      [gp_payload_get $w cand.samples]] \
                "with {$values}, when the time of the three requests is up: requests left, missing, dropped, and the proof of the snapshot"
            assert_equal [lindex $expected 0] [gp_info x:gone missing] "missing of x:gone with {$values}"
            assert_equal {} [gp_unloadable x:gone]
        }
        assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
        r del x:gone
    }

    gp_test {RESTORE: when giving up a request stops the gate, the same write gives up everything else that is too old} {
        # CHANGES.md 9e: after a request that proof had moved on without is
        # given up, the queue of proof is put in order (an alarm may have
        # left requests at its head that are proof for nothing any more),
        # so that the requests behind them are not kept for another write.
        # Seen as an AOF or a primary leaves the gate, as in the tests
        # before.
        gp_begin 536
        r config set rdbcompression no
        # The gate with spot checks: four requests wait. In this payload
        # proof has moved on without the first (flag 32), which was asked
        # after the last answer and comes after 31 lost answers in a row;
        # the second, answered, is proof for a snapshot that is gone; the
        # third is answered; the fourth is proof for nothing (flags 1 and 2
        # are left of it).
        set o [gp_payload_open [dict get $::gp_dumped e:spot:a payload]]
        assert_equal {11 27 27 11} [list [gp_payload_get $o req.0.flags] [gp_payload_get $o req.1.flags] \
                                         [gp_payload_get $o req.2.flags] [gp_payload_get $o req.3.flags]]
        set missing [gp_payload_get $o stats.missing]
        set dropped [gp_payload_get $o stats.dropped]
        assert_equal 0 [gp_payload_get $o stats.revocations]
        assert_equal OK [r restore x:queue 0 [gp_payload_with $o req.0.flags 43 missing_run 31 answer_mark 0 \
                                                 req.1.serv_gen 0 req.1.spot 0 req.3.flags 3]]
        # Time passes, more than the feedback timeout. The first request is
        # given up: the 32nd lost answer in a row, so the gate stops. The
        # two answers are proof for nothing now, and the fourth request is
        # too old: nothing is left.
        assert_equal OK [r debug mark-internal-client]
        catch {r _gate.apply x:queue TICK [expr {[gp_payload_get $o clock] + [gp_payload_get $o timeout] + 1}]} applied
        r debug mark-internal-client unmark
        assert_equal OK $applied
        set w [gp_payload_open [r dump x:queue]]
        assert_equal [list 0 [expr {$missing + 1}] [expr {$dropped + 1}] 1 1 0] \
            [list [gp_payload_get $w count] [gp_payload_get $w stats.missing] [gp_payload_get $w stats.dropped] \
                  [gp_payload_get $w stats.revocations] [gp_payload_get $w blind] [gp_payload_get $w serv]] \
            "requests left, missing, dropped, revocations, whether the gate has stopped, whether a snapshot answers"
        assert_equal {} [gp_unloadable x:queue]
        assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
        r del x:queue
    }

    gp_test {RESTORE: a gate that was dumped where the clock is ahead still loses the answers that come too late} {
        gp_begin 522
        r flushall
        r config set rdbcompression no
        gp_score x:clock 0.2 200
        set ids [gp_right x:clock 1]
        lassign [gp_dump_split [r dump x:clock]] head blob version
        # The clock of the gate as of its last write: in its ledger, after
        # the random number its ids start with and the time it was created.
        set epoch [binary format w 0x[lindex [split [lindex $ids 0] -] 0]]
        set x_clock [expr {[string first $epoch $blob] + 16}]
        assert {$x_clock > 16}
        binary scan [string range $blob $x_clock [expr {$x_clock + 7}]] w clock
        set now [clock milliseconds]
        assert {$clock <= $now + 1000 && $clock > $now - 60000}
        # The same gate as another server would have dumped it: one whose
        # clock is right, one whose clock is an hour ahead (or this
        # server's clock was set back since), and a year ahead.
        set asked {}
        foreach {what ahead} {"is right" 0 "is an hour ahead" 3600000 "is a year ahead" 31536000000} {
            set payload [gp_dump_join $head [string replace $blob $x_clock [expr {$x_clock + 7}] [binary format w [expr {$clock + $ahead}]]] $version]
            foreach reset {0 1} {
                set key x:clock:$ahead:$reset
                assert_equal OK [r restore $key 0 $payload] "RESTORE of a gate whose clock $what"
                if {$reset} { assert_equal OK [r greset $key] }
                lassign [r gpredict $key SCORE 0.9] action id
                assert_equal ABSTAIN $action
                lappend asked "a gate from where the clock $what[expr {$reset ? ", after GRESET" : ""}]" $key $id
            }
        }
        # The feedback timeout of these gates is a fifth of a second.
        after 300
        set problems {}
        foreach {what key id} $asked {
            set used [r glearn $key $id CORRECT 1]
            set missing [gp_info $key missing]
            if {$used != 0 || $missing != 1} {
                lappend problems "$what: feedback after 300 ms, with a feedback timeout of 200 ms: GLEARN replied $used, missing is $missing"
            }
            # Its clock went on from where it was: what the gate is now
            # must be something Redis can save and load.
            if {[catch {r restore x:again 0 [r dump $key] replace} reply]} {
                lappend problems "$what: after 300 ms the gate cannot be restored from its own DUMP: $reply"
            }
            assert_equal 1 [r del $key]
        }
        r del x:again
        if {[llength $problems]} { fail [join $problems "\n"] }
    }

    gp_test {RESTORE of a damaged payload: a clock past the end of its range is refused; a gate whose clock is at the end, or gets there, can be used, saved and loaded for good} {
        # Follows from the test before: the payload of x:clock and where it
        # holds its clock. The last moment of a gate's clock is a week
        # before the largest number there is.
        gp_begin 524
        set last $::gp_clock_max
        foreach {what value} [list "one millisecond past the end of its range" [expr {$last + 1}] \
                                   "at the largest number there is" 9223372036854775807 \
                                   "before its range" -1] {
            set payload [gp_dump_join $head [string replace $blob $x_clock [expr {$x_clock + 7}] [binary format w $value]] $version]
            assert_equal refused [gp_restore_damaged $payload "a clock $what"] "a clock $what"
        }
        foreach {what value} [list "at the end of its range" $last \
                                   "10 milliseconds before the end of its range" [expr {$last - 10}] \
                                   "a minute before the end of its range" [expr {$last - 60000}]] {
            set payload [gp_dump_join $head [string replace $blob $x_clock [expr {$x_clock + 7}] [binary format w $value]] $version]
            r del x:late
            assert_equal OK [r restore x:late 0 $payload] "RESTORE of a gate with its clock $what"
            # A request, and its answer 50 milliseconds later, which is in
            # time (the feedback timeout of this gate is a fifth of a
            # second, and a clock at the end of its range stands still).
            lassign [r gpredict x:late SCORE 0.9] action id
            assert_equal ABSTAIN $action
            assert_equal {} [gp_unloadable x:late] "a gate from where the clock is $what, with a request waiting"
            after 50
            assert_equal 1 [r glearn x:late $id CORRECT 1] "a clock $what: feedback in time"
            gp_right x:late 2
            assert_equal {} [gp_unloadable x:late] "a gate from where the clock is $what, after a request, 50 ms and two more requests"
            # The clock it is saved with went on from where it was, and not
            # past the end of its range.
            set saved [gp_payload_get [gp_payload_open [r dump x:late]] clock]
            assert {$saved >= $value && $saved <= $last}
            if {$value + 50 > $last} { assert_equal $last $saved "the clock of a gate from where the clock is $what, 50 ms later" }
            assert_equal OK [r greset x:late]
            gp_right x:late 1
            assert_equal {} [gp_unloadable x:late] "a gate from where the clock is $what, after GRESET"
            set saved [gp_payload_get [gp_payload_open [r dump x:late]] clock]
            assert {$saved >= $value && $saved <= $last}
        }
        r del x:late
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

    gp_test {RESTORE: a gate with every count at 2^62 and its clock at its last moment, gates with the last snapshot number used or still to use, and a gate without a request number left can be used, saved and loaded for good} {
        gp_begin 515
        r flushall
        r config set rdbcompression no
        # The gates of gp_limits, with a feedback timeout shorter than the
        # sleep below, and one more: a gate that has one snapshot number
        # left.
        set ::gp_lim [gp_limits x:lim $::gp_lost]
        gp_limits_intact $::gp_lim
        set o [dict get $::gp_lim opened]
        set max $::gp_count_max
        set values [list generations [expr {$max - 1}] serv.gen [expr {$max - 1}]]
        for {set i 0} {$i < 4} {incr i} { lappend values req.$i.serv_gen [expr {$max - 1}] }
        assert_equal OK [r restore x:lim:snap1 0 [gp_payload_with $o {*}$values]]
        # And a gate that has no request number left (it holds no request).
        gp_score x:plain
        set plain [gp_payload_open [r dump x:plain]]
        assert_equal 1 [r del x:plain]
        assert_equal OK [r restore x:lim:full 0 [gp_payload_with $plain next $max]]
        set problems {}
        foreach {key what} {x:lim:all "every count at 2^62 and the clock at its last moment"
                            x:lim:snap "2^62 snapshots taken"
                            x:lim:snap1 "one snapshot number left"
                            x:lim:full "no request number left"} {
            lappend problems {*}[gp_use_for_good $key $what]
        }
        # The gate without a request number has asked for no feedback, and
        # still has no number.
        assert_equal {ABSTAIN {}} [r gpredict x:lim:full SCORE 0.9]
        set w [gp_payload_open [r dump x:lim:full]]
        assert_equal [list $max 0] [list [gp_payload_get $w next] [gp_payload_get $w count]] "the next request number of x:lim:full, and the requests it holds"
        assert_equal 1 [r del x:lim:full]
        # The gate that had one snapshot number left has used it for the
        # snapshot it tests since it came back, the last there is: 2^62.
        # A reset drops that snapshot, and there is no number for another:
        # the gate has nothing to test, like the one that had none left.
        assert_equal $max [gp_info x:lim:snap1 candidate] "the snapshot x:lim:snap1 tests"
        assert_equal CALIBRATING [gp_info x:lim:snap1 state] "state of x:lim:snap1"
        assert_equal OK [r greset x:lim:snap1]
        gp_check_reply x:lim:snap1 [r gpredict x:lim:snap1 SCORE 0.9]
        foreach key {x:lim:snap x:lim:snap1} {
            set info [gp_info $key]
            assert_equal COLD [dict get $info state] "state of $key"
            assert_equal 0 [dict get $info candidate] "candidate of $key"
            assert_equal 0 [dict get $info generation] "generation of $key"
        }
        # The gate with every count at its limit has gone on counting. It
        # shows what it has counted, and is saved with no count above 2^62.
        set info [gp_info x:lim:all]
        foreach field {missing revocations} {
            assert {[dict get $info $field] >= $max}
        }
        set w [gp_payload_open [r dump x:lim:all]]
        foreach name [gp_blob_counters [dict get $w fields]] {
            if {[gp_payload_get $w $name] > $max} { lappend problems "x:lim:all is saved with $name [gp_payload_get $w $name]" }
        }
        foreach name {promotions revocations observations learned} {
            assert_equal $max [gp_payload_get $w stats.$name] "$name x:lim:all is saved with"
        }
        assert_equal $::gp_clock_max [gp_payload_get $w clock] "the clock x:lim:all is saved with"
        # Later: for the gates whose clocks go on, the requests that were
        # left unanswered are lost.
        after $::gp_wait
        foreach {key what} {x:lim:all "every count at 2^62 and the clock at its last moment"
                            x:lim:snap "2^62 snapshots taken"
                            x:lim:snap1 "one snapshot number left"} {
            gp_check_reply $what [r gpredict $key SCORE 0.9]
            set problem [gp_unloadable $key]
            if {$problem ne {}} { lappend problems "$what: when time has passed the gate cannot be restored from its own DUMP: $problem" }
            assert_equal OK [r greset $key]
            gp_check_reply $what [r gpredict $key SCORE 0.9]
            set problem [gp_unloadable $key]
            if {$problem ne {}} { lappend problems "$what: after GRESET the gate cannot be restored from its own DUMP: $problem" }
            gp_check_info $what [gp_info $key]
            assert_equal 1 [r del $key]
        }
        if {[llength $problems]} { fail [join $problems "\n"] }
    }

    gp_test {RESTORE: a gate with one request number left can be used, saved and loaded for good} {
        # Follows from the test before: the gates of gp_limits.
        gp_begin 527
        set problems [gp_use_for_good x:lim:next "one request number left"]
        # It has handed out the last request number there is, 2^62 - 1,
        # and has none left: it asks for no more feedback.
        assert_equal {ABSTAIN {}} [r gpredict x:lim:next SCORE 0.9]
        set w [gp_payload_open [r dump x:lim:next]]
        assert_equal $::gp_count_max [gp_payload_get $w next] "the next request number x:lim:next is saved with"
        r del x:lim:next
        if {[llength $problems]} { fail [join $problems "\n"] }
    }

    gp_test {RESTORE: a gate that has counted 2^62 requests can be used, saved and loaded for good} {
        # Follows from the test before: the gates of gp_limits. What became
        # of the requests of a gate (answered by itself, sent to the
        # teacher as a spot check, left to the teacher) adds up to its
        # requests: it must still add up when the gate is saved with 2^62
        # for a number that is beyond.
        gp_begin 528
        set problems [gp_use_for_good x:lim:count "2^62 requests asked, nearly all left to the teacher"]
        lappend problems {*}[gp_use_for_good x:lim:own "2^62 requests asked, nearly all answered by the gate"]
        # The second gate answers by itself once it has proven itself
        # again, and counts that.
        assert {[gp_info x:lim:own accepted] > $::gp_count_max - 4}
        foreach key {x:lim:count x:lim:own} {
            set w [gp_payload_open [r dump $key]]
            set saved {}
            foreach name {requests accepted audited abstained} { lappend saved [gp_payload_get $w stats.$name] }
            lassign $saved requests accepted audited abstained
            if {$requests != $::gp_count_max || $requests != $accepted + $audited + $abstained} {
                lappend problems "$key is saved with requests, accepted, audited, abstained = $saved"
            }
        }
        r del x:lim:count x:lim:own
        if {[llength $problems]} { fail [join $problems "\n"] }
    }

    gp_test {RESTORE: GINFO shows a gate that has no snapshot number left as its first write will leave it} {
        # Follows from the tests before: the gates of gp_limits. A gate
        # that came back shows what its first write will make of it; this
        # one will not test a snapshot again, for want of a number.
        gp_begin 529
        assert_equal OK [r restore x:nosnap 0 [dict get $::gp_lim gates x:lim:snap]]
        set shown [gp_info x:nosnap]
        gp_check_reply x:nosnap [r gpredict x:nosnap SCORE 0.9]
        set now [gp_info x:nosnap]
        r del x:nosnap
        foreach field {state generation candidate} {
            assert_equal [dict get $now $field] [dict get $shown $field] "$field of a gate with 2^62 snapshots taken, as GINFO showed it before its first write"
        }
    }

    gp_test {RESTORE: what a snapshot that is tested behind another has counted beyond 2^62, when an AOF or a primary goes on from it, is saved as 2^62} {
        # An operation of an AOF or of a primary does not make a restored
        # gate start over (a test connection may send one: DEBUG
        # MARK-INTERNAL-CLIENT). The counts of a snapshot that is tested:
        # its proof, the count a takeover is decided on, and the requests
        # it left to the teacher for nothing.
        gp_begin 533
        r config set rdbcompression no
        set max $::gp_count_max
        # The classifier with a takeover undecided; three odd texts wait,
        # proof for both snapshots: the first took them for spam, without
        # being sure. Its second snapshot with 2^62 pieces of proof, all of
        # which the first one took for spam, and no budget (or it would be
        # dropped at once). The answer of the first odd text arrives.
        set dumped [dict get $::gp_dumped e:behind:c]
        set o [gp_payload_open [dict get $dumped payload]]
        assert_equal OK [r restore x:far 0 [gp_payload_with $o cand.samples $max cand.paired.0.0 $max cand.budget 0]]
        set id [lindex [dict get $dumped meta behind waiting] 0 0]
        assert_equal OK [r debug mark-internal-client]
        catch {r _gate.apply x:far LEARN [gp_payload_get $o clock] $id 1} applied
        r debug mark-internal-client unmark
        assert_equal OK $applied
        assert_equal {} [gp_unloadable x:far] "the gate with a snapshot that has 2^62 pieces of proof, after one more"
        set w [gp_payload_open [r dump x:far]]
        assert_equal [list 2 2 $max $max] \
            [list [gp_payload_get $w count] [gp_payload_get $w cand.gen] [gp_payload_get $w cand.samples] [gp_payload_get $w cand.paired.0.0]] \
            "requests that wait, the snapshot that is tested, its proof, and how much of it the first snapshot took for spam"
        # The classifier whose snapshot leaves everything to the teacher,
        # with 2^62 such requests and no budget. One more is answered.
        set dumped [dict get $::gp_dumped e:defer:c]
        set o [gp_payload_open [dict get $dumped payload]]
        assert_equal OK [r restore x:far 0 [gp_payload_with $o cand.missed $max cand.budget 0] replace]
        set id [lindex [dict get $dumped meta old] 10 0]
        assert_equal OK [r debug mark-internal-client]
        catch {r _gate.apply x:far LEARN [gp_payload_get $o clock] $id 0} applied
        r debug mark-internal-client unmark
        assert_equal OK $applied
        assert_equal {} [gp_unloadable x:far] "the gate with a snapshot that left 2^62 requests to the teacher, after one more"
        set w [gp_payload_open [r dump x:far]]
        assert_equal [list 1 1 $max] [list [gp_payload_get $w count] [gp_payload_get $w cand.gen] [gp_payload_get $w cand.missed]] \
            "requests that wait, the snapshot that is tested, and the requests it left to the teacher"
        assert_equal 0 [gp_mismatches] "operations that did not fit their gate"
        r del x:far
    }

    gp_test {RESTORE of a damaged payload: random damage to gates at their limits is refused or leaves a working gate} {
        # Follows from the tests before: the payloads of gp_limits. Where
        # every count is at its limit a bit more is too much, and a bit
        # less leaves a count that gets there by counting.
        gp_begin 534
        expr {srand(534)}
        r config set rdbcompression no
        set count {refused 0 accepted 0}
        foreach name {all snap} {
            lassign [gp_dump_split [dict get $::gp_lim gates x:lim:$name]] head blob version
            set length [string length $blob]
            for {set i 0} {$i < 40} {incr i} {
                set damaged $blob
                set what "x:lim:$name:"
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
                dict incr count [gp_restore_damaged [gp_dump_join $head $damaged $version] $what]
            }
        }
        assert_equal 80 [expr {[dict get $count refused] + [dict get $count accepted]}]
        assert {[dict get $count refused] >= 10 && [dict get $count accepted] >= 10}
        assert_equal PONG [r ping]
        if {$::verbose} { puts "damaged payloads of gates at their limits: $count" }
    }

    gp_test {RESTORE of a damaged payload: a model that has learned from 2^62 examples can go on learning, and be saved and loaded} {
        gp_begin 530
        r flushall
        r config set rdbcompression no
        gp_classifier x:topc 1000
        gp_teach x:topc 300
        set o [gp_payload_open [r dump x:topc]]
        assert_equal 300 [gp_payload_get $o trainer.updates]
        foreach {top expected} [list 9223372036854775807 refused [expr {$::gp_count_max + 1}] refused \
                                     $::gp_count_max accepted [expr {$::gp_count_max - 1}] accepted] {
            # gp_restore_damaged teaches the gate, answers its requests and
            # resets it, and restores its DUMP after each.
            assert_equal $expected [gp_restore_damaged [gp_payload_with $o trainer.updates $top] "learned = $top"] "learned = $top"
        }
        # GADD says how many examples the gate has learned from, and what
        # is saved is not above 2^62.
        assert_equal OK [r restore x:most 0 [gp_payload_with $o trainer.updates [expr {$::gp_count_max - 1}]]]
        assert_equal $::gp_count_max [r gadd x:most TEXT "one more" LABEL spam]
        assert {[r gadd x:most TEXT "and one more" LABEL ham] >= $::gp_count_max}
        assert_equal $::gp_count_max [gp_payload_get [gp_payload_open [r dump x:most]] trainer.updates] "learned x:most is saved with"
        assert_equal {} [gp_unloadable x:most]
        r del x:topc x:most
    }

    gp_test {RESTORE of a damaged payload: GINFO and GADD never show a negative count} {
        gp_begin 516
        r flushall
        r config set rdbcompression no
        set problems {}
        # A count of 2^63 or more would show as a negative number. The
        # number of examples a model has learned from:
        gp_classifier x:count 1000
        gp_teach x:count 300
        set o [gp_payload_open [r dump x:count]]
        foreach {top value} {2^63 9223372036854775808 2^64-1 18446744073709551615} {
            r del x:huge
            if {[catch {r restore x:huge 0 [gp_payload_with $o trainer.updates $value]} reply]} {
                assert_match "*Bad data format*" $reply
                continue
            }
            if {[gp_info x:huge learned] < 0} { lappend problems "learned at $top: GINFO shows learned [gp_info x:huge learned]" }
            set learned [r gadd x:huge TEXT "one more" LABEL spam]
            if {$learned < 0} { lappend problems "learned at $top: GADD replied $learned" }
        }
        r del x:count x:huge
        # The counters GINFO shows, in a gate that has counted something of
        # each but for requests it answered by itself: the gate of the
        # DUMP / RESTORE test with spot checks (which lost answers), and
        # for revocations one that stopped.
        set o [gp_payload_open [dict get $::gp_dumped e:spot:a payload]]
        set info [dict get $::gp_dumped e:spot:a info]
        foreach field {requests audited abstained missing} {
            assert {[gp_payload_get $o stats.$field] > 0}
            assert_equal [dict get $info $field] [gp_payload_get $o stats.$field] "$field of e:spot:a in its payload"
        }
        set stopped [gp_payload_open [dict get $::gp_dumped e:blind:a payload]]
        assert {[gp_payload_get $stopped stats.revocations] > 0}
        # Each of them at 2^63 and at 2^64 - 1. The request counters must
        # add up (or the payload is refused for that), so the total moves
        # with its part.
        foreach {from fields} [list $o {requests accepted} $o {requests audited} $o {requests abstained} $o missing \
                                    $stopped revocations] {
            foreach top {2^63 2^64-1} {
                set part [lindex $fields end]
                set was [gp_payload_get $from stats.$part]
                if {$top eq "2^63"} {
                    set moved [expr {$was + 9223372036854775808}]
                } else {
                    set moved 18446744073709551615
                }
                set values [list stats.$part $moved]
                if {[llength $fields] == 2} {
                    lappend values stats.requests [expr {([gp_payload_get $from stats.requests] - $was + $moved) & 0xffffffffffffffff}]
                }
                r del x:huge
                if {[catch {r restore x:huge 0 [gp_payload_with $from {*}$values]} reply]} {
                    assert_match "*Bad data format*" $reply
                    continue
                }
                # It got in: shown as it is, and after it was used.
                foreach when {"" ", after a request"} {
                    set shown [gp_info x:huge]
                    foreach field {learned requests accepted audited abstained audit-samples audit-errors
                                   calibration-samples pending missing revocations generation candidate memory} {
                        if {[dict get $shown $field] < 0} {
                            lappend problems "$part at $top$when: GINFO shows $field [dict get $shown $field]"
                        }
                    }
                    r gpredict x:huge SCORE 0.9
                }
            }
        }
        r del x:huge
        if {[llength $problems]} { fail [join $problems "\n"] }
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
