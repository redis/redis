source tests/support/cluster.tcl

start_cluster 3 0 {tags {external:skip cluster bitmap bitmap-roaring}} {
    test "Roaring bitmap BITOP works with hash-slot-tagged keys" {
        set slot [R 0 cluster keyslot "{bitop}foo"]
        foreach key {"{bitop}bar" "{bitop}dest" "{bitop}not"} {
            assert_equal $slot [R 0 cluster keyslot $key]
        }

        set port [srv 0 port]
        set cluster [redis_cluster 127.0.0.1:$port]
        array set node [$cluster masternode_for_slot $slot]
        set owner $node(link)

        assert_equal 0 [$owner setbit "{bitop}foo" 1 1]
        assert_equal 0 [$owner setbit "{bitop}bar" 2 1]
        assert_equal OK [convert_string_bitmap_to_roaring $owner "{bitop}foo"]
        assert_equal OK [convert_string_bitmap_to_roaring $owner "{bitop}bar"]
        assert_equal bitmap [$owner type "{bitop}foo"]
        assert_equal bitmap-roaring [$owner object encoding "{bitop}foo"]

        assert_equal 1 [$owner bitop or "{bitop}dest" "{bitop}foo" "{bitop}bar"]
        assert_equal bitmap [$owner type "{bitop}dest"]
        assert_equal bitmap-roaring [$owner object encoding "{bitop}dest"]
        assert_equal 1 [$owner getbit "{bitop}dest" 1]
        assert_equal 1 [$owner getbit "{bitop}dest" 2]

        assert_equal 1 [$owner bitop not "{bitop}not" "{bitop}foo"]
        assert_equal bitmap [$owner type "{bitop}not"]
        assert_equal bitmap-roaring [$owner object encoding "{bitop}not"]
        assert_equal 1 [$owner getbit "{bitop}not" 0]
        assert_equal 0 [$owner getbit "{bitop}not" 1]
        assert_equal 1 [$owner getbit "{bitop}foo" 1]

        # The explicit result mode shifts key positions; cluster routing must
        # still identify the destination and every source.
        $owner set "{bitop}mode:s1" [binary format H* f0]
        $owner set "{bitop}mode:s2" [binary format H* 0f]
        assert_equal 1 [$owner bitop or ROARING "{bitop}mode:dest" \
            "{bitop}mode:s1" "{bitop}mode:s2"]
        assert_equal bitmap [$owner type "{bitop}mode:dest"]
        assert_equal [binary format H* ff] \
            [$owner debug bitmap-raw "{bitop}mode:dest"]
        assert_error {*CROSSSLOT Keys in request don't hash to the same slot*} \
            [list $owner bitop or ROARING "{bitop}mode:cross" \
                "{bitop}mode:s1" "{other}mode:s2"]
        assert_equal PONG [$owner ping]

        $cluster close
    }
}

start_cluster 3 0 {tags {external:skip cluster bitmap bitmap-roaring}} {
    test "MIGRATE moves a Roaring bitmap key between nodes" {
        set key "{bitmig}bm"
        set slot [R 0 cluster keyslot $key]

        set port [srv 0 port]
        set cluster [redis_cluster 127.0.0.1:$port]
        array set node [$cluster masternode_for_slot $slot]
        set owner $node(link)
        set owner_id [$owner cluster myid]

        # Pick any other master as the target.
        set target -1
        for {set i 0} {$i < 3} {incr i} {
            if {[R $i cluster myid] ne $owner_id} {
                set target $i
                break
            }
        }
        set target_id [R $target cluster myid]
        set target_port [srv [expr {0 - $target}] port]

        $owner config set bitmap-default-roaring yes
        $owner del $key
        $owner setbit $key 5 1
        $owner setbit $key 100000 1
        $owner config set bitmap-default-roaring no
        assert_equal bitmap [$owner type $key]

        assert_equal OK [R $target cluster setslot $slot importing $owner_id]
        assert_equal OK [$owner cluster setslot $slot migrating $target_id]
        assert_equal OK [$owner migrate 127.0.0.1 $target_port $key 0 5000]
        assert_equal OK [R $target cluster setslot $slot node $target_id]
        assert_equal OK [$owner cluster setslot $slot node $target_id]

        assert_equal bitmap [R $target type $key]
        assert_equal bitmap-roaring [R $target object encoding $key]
        assert_equal 2 [R $target bitcount $key]
        assert_equal 1 [R $target getbit $key 5]
        assert_equal 1 [R $target getbit $key 100000]

        $cluster close
    }
}
