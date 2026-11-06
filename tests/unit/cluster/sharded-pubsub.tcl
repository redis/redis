#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

start_cluster 1 1 {tags {external:skip cluster}} {
    set primary_id 0
    set replica1_id 1

    set primary [Rn $primary_id]
    set replica [Rn $replica1_id]

    test "Sharded pubsub publish behavior within multi/exec" {
        foreach {node} {primary replica} {
            set node [set $node]
            $node MULTI
            $node SPUBLISH ch1 "hello"
            $node EXEC
        }
    }

    test "Sharded pubsub within multi/exec with cross slot operation" {
        $primary MULTI
        $primary SPUBLISH ch1 "hello"
        $primary GET foo
        catch {$primary EXEC} err
        assert_match {CROSSSLOT*} $err
    }

    test "Sharded pubsub publish behavior within multi/exec with read operation on primary" {
        $primary MULTI
        $primary SPUBLISH foo "hello"
        $primary GET foo
        $primary EXEC
    } {0 {}}

    test "Sharded pubsub publish behavior within multi/exec with read operation on replica" {
        $replica MULTI
        $replica SPUBLISH foo "hello"
        catch {[$replica GET foo]} err
        assert_match {MOVED*} $err
        catch {[$replica EXEC]} err
        assert_match {EXECABORT*} $err
    }

    test "Sharded pubsub publish behavior within multi/exec with write operation on primary" {
        $primary MULTI
        $primary SPUBLISH foo "hello"
        $primary SET foo bar
        $primary EXEC
    } {0 OK}

    test "Sharded pubsub publish behavior within multi/exec with write operation on replica" {
        $replica MULTI
        $replica SPUBLISH foo "hello"
        catch {[$replica SET foo bar]} err
        assert_match {MOVED*} $err
        catch {[$replica EXEC]} err
        assert_match {EXECABORT*} $err
    }

    # Regression: shard channel slot must not follow getKeySlot() current_client
    # cache when CLIENT KILL runs inside another client's EXEC (pubsubUnsubscribeChannel).
    test {Shard pubsub: CLIENT KILL subscriber inside MULTI/EXEC (cross-slot)} {
        # SET fixes the transaction client's slot to keyk's slot; the subscriber must
        # use a shard channel in a different slot so a wrong-slot lookup would fail.
        set keyk "{06S}k"
        set channel "{Qi}ch"
        assert {[R 0 cluster keyslot $channel] != [R 0 cluster keyslot $keyk]}

        set rd_sub [redis_deferring_client]
        $rd_sub client id
        set cid [$rd_sub read]
        $rd_sub ssubscribe $channel
        $rd_sub read

        $primary multi
        $primary set $keyk v
        $primary client kill id $cid
        set got [$primary exec]

        assert_equal {OK 1} $got
        assert_equal PONG [$primary ping]

        catch {$rd_sub read}
        $rd_sub close
    }
}
