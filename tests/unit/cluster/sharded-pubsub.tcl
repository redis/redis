#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
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
        catch {[$primary EXEC]} err
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
}
