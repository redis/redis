# Vector Sets regression tests.

tags {"aof" "external:skip" "logreqres:skip"} {
    # Vector Sets can be compiled out of the server (make SKIP_VEC_SETS=yes).
    # The AOF crafted below replays VADD commands, and a server without the
    # Vector Sets support would fail to load it, so probe the feature first.
    set has_vsets 0
    start_server {} {
        set has_vsets [server_has_command vadd]
    }

    if {$has_vsets} {
        # VADD with the CAS option blocks its client while a background
        # thread collects the neighbor candidates for the HNSW insertion,
        # and the actual insertion is applied later, in the reply callback.
        # The AOF loader replays commands on a fake client that must
        # complete synchronously and must never block, so the CAS option is
        # dropped while the server is loading, as it already is for
        # replication links, Lua scripts and MULTI/EXEC. Otherwise the
        # replayed VADD blocks the fake client and the loader aborts on its
        # "fake client should not block" assertion.
        set server_path [tmpdir server.vset-aof-cas]
        set aof_dirpath "$server_path/appendonlydir"
        create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1$::incr_aof_suffix$::aof_format_suffix" {
            append_to_aof [formatCommand select 0]
            # The first VADD creates the vector set, the second one adds a
            # new element to the existing set: with CAS this last variant is
            # the one that would block its client.
            append_to_aof [formatCommand vadd k VALUES 2 1 0 e1 CAS]
            append_to_aof [formatCommand vadd k VALUES 2 0 1 e2 CAS]
        }
        create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof$::manifest_suffix" {
            append_to_manifest "file appendonly.aof.1$::incr_aof_suffix$::aof_format_suffix seq 1 type i\n"
        }

        start_server [list overrides [list dir $server_path appendonly yes auto-aof-rewrite-percentage 0] keep_persistence true] {
            test {VADD with the CAS option in the AOF does not crash the server on load} {
                r select 0
                assert_equal PONG [r ping]
                assert_equal 2 [r vcard k]
                # Both elements were actually inserted, with their vectors.
                assert_equal e1 [lindex [r vsim k VALUES 2 1 0] 0]
                assert_equal e2 [lindex [r vsim k VALUES 2 0 1] 0]
            }
        }
    }
}
