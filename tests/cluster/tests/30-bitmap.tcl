# Cluster tests for bitmap commands.

source "../tests/includes/init-tests.tcl"

test "Create a 3 nodes cluster" {
    create_cluster 3 0
}

test "Native bitmap BITOP works with hash-slot-tagged keys" {
    set slot [R 0 cluster keyslot "{bitop}foo"]
    foreach key {"{bitop}bar" "{bitop}dest" "{bitop}not"} {
        assert_equal $slot [R 0 cluster keyslot $key]
    }

    set port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$port]
    array set node [$cluster masternode_for_slot $slot]
    set owner $node(link)

    assert_equal 0 [$owner setbit "{bitop}foo" 1 1]
    assert_equal 0 [$owner setbit "{bitop}bar" 2 1]
    assert_equal OK [$owner bitmap convert "{bitop}foo"]
    assert_equal OK [$owner bitmap convert "{bitop}bar"]
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
    assert_equal PONG [$owner ping]

    $cluster close
}
