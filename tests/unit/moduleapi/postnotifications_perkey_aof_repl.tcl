set testmodule [file normalize tests/modules/postnotifications_perkey_metadata.so]

# AOF replay on a standalone master.
#
# Asserts that per-key post-notification jobs fire during AOF replay with the
# same pattern as during normal execution: once per single command, once per
# MULTI/EXEC sub-command. The per-key callback attaches module key metadata,
# which is NOT in the AOF — so its presence after reload is direct evidence
# that the callback re-ran during replay. The module-internal fire counter
# (read via `pkmeta.firecount`) is the load-bearing assertion.
tags "modules aof external:skip" {
    foreach aofload_type {debug_cmd startup} {
        test "perkey-aof: single command rebuilds metadata via AOF reload (load=$aofload_type)" {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                r config set appendonly yes
                r config set auto-aof-rewrite-percentage 0
                waitForBgrewriteaof r

                r hset h1 f v
                assert_equal "notified" [r pkmeta.getmeta h1]
                assert_equal 1 [r pkmeta.firecount]

                # Reset the counter so the post-reload count reflects only
                # what the AOF replay path produced.
                r pkmeta.reset

                if {$aofload_type == "debug_cmd"} {
                    r debug loadaof
                } else {
                    r config rewrite
                    restart_server 0 true false
                    wait_done_loading r
                }

                assert_equal "notified" [r pkmeta.getmeta h1]
                assert_equal 1 [r pkmeta.firecount]
            }
        }

        test "perkey-aof: MULTI/EXEC fires once per sub-command during AOF reload (load=$aofload_type)" {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                r config set appendonly yes
                r config set auto-aof-rewrite-percentage 0
                waitForBgrewriteaof r

                r multi
                r hset h1 f v
                r hset h2 f v
                r hset h3 f v
                r exec

                assert_equal 3 [r pkmeta.firecount]
                r pkmeta.reset

                if {$aofload_type == "debug_cmd"} {
                    r debug loadaof
                } else {
                    r config rewrite
                    restart_server 0 true false
                    wait_done_loading r
                }

                # Each sub-command's per-key drain must have fired during
                # replay — three HSETs → three callback invocations.
                assert_equal 3 [r pkmeta.firecount]
                assert_equal "notified" [r pkmeta.getmeta h1]
                assert_equal "notified" [r pkmeta.getmeta h2]
                assert_equal "notified" [r pkmeta.getmeta h3]
            }
        }

        test "perkey-aof: HSET + HEXPIRE in MULTI/EXEC fires twice during AOF reload (load=$aofload_type)" {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                r config set appendonly yes
                r config set auto-aof-rewrite-percentage 0
                waitForBgrewriteaof r

                r multi
                r hset h_hexp f v
                r hexpire h_hexp 100 FIELDS 1 f
                r exec

                assert_equal 2 [r pkmeta.firecount]
                r pkmeta.reset

                if {$aofload_type == "debug_cmd"} {
                    r debug loadaof
                } else {
                    r config rewrite
                    restart_server 0 true false
                    wait_done_loading r
                }

                # HSET + HEXPIRE on the same key — two KSN events, two
                # per-key job firings. This was the original motivating
                # scenario (RED-197766).
                assert_equal 2 [r pkmeta.firecount]
                assert_equal "notified" [r pkmeta.getmeta h_hexp]
            }
        }
    }
}

# RDB load is intentionally outside the firing pattern.
#
# RDB load decodes keys directly without running commands, so no KSN fires,
# so per-key callbacks do not run. This test pins that design boundary: if
# anyone later "fixes" RDB load to fire KSN, this assertion will break and
# force the change to be considered explicitly.
tags "modules external:skip" {
    test "perkey-rdb: RDB-only restart does NOT rebuild metadata (no KSN on RDB load)" {
        start_server [list overrides [list loadmodule "$testmodule" appendonly no]] {
            r hset h_rdb f v
            assert_equal "notified" [r pkmeta.getmeta h_rdb]
            assert_equal 1 [r pkmeta.firecount]

            r pkmeta.reset
            r debug reload

            # RDB roundtrip: the key is back, but its metadata is not (the
            # metadata class doesn't persist via rdb_save/rdb_load in this
            # module), and the per-key job did NOT fire during load.
            assert_equal "hash" [r type h_rdb]
            assert_equal {} [r pkmeta.getmeta h_rdb]
            assert_equal 0 [r pkmeta.firecount]
        }
    } {} {needs:debug}
}

# AOF replay on a replica at startup.
#
# Exercises the carve-out in RM_AddPostNotificationJobForKey that permits
# registration during loading even when masterhost is set. Without that
# carve-out the per-key job would be refused on a replica's own AOF replay
# and metadata would not be rebuilt at startup.
tags "modules aof external:skip" {
    test "perkey-aof-replica: AOF replay on a replica at startup rebuilds metadata" {
        # Master is loaded with the module too so its propagation path is
        # comparable to a real deployment. The point under test is the AOF
        # replay step on the replica at restart, not the initial sync.
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            start_server [list overrides [list \
                    loadmodule $testmodule \
                    appendonly yes \
                    auto-aof-rewrite-percentage 0 \
                    replicaof "$master_host $master_port"]] {
                set replica [srv 0 client]
                wait_for_sync $replica
                # The replica boots with appendonly=yes and replicaof, so
                # post-sync it kicks off a background AOF rewrite. Until that
                # child finishes, propagated commands land in a temp incr
                # file that `debug loadaof` won't see — wait it out before
                # driving the write under test.
                waitForBgrewriteaof $replica

                # Drive a write on the master; replica receives it via
                # propagation and writes it to its own AOF.
                $master hset h_repl f v
                wait_for_ofs_sync $master $replica

                # Sanity: key did propagate
                assert_equal 1 [$replica hexists h_repl f]
                assert_equal "notified" [$replica pkmeta.getmeta h_repl]
                $replica pkmeta.reset

                # Use debug loadaof to exercise the AOF replay path
                # specifically on a configured replica (masterhost set,
                # repl_slave_ro true, server.loading=1). A full restart
                # would re-sync from master via RDB and wipe metadata —
                # that is a separate code path. We deliberately do not
                # rewrite the AOF here: rewriting converts the HSET into
                # the RDB-encoded base AOF, and RDB load (preamble or
                # otherwise) intentionally does not fire KSN. The
                # incremental AOF — which is what propagated commands
                # land in — is what the per-key drain runs against.
                $replica debug loadaof

                # The AOF reload fires the per-key job on the replica; the
                # callback runs with masterhost set, repl_slave_ro on, and
                # server.loading == 1, which is exactly the carve-out.
                assert_equal "notified" [$replica pkmeta.getmeta h_repl]
                assert {[$replica pkmeta.firecount] >= 1}
            }
        }
    }
}

# Master → replica steady-state propagation.
#
# With the replica check dropped, per-key jobs fire on the replica too:
# both sides run the same KSN over the same command stream and maintain
# their per-key state independently. No metadata traffic on the wire.
tags "modules external:skip" {
    test "perkey-repl: replica builds metadata from master-propagated single command" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]

                $replica replicaof $master_host $master_port
                wait_for_sync $replica

                $master pkmeta.reset
                $replica pkmeta.reset

                $master hset h_prop f v
                wait_for_ofs_sync $master $replica

                # Both sides ran the per-key job locally — no metadata
                # crossed the replication stream.
                assert_equal "notified" [$master pkmeta.getmeta h_prop]
                assert_equal "notified" [$replica pkmeta.getmeta h_prop]
                assert_equal 1 [$master pkmeta.firecount]
                assert_equal 1 [$replica pkmeta.firecount]
            }
        }
    }

    test "perkey-repl: replica fires per sub-command for propagated MULTI/EXEC" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]

                $replica replicaof $master_host $master_port
                wait_for_sync $replica

                $master pkmeta.reset
                $replica pkmeta.reset

                $master multi
                $master hset hp1 f v
                $master hset hp2 f v
                $master hset hp3 f v
                $master exec
                wait_for_ofs_sync $master $replica

                assert_equal 3 [$master pkmeta.firecount]
                assert_equal 3 [$replica pkmeta.firecount]
                foreach key {hp1 hp2 hp3} {
                    assert_equal "notified" [$master pkmeta.getmeta $key]
                    assert_equal "notified" [$replica pkmeta.getmeta $key]
                }
            }
        }
    }
}

# Negative coverage: API misuse outside a KSN handler.
#
# The only remaining runtime guard. Calling RM_AddPostNotificationJobForKey
# from a regular module command (not a KSN handler) must return
# REDISMODULE_ERR with a LL_WARNING log entry.
tags "modules external:skip" {
    test "perkey-misuse: registration refused outside a KSN handler" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            assert_error {ERR registration refused*} {r pkmeta.try_outside any_key}
        }
    }
}
