start_cluster 2 2 {tags {external:skip cluster}} {
    test {Key lazy expires during key migration} {
        R 0 DEBUG SET-ACTIVE-EXPIRE 0

        set key_slot [R 0 CLUSTER KEYSLOT FOO]
        R 0 set FOO BAR PX 10
        set src_id [R 0 CLUSTER MYID]
        set trg_id [R 1 CLUSTER MYID]
        R 0 CLUSTER SETSLOT $key_slot MIGRATING $trg_id
        R 1 CLUSTER SETSLOT $key_slot IMPORTING $src_id
        after 11
        assert_error {ASK*} {R 0 GET FOO}
        R 0 ping
    } {PONG}

    test "Coverage: Basic cluster commands" {
        assert_equal {OK} [R 0 CLUSTER saveconfig]

        set id [R 0 CLUSTER MYID]
        assert_equal {0} [R 0 CLUSTER count-failure-reports $id]

        R 0 flushall
        assert_equal {OK} [R 0 CLUSTER flushslots]
    }

    test "CROSSSLOT error for keys in different slots" {
        # Test MSET with keys in different slots
        assert_error {*CROSSSLOT Keys in request don't hash to the same slot*} {R 0 MSET foo bar baz qux}
        
        # Test DEL with keys in different slots  
        assert_error {*CROSSSLOT Keys in request don't hash to the same slot*} {R 0 DEL foo bar}
        
        # Test MGET with keys in different slots
        assert_error {*CROSSSLOT Keys in request don't hash to the same slot*} {R 0 MGET foo bar}
    } 
}

proc cluster_bus_u16be {n} { binary format S $n }
proc cluster_bus_u32be {n} { binary format I $n }

proc get_cluster_bus_port {id} {
    set addr [dict get [cluster_get_myself $id] addr]
    if {![regexp {@([0-9]+)} $addr -> cport]} {
        fail "Unable to parse cluster bus port from CLUSTER NODES addr '$addr'"
    }
    return $cport
}

# offsetof(clusterMsg, data) == CLUSTERMSG_MIN_LEN: header length with zero gossip entries.
set ::cluster_msg_min_len 2256

proc build_meet_packet {client_port cluster_port count totlen {ext_length ""}} {
    set pkt [string repeat "\0" $totlen]
    set pkt [string replace $pkt 0 3 "RCmb"]
    set pkt [string replace $pkt 4 7 [cluster_bus_u32be $totlen]]
    set pkt [string replace $pkt 8 9 [cluster_bus_u16be 1]] ;# CLUSTER_PROTO_VER
    set pkt [string replace $pkt 10 11 [cluster_bus_u16be $client_port]]
    set pkt [string replace $pkt 12 13 [cluster_bus_u16be 2]] ;# CLUSTERMSG_TYPE_MEET
    set pkt [string replace $pkt 14 15 [cluster_bus_u16be $count]]
    set pkt [string replace $pkt 2214 2215 [cluster_bus_u16be 1]] ;# extensions
    set pkt [string replace $pkt 2248 2249 [cluster_bus_u16be $cluster_port]]
    set pkt [string replace $pkt 2253 2253 [binary format c 4]] ;# CLUSTERMSG_FLAG0_EXT_DATA
    if {$ext_length ne ""} {
        set off $::cluster_msg_min_len ;# first extension header follows the gossip section
        set pkt [string replace $pkt $off [expr {$off + 3}] [cluster_bus_u32be $ext_length]]
    }
    assert_equal $totlen [string length $pkt]
    return $pkt
}

proc assert_bus_packet_rejected {count totlen ext_length logpattern} {
    set cport [get_cluster_bus_port 0]
    set pkt [build_meet_packet [srv 0 port] $cport $count $totlen $ext_length]
    set loglines [count_log_lines 0]

    set fd [socket [srv 0 host] $cport]
    fconfigure $fd -translation binary -encoding binary -buffering none
    puts -nonewline $fd $pkt
    flush $fd
    catch {close $fd}

    wait_for_log_messages 0 $logpattern $loglines 50 20
    assert_equal {PONG} [R 0 PING]
}

start_cluster 1 0 {tags {external:skip cluster tls:skip}} {
    test "Gossip count exceeding packet length is rejected" {
        # Declared gossip section overruns even a doubled receive buffer.
        set count [expr {2 * $::cluster_msg_min_len / 104 + 1}]
        assert_bus_packet_rejected $count $::cluster_msg_min_len "" \
            {"*Received invalid meet packet with gossip count*exceeds total packet length*"}
    }

    test "Extension header exceeding packet length is rejected" {
        # Zero-entry gossip fits exactly, leaving no room for the fixed extension header.
        assert_bus_packet_rejected 0 $::cluster_msg_min_len "" \
            {"*Received invalid meet packet with extension data*exceeds total packet length*"}
    }

    test "Extension length overflowing 16 bits is rejected" {
        # Room for the 8-byte header only, but the extension claims 0x00010008 (65544)
        # bytes: its low 16 bits (8) would pass a truncated check; the full length must not.
        assert_bus_packet_rejected 0 [expr {$::cluster_msg_min_len + 8}] 65544 \
            {"*Received invalid meet packet with extension data*exceeds total packet length*"}
    }
}
