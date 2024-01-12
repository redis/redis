start_cluster 1 1 {tags {external:skip cluster}} {
        set primary_id 0
        set replica1_id 1
        
        set primary [Rn $primary_id]
        set replica [Rn $replica1_id]

    test "Sharded pubsub publish behavior on a replica" {
        $replica CONFIG SET cluster-allow-pubsubshard-publish-replica no
        set channelname ch3
        assert_error "*MOVED*" {$replica spublish $channelname "hello"}
        $replica CONFIG SET cluster-allow-pubsubshard-publish-replica yes
        assert_equal 0 [$replica spublish $channelname "hello"]
    }

    test "Sharded pubsub publish behavior on a primary" {
        # For a primary, the behavior is the same regardless of the config value.
        $primary CONFIG SET cluster-allow-pubsubshard-publish-replica no
        set channelname ch3
        $primary spublish $channelname "hello"
        assert_equal 0 [$replica spublish $channelname "hello"]
        $primary CONFIG SET cluster-allow-pubsubshard-publish-replica yes
        assert_equal 0 [$replica spublish $channelname "hello"]
    }
}
