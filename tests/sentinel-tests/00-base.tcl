test "Sentinels aren't monitoring any master" {
    foreach_sentinel_id id {
        assert {[S $id sentinel masters] eq {}}
    }
}
