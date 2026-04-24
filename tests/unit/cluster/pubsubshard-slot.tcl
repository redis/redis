# Regression for shard pubsub slot lookup during command execution.
# Without hashing the channel name while CLIENT_EXECUTING_COMMAND is set,
# pubsubUnsubscribeChannel() could use current_client->slot (from a prior
# command in the same EXEC) and crash. Fixed in commit 2fb406e18.

start_cluster 1 0 {tags {external:skip cluster}} {

    test {Shard pubsub: CLIENT KILL subscriber inside MULTI/EXEC (cross-slot)} {
        # SET fixes the transaction client's slot to keyk's slot; the subscriber
        # must be on a shard channel in a different slot so a wrong-slot lookup fails.
        set keyk k
        set channel ch0
        for {set i 0} {$i < 200} {incr i} {
            set channel "ch$i"
            if {[R 0 cluster keyslot $channel] != [R 0 cluster keyslot $keyk]} {
                break
            }
        }
        assert {[R 0 cluster keyslot $channel] != [R 0 cluster keyslot $keyk]}

        set rd_sub [redis_deferring_client]
        $rd_sub client id
        set cid [$rd_sub read]
        $rd_sub ssubscribe $channel
        $rd_sub read

        r multi
        r set $keyk v
        r client kill id $cid
        set got [r exec]

        assert_equal {OK 1} $got
        assert_equal PONG [R 0 ping]

        catch {$rd_sub read}
        $rd_sub close
    }
}
