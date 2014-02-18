test "Sentinels aren't monitoring any master" {
    foreach_sentinel_id id {
        assert {[S $id sentinel masters] eq {}}
    }
}

test "Sentinels can start monitoring a master" {
    create_redis_master_slave_cluster 3
}
