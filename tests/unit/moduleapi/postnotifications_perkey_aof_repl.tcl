set testmodule [file normalize tests/modules/postnotifications_perkey_metadata.so]

# AOF replay on a standalone master: per-key jobs fire during replay with the
# same pattern as normal execution (once per command / per MULTI/EXEC
# sub-command). Metadata isn't in the AOF, so its presence after reload proves
# the callback re-ran; pkmeta.firecount is the load-bearing assertion.
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

                # Reset so the count reflects only the AOF replay path.
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

                # Three HSETs → three firings during replay.
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
                # firings (the original motivating scenario, RED-197766).
                assert_equal 2 [r pkmeta.firecount]
                assert_equal "notified" [r pkmeta.getmeta h_hexp]
            }
        }
    }
}

# RDB load decodes keys directly without running commands, so no KSN fires and
# per-key callbacks do not run. This pins that boundary: making RDB load fire
# KSN would break this assertion and force an explicit decision.
tags "modules external:skip" {
    test "perkey-rdb: RDB-only restart does NOT rebuild metadata (no KSN on RDB load)" {
        start_server [list overrides [list loadmodule "$testmodule" appendonly no]] {
            r hset h_rdb f v
            assert_equal "notified" [r pkmeta.getmeta h_rdb]
            assert_equal 1 [r pkmeta.firecount]

            r pkmeta.reset
            r debug reload

            # Key is back, but metadata is not (not persisted by this module)
            # and the per-key job did NOT fire during load.
            assert_equal "hash" [r type h_rdb]
            assert_equal {} [r pkmeta.getmeta h_rdb]
            assert_equal 0 [r pkmeta.firecount]
        }
    } {} {needs:debug}
}

# AOF replay on a replica at startup: exercises the carve-out that permits
# registration during loading even when masterhost is set. Without it the
# per-key job would be refused on a replica's own AOF replay.
tags "modules aof external:skip" {
    test "perkey-aof-replica: AOF replay on a replica at startup rebuilds metadata" {
        # Master also loads the module so propagation matches a real
        # deployment. Under test is the replica's AOF replay, not initial sync.
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
                # Post-sync the replica kicks off a background AOF rewrite;
                # until it finishes propagated commands land in a temp incr
                # file that `debug loadaof` won't see. Wait it out first.
                waitForBgrewriteaof $replica

                # Write on the master; replica gets it via propagation and
                # writes it to its own AOF.
                $master hset h_repl f v
                wait_for_ofs_sync $master $replica

                # Sanity: key did propagate
                assert_equal 1 [$replica hexists h_repl f]
                assert_equal "notified" [$replica pkmeta.getmeta h_repl]
                $replica pkmeta.reset

                # debug loadaof exercises the AOF replay path on a configured
                # replica (masterhost set, repl_slave_ro, loading=1) — exactly
                # the carve-out. A full restart would re-sync via RDB (separate
                # path). We don't rewrite the AOF: that would turn the HSET into
                # an RDB base AOF, and RDB load doesn't fire KSN; the per-key
                # drain runs against the incremental AOF instead.
                $replica debug loadaof

                # The reload fired the per-key job on the replica.
                assert_equal "notified" [$replica pkmeta.getmeta h_repl]
                assert {[$replica pkmeta.firecount] >= 1}
            }
        }
    }
}

# Master → replica steady-state propagation: per-key jobs fire on both sides,
# each running the same KSN over the same command stream and keeping its own
# state. No metadata crosses the wire.
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

                # Both sides ran the job locally — nothing crossed the wire.
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

# Negative coverage: calling the API from a regular command (not a KSN
# handler) must return REDISMODULE_ERR.
tags "modules external:skip" {
    test "perkey-misuse: registration refused outside a KSN handler" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            assert_error {ERR registration refused*} {r pkmeta.try_outside any_key}
        }
    }
}

# Per-key firing order / granularity on a standalone master. Since a callback
# may not touch the keyspace, order and granularity are observed through the
# module-internal fire log.
tags "modules external:skip" {
    test "perkey-order: fires once per key in submission order across a MULTI/EXEC" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            r multi
            r hset ha f v
            r hset hb f v
            r hset hc f v
            r exec
            # One firing per sub-command, in order.
            assert_equal {ha hb hc} [r pkmeta.firelog]
            assert_equal 3 [r pkmeta.firecount]
        }
    }

    test "perkey-order: per-key job fires between sub-commands inside MULTI/EXEC" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r flushall
            r pkmeta.reset
            r multi
            r hset oa f v
            r hset ob f v
            r hset oc f v
            r exec
            assert_equal {oa ob oc} [r pkmeta.firelog]
            # DB size observed at each firing. If jobs fire between sub-commands
            # the size grows 1,2,3 as each HSET creates its key; if they were
            # batched at the end of EXEC every firing would observe all 3 keys.
            assert_equal {1 2 3} [r pkmeta.dbsizelog]
        }
    }

    test "perkey-order: per-key job fires between commands inside a script (EVAL)" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r flushall
            r pkmeta.reset
            # Three HSETs from inside a single script. The per-key drain runs
            # after each redis.call() returns (scriptCall in script.c), so each
            # firing observes the db growing 1,2,3 — proving firing happens
            # between script commands, not batched at the script's end.
            r eval {
                redis.call('hset', KEYS[1], 'f', 'v')
                redis.call('hset', KEYS[2], 'f', 'v')
                redis.call('hset', KEYS[3], 'f', 'v')
                return 'ok'
            } 3 ea eb ec
            assert_equal {ea eb ec} [r pkmeta.firelog]
            assert_equal {1 2 3} [r pkmeta.dbsizelog]
        }
    }

    test "perkey-order: multi-key command fires one job per affected key" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            # MSET emits one KSN per key → one job per key.
            r mset ma 1 mb 2 mc 3
            assert_equal {ma mb mc} [r pkmeta.firelog]
            assert_equal 3 [r pkmeta.firecount]
        }
    }
}

# No-write contract: a per-key callback may not emit any RM_Call.
tags "modules external:skip" {
    test "perkey-contract: RM_Call from inside a per-key callback is refused" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            set repl [attach_to_replication_stream]

            r set probe_x 1

            # Refused, and no keyspace write happened.
            assert_equal 1 [r pkmeta.rmcall_blocked]
            assert_equal 0 [r exists pkmeta_rmcall_sink]

            # Only the originating SET propagated.
            assert_replication_stream $repl {
                {select *}
                {set probe_x 1}
            }
            close_replication_stream $repl
        }
    }

    test "perkey-contract: refusal repeats per firing and never writes" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            r mset probe_a 1 probe_b 2 probe_c 3
            # One refused RM_Call per affected key, still no sink key.
            assert_equal 3 [r pkmeta.rmcall_blocked]
            assert_equal 0 [r exists pkmeta_rmcall_sink]
        }
    }
}

# No-keyspace contract: a per-key callback may not fire a keyspace notification.
# RM_NotifyKeyspaceEvent is refused for the same reason RM_Call is — a nested
# notification could enqueue further per-key jobs mid-drain.
tags "modules external:skip" {
    test "perkey-contract: RM_NotifyKeyspaceEvent from inside a per-key callback is refused" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            set repl [attach_to_replication_stream]

            r set notifyprobe_x 1

            # The notification attempt was refused.
            assert_equal 1 [r pkmeta.notify_blocked]

            # Only the originating SET propagated; the refused notification did
            # not dispatch anything.
            assert_replication_stream $repl {
                {select *}
                {set notifyprobe_x 1}
            }
            close_replication_stream $repl
        }
    }

    test "perkey-contract: notification refusal repeats per firing" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            r mset notifyprobe_a 1 notifyprobe_b 2 notifyprobe_c 3
            # One refused RM_NotifyKeyspaceEvent per affected key.
            assert_equal 3 [r pkmeta.notify_blocked]
        }
    }
}

# Expiry / eviction: the per-key job must fire when a key disappears — the real
# metadata-cleanup use case.
tags "modules external:skip" {
    test "perkey-expire: lazy expire on read fires the per-key job (drain mid-read)" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            # Disable active expire so the key is reaped by the READ, not by cron.
            r debug set-active-expire 0
            r set ek v px 10
            r pkmeta.reset                        ;# clear the live-key SET firing(s)
            after 20                              ;# key is now logically expired
            # The GET triggers lazy expire + the "expired" KSN; the per-key job
            # fires from inside the read's lookup and sees an empty key.
            assert_equal {} [r get ek]
            assert_equal 1 [r pkmeta.empty_firecount]
            assert_equal {ek} [r pkmeta.empty_firelog]
            r debug set-active-expire 1
        }
    }

    test "perkey-expire: active expire (cron) fires the per-key job without a read" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r debug set-active-expire 1
            r set ak v px 50
            r pkmeta.reset
            # Never touch ak — let the active-expire cycle reap it. The poll loop
            # only issues pkmeta.empty_firecount, which does not access ak.
            wait_for_condition 50 20 {
                [r pkmeta.empty_firecount] == 1
            } else {
                fail "active expire did not fire the per-key job for ak"
            }
            assert_equal {ak} [r pkmeta.empty_firelog]
        }
    }
}

# Pointer-safety guard: the per-key job's first RM_SetKeyMeta attach reallocates
# the key's kvobj, so the job must run only after the triggering command has
# fully returned (the afterCommand / postExecutionUnitOperations drain).
# SMOVE reproduces the crash if the drain runs inside the command's own notification.
tags "modules external:skip" {
    test "perkey-ptr-safety: SMOVE first metadata attach must not UAF the source set" {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            r pkmeta.reset
            r sadd s a b
            r sadd d z
            # Reload from RDB so members survive but module metadata is dropped
            # (not persisted, no KSN on RDB load) — makes the next write to 's'
            # a FIRST metadata attach, i.e. the reallocating path.
            r debug reload
            r pkmeta.reset
            assert_equal {} [r pkmeta.getmeta s]
            r smove s d a
            assert_equal {b} [lsort [r smembers s]]
            assert_equal {a z} [lsort [r smembers d]]
            assert_equal PONG [r ping]   ;# server alive: drain ran post-command
        }
    } {} {needs:debug}
}
