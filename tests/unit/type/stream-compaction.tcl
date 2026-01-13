# Tests for stream compaction features:
# 1. Garbage collection: remove deleted entries from listpacks
# 2. Node merging: merge sparse adjacent nodes

# Helper to get the number of radix tree keys (nodes) in a stream
proc stream_node_count {key} {
    dict get [r XINFO STREAM $key] radix-tree-keys
}

# Helper to get stream length
proc stream_len {key} {
    r XLEN $key
}

start_server {tags {"stream"}} {
    # ============================================================
    # PART 1: Garbage Collection Tests
    # ============================================================

    test {GC - Basic: deleting >50% entries triggers compaction} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add exactly 10 entries to ensure they go into one node
        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Initially we should have 1 node
        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes == 1}

        # Delete 6 entries (>50%) - this should trigger GC
        for {set i 0} {$i < 6} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        # The stream should still work correctly
        assert_equal [stream_len mystream] 4

        # Verify remaining entries are intact
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 4
        for {set i 0} {$i < 4} {incr i} {
            set expected_val [expr {$i + 6}]
            assert_equal [lindex $remaining $i 1] [list f $expected_val]
        }

        r config set stream-node-max-entries 100
    }

    test {GC - No compaction when <50% deleted} {
        r DEL mystream
        r config set stream-node-max-entries 10

        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Delete only 4 entries (<50%) - no GC should happen
        for {set i 0} {$i < 4} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        assert_equal [stream_len mystream] 6

        # Verify all remaining entries are accessible
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 6

        r config set stream-node-max-entries 100
    }

    test {GC - Single-node stream stays intact} {
        r DEL mystream
        r config set stream-node-max-entries 5

        # Create a stream with only one node
        set ids {}
        for {set i 0} {$i < 5} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        assert_equal [stream_node_count mystream] 1

        # Delete all but one entry - last node should remain
        for {set i 0} {$i < 4} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        # Stream should still have 1 entry
        assert_equal [stream_len mystream] 1
        assert_equal [stream_node_count mystream] 1

        # The remaining entry should be accessible
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 1
        assert_equal [lindex $remaining 0 1] {f 4}

        r config set stream-node-max-entries 100
    }

    test {GC - Multiple nodes, only non-last nodes get GC'd} {
        r DEL mystream
        r config set stream-node-max-entries 5

        # Create stream with multiple nodes (at least 3 nodes)
        set ids {}
        for {set i 0} {$i < 15} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 3}

        # Delete most entries from the first node (entries 0-3)
        for {set i 0} {$i < 4} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        # Delete most entries from a middle node (entries 5-8)
        for {set i 5} {$i < 9} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        # The stream should still be valid
        assert_equal [stream_len mystream] 7

        # All remaining entries should be accessible
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 7

        r config set stream-node-max-entries 100
    }

    test {GC - Stress test with random deletions} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add many entries to create multiple nodes
        set ids {}
        for {set i 0} {$i < 200} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        set initial_len [stream_len mystream]
        assert_equal $initial_len 200

        # Randomly delete about 70% of entries
        set ids_to_delete [lrange [lshuffle $ids] 0 139]
        foreach id $ids_to_delete {
            r XDEL mystream $id
        }

        # Verify length
        assert_equal [stream_len mystream] 60

        # Verify all remaining entries are accessible via XRANGE
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 60

        # Verify XREVRANGE works too
        set rev_remaining [r XREVRANGE mystream + -]
        assert_equal [llength $rev_remaining] 60

        r config set stream-node-max-entries 100
    }

    test {GC - Works correctly with XTRIM MAXLEN} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add entries
        for {set i 0} {$i < 50} {incr i} {
            r XADD mystream * f $i
        }

        # Trim to 10 entries
        r XTRIM mystream MAXLEN = 10

        assert_equal [stream_len mystream] 10

        # Verify entries are the last 10
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 10
        for {set i 0} {$i < 10} {incr i} {
            set expected_val [expr {$i + 40}]
            assert_equal [lindex $remaining $i 1] [list f $expected_val]
        }

        r config set stream-node-max-entries 100
    }

    test {GC - Works correctly with XTRIM MINID} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add entries with specific IDs
        set ids {}
        for {set i 1} {$i <= 50} {incr i} {
            lappend ids [r XADD mystream $i-0 f $i]
        }

        # Trim entries with ID < 40-0
        r XTRIM mystream MINID = 40-0

        assert_equal [stream_len mystream] 11

        # Verify entries start from 40-0
        set remaining [r XRANGE mystream - +]
        assert_equal [lindex $remaining 0 0] "40-0"

        r config set stream-node-max-entries 100
    }

    test {GC - Entries with different field names (non-SAMEFIELDS)} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add entries with different field names
        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream * field$i value$i]
        }

        # Delete 6 entries to trigger GC
        for {set i 0} {$i < 6} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        assert_equal [stream_len mystream] 4

        # Verify remaining entries have correct field names
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 4
        for {set i 0} {$i < 4} {incr i} {
            set expected_idx [expr {$i + 6}]
            assert_equal [lindex $remaining $i 1] [list field$expected_idx value$expected_idx]
        }

        r config set stream-node-max-entries 100
    }

    test {GC - Works with consumer groups} {
        r DEL mystream
        r config set stream-node-max-entries 10

        # Add entries
        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Create consumer group
        r XGROUP CREATE mystream mygroup 0

        # Read some entries
        r XREADGROUP GROUP mygroup consumer1 COUNT 5 STREAMS mystream >

        # Delete entries that were read (triggering GC)
        for {set i 0} {$i < 6} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        assert_equal [stream_len mystream] 4

        # Consumer group should still work
        r XPENDING mystream mygroup
        # The remaining unread entries should still be readable
        set remaining [r XREADGROUP GROUP mygroup consumer1 STREAMS mystream >]

        r config set stream-node-max-entries 100
    }

    test {GC - Iterator continues correctly after GC during XDEL} {
        r DEL mystream
        r config set stream-node-max-entries 5

        # Add entries to create multiple nodes
        set ids {}
        for {set i 0} {$i < 20} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Delete entries scattered across nodes
        # This tests that the iterator re-seeks correctly after compaction
        r XDEL mystream [lindex $ids 0]
        r XDEL mystream [lindex $ids 1]
        r XDEL mystream [lindex $ids 2]
        r XDEL mystream [lindex $ids 5]
        r XDEL mystream [lindex $ids 6]
        r XDEL mystream [lindex $ids 7]
        r XDEL mystream [lindex $ids 10]
        r XDEL mystream [lindex $ids 11]
        r XDEL mystream [lindex $ids 12]

        # Verify remaining entries
        assert_equal [stream_len mystream] 11

        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 11

        r config set stream-node-max-entries 100
    }

    # ============================================================
    # PART 2: Node Merging Tests
    #
    # Merge happens when:
    # 1. Current node is sparse (< 50% of max_entries)
    # 2. Combined entries/bytes stay under max_entries/max_bytes
    # 3. Next node is not the tail
    # ============================================================

    test {Merge - Basic: sparse nodes merge when combined fits} {
        r DEL mystream
        # With 100 max entries, a node becomes "sparse" when < 50 entries (50%)
        r config set stream-node-max-entries 100

        # Create 3 nodes with ~100 entries each
        set ids1 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids1 [r XADD mystream * f $i]
        }
        set ids2 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids2 [r XADD mystream * f [expr {100 + $i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids3 [r XADD mystream * f [expr {200 + $i}]]
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 3}

        # FIRST: Make node 2 sparse by deleting most entries (keep ~15)
        # This way when node 1 becomes sparse, combined (~20) < 100
        for {set i 15} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids2 $i]
        }

        # NOW: Make node 1 sparse (keep only 5 entries)
        # This should trigger merge with sparse node 2
        for {set i 5} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Verify merge happened: should go from 3 nodes to 2
        set final_nodes [stream_node_count mystream]
        assert {$final_nodes < $initial_nodes}

        # Verify stream data is correct
        # Kept: 5 from node1 + 15 from node2 + 100 from node3 = 120
        assert_equal [stream_len mystream] 120

        # All remaining entries should be accessible
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 120

        r config set stream-node-max-entries 100
    }

    test {Merge - No merge when combined exceeds limit} {
        r DEL mystream
        r config set stream-node-max-entries 100

        # Create 3 nodes with ~100 entries each
        set ids1 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids1 [r XADD mystream * f $i]
        }
        set ids2 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids2 [r XADD mystream * f [expr {100 + $i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids3 [r XADD mystream * f [expr {200 + $i}]]
        }

        set initial_nodes [stream_node_count mystream]

        # Make node 1 sparse, but node 2 is still full (100 entries)
        # Combined would be ~105 > 100, so NO merge
        for {set i 5} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Node count should remain the same (no merge possible)
        set final_nodes [stream_node_count mystream]
        assert_equal $final_nodes $initial_nodes

        # Stream should still be correct
        assert_equal [stream_len mystream] 205
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 205

        r config set stream-node-max-entries 100
    }

    test {Merge - Last node is never merged into} {
        r DEL mystream
        r config set stream-node-max-entries 20

        # Create ONLY 2 nodes
        set ids1 {}
        for {set i 0} {$i < 20} {incr i} {
            lappend ids1 [r XADD mystream * f $i]
        }
        set ids2 {}
        for {set i 0} {$i < 5} {incr i} {
            # Make node 2 sparse so combined would fit (1 + 5 = 6 < 20)
            lappend ids2 [r XADD mystream * f [expr {20 + $i}]]
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 2}

        # Delete most entries from node 1 (first node) to make it sparse
        # Even though combined would fit (1 + 5 = 6 < 20), node 2 is the LAST
        # node, so merge should NOT happen
        for {set i 1} {$i < 20} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Node count should NOT decrease (no merge into last node)
        set final_nodes [stream_node_count mystream]
        assert_equal $final_nodes $initial_nodes

        # The stream should still work correctly
        assert_equal [stream_len mystream] 6

        # All entries still accessible
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 6

        r config set stream-node-max-entries 100
    }

    test {Merge - Nodes with different master fields don't merge} {
        r DEL mystream
        r config set stream-node-max-entries 20

        # Create 3 nodes with different field names to prevent merging
        # Node 1: field name "a"
        set ids1 {}
        for {set i 0} {$i < 20} {incr i} {
            lappend ids1 [r XADD mystream * a $i]
        }
        # Node 2: field name "b" (different master) - make it sparse
        set ids2 {}
        for {set i 0} {$i < 5} {incr i} {
            lappend ids2 [r XADD mystream * b [expr {20 + $i}]]
        }
        # Node 3: field name "c" (different master)
        set ids3 {}
        for {set i 0} {$i < 20} {incr i} {
            lappend ids3 [r XADD mystream * c [expr {40 + $i}]]
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 3}

        # Delete most entries from node 1 to make it sparse
        # Even though combined entries would fit (1 + 5 = 6 < 20),
        # masters are different (a vs b), so NO merge
        for {set i 1} {$i < 20} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Node count should NOT decrease (no merge due to different masters)
        set final_nodes [stream_node_count mystream]
        assert_equal $final_nodes $initial_nodes

        # Stream should still be valid
        assert_equal [stream_len mystream] 26

        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 26

        r config set stream-node-max-entries 100
    }

    test {Merge - Size limits are respected} {
        r DEL mystream
        r config set stream-node-max-entries 100
        r config set stream-node-max-bytes 1000

        # Create entries that take up space
        set ids1 {}
        for {set i 0} {$i < 50} {incr i} {
            # Add entries with larger values to consume bytes
            lappend ids1 [r XADD mystream * f [string repeat "x" 10]]
        }

        set ids2 {}
        for {set i 0} {$i < 50} {incr i} {
            lappend ids2 [r XADD mystream * f [string repeat "y" 10]]
        }

        # Delete most entries from node 1
        for {set i 1} {$i < 50} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Stream should still be valid regardless of whether merge happened
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 51

        r config set stream-node-max-bytes 4096
        r config set stream-node-max-entries 100
    }

    test {Merge - Data integrity after merge} {
        r DEL mystream
        r config set stream-node-max-entries 100

        # Create 3 nodes
        set all_ids {}
        for {set node 0} {$node < 3} {incr node} {
            for {set i 0} {$i < 100} {incr i} {
                set val [expr {$node * 100 + $i}]
                lappend all_ids [r XADD mystream * f $val]
            }
        }

        set initial_nodes [stream_node_count mystream]

        # FIRST: Make node 2 sparse by deleting most entries (keep 10)
        for {set i 110} {$i < 200} {incr i} {
            r XDEL mystream [lindex $all_ids $i]
        }

        # NOW: Make node 1 sparse (keep entries 0-4)
        # Combined: 5 + 10 = 15 < 100, should merge
        for {set i 5} {$i < 100} {incr i} {
            r XDEL mystream [lindex $all_ids $i]
        }

        # Verify merge happened
        set final_nodes [stream_node_count mystream]
        assert {$final_nodes < $initial_nodes}

        # Verify remaining data is correct
        set remaining [r XRANGE mystream - +]

        # We should have 5 from node1 + 10 from node2 + 100 from node3 = 115
        assert_equal [llength $remaining] 115

        # Verify first 5 entries (kept from first node, now in merged node)
        for {set i 0} {$i < 5} {incr i} {
            set entry [lindex $remaining $i]
            assert_equal [lindex $entry 1] [list f $i]
        }

        # Verify entries from second node (indices 5-14)
        for {set i 0} {$i < 10} {incr i} {
            set entry [lindex $remaining [expr {5 + $i}]]
            set expected_val [expr {100 + $i}]
            assert_equal [lindex $entry 1] [list f $expected_val]
        }

        # Verify entries from third node (indices 15-114)
        for {set i 0} {$i < 100} {incr i} {
            set entry [lindex $remaining [expr {15 + $i}]]
            set expected_val [expr {200 + $i}]
            assert_equal [lindex $entry 1] [list f $expected_val]
        }

        r config set stream-node-max-entries 100
    }

    test {Merge - Stress test with many deletions} {
        r DEL mystream
        r config set stream-node-max-entries 50

        # Create many entries across many nodes
        set all_ids {}
        for {set i 0} {$i < 500} {incr i} {
            lappend all_ids [r XADD mystream * f $i]
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 10}

        # Randomly delete 80% of entries - this should trigger many GC and merge operations
        set ids_shuffled [lshuffle $all_ids]
        set ids_to_delete [lrange $ids_shuffled 0 399]

        foreach id $ids_to_delete {
            r XDEL mystream $id
        }

        # Verify stream integrity
        assert_equal [stream_len mystream] 100

        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 100

        # Verify reverse range works
        set rev_remaining [r XREVRANGE mystream + -]
        assert_equal [llength $rev_remaining] 100

        r config set stream-node-max-entries 100
    }

    test {Merge - Multiple sequential merges} {
        r DEL mystream
        r config set stream-node-max-entries 20

        # Create 5 nodes
        set all_ids {}
        for {set node 0} {$node < 5} {incr node} {
            for {set i 0} {$i < 20} {incr i} {
                lappend all_ids [r XADD mystream * f [expr {$node * 20 + $i}]]
            }
        }

        set initial_nodes [stream_node_count mystream]
        assert {$initial_nodes >= 5}

        # Delete from alternating nodes to trigger multiple potential merges
        # Delete most of node 1 (indices 20-39, keep 20-21)
        for {set i 22} {$i < 40} {incr i} {
            r XDEL mystream [lindex $all_ids $i]
        }

        # Delete most of node 3 (indices 60-79, keep 60-61)
        for {set i 62} {$i < 80} {incr i} {
            r XDEL mystream [lindex $all_ids $i]
        }

        # Verify stream integrity
        set remaining [r XRANGE mystream - +]
        set expected_count [expr {20 + 2 + 20 + 2 + 20}] ;# 64 entries
        assert_equal [llength $remaining] $expected_count
        assert_equal [stream_len mystream] $expected_count

        r config set stream-node-max-entries 100
    }

    test {Merge - Works with XREADGROUP after merge} {
        r DEL mystream
        r config set stream-node-max-entries 50

        # Create entries
        set ids {}
        for {set i 0} {$i < 150} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Create consumer group
        r XGROUP CREATE mystream mygroup 0

        # Read some entries from first node
        r XREADGROUP GROUP mygroup consumer1 COUNT 25 STREAMS mystream >

        # Delete most of first node to trigger merge
        for {set i 5} {$i < 50} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        # Continue reading - should work after merge
        set result [r XREADGROUP GROUP mygroup consumer1 COUNT 50 STREAMS mystream >]

        # Verify we can still read entries
        assert {[llength $result] > 0}

        r config set stream-node-max-entries 100
    }

    test {Merge - Tombstones from neighbor node are skipped during merge} {
        r DEL mystream
        r config set stream-node-max-entries 100

        # Create 3 nodes with 100 entries each
        set ids1 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids1 [r XADD mystream * f $i]
        }
        set ids2 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids2 [r XADD mystream * f [expr {100 + $i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids3 [r XADD mystream * f [expr {200 + $i}]]
        }

        assert {[stream_node_count mystream] >= 3}

        # Delete 40% from node 2 - this is < 50%, so NO GC happens.
        # Node 2 now has 60 live entries + 40 tombstones.
        for {set i 0} {$i < 40} {incr i} {
            r XDEL mystream [lindex $ids2 $i]
        }

        # Delete 90% from node 1 - this triggers GC on node 1.
        # After GC, node 1 has 10 live entries.
        # Merge check: 10 + 60 = 70 < 100, so merge should happen.
        # The key test: tombstones from node 2 must be SKIPPED during merge.
        # If tombstones were copied, merged node would have 10 + 60 + 40 = 110 entries.
        for {set i 10} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Verify merge happened (should go from 3 nodes to 2)
        assert {[stream_node_count mystream] == 2}

        # Verify stream length: 10 + 60 + 100 = 170
        assert_equal [stream_len mystream] 170

        # Verify all remaining entries are accessible and correct
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 170

        # Verify first 10 entries (from node 1)
        for {set i 0} {$i < 10} {incr i} {
            assert_equal [lindex $remaining $i 1] [list f $i]
        }

        # Verify next 60 entries (from node 2, indices 40-99)
        for {set i 0} {$i < 60} {incr i} {
            set expected_val [expr {100 + 40 + $i}]
            assert_equal [lindex $remaining [expr {10 + $i}] 1] [list f $expected_val]
        }

        # Verify last 100 entries (from node 3)
        for {set i 0} {$i < 100} {incr i} {
            set expected_val [expr {200 + $i}]
            assert_equal [lindex $remaining [expr {70 + $i}] 1] [list f $expected_val]
        }

        r config set stream-node-max-entries 100
    }

    test {GC and Merge - Multiple fields per entry} {
        r DEL mystream
        r config set stream-node-max-entries 50

        # Create entries with multiple fields (tests SAMEFIELDS handling)
        set ids1 {}
        for {set i 0} {$i < 50} {incr i} {
            lappend ids1 [r XADD mystream * name user$i age $i city city$i]
        }
        set ids2 {}
        for {set i 0} {$i < 50} {incr i} {
            lappend ids2 [r XADD mystream * name user[expr {50+$i}] age [expr {50+$i}] city city[expr {50+$i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 50} {incr i} {
            lappend ids3 [r XADD mystream * name user[expr {100+$i}] age [expr {100+$i}] city city[expr {100+$i}]]
        }

        assert {[stream_node_count mystream] >= 3}

        # Make node 2 sparse (keep 10 entries)
        for {set i 10} {$i < 50} {incr i} {
            r XDEL mystream [lindex $ids2 $i]
        }

        # Make node 1 sparse to trigger GC + merge
        for {set i 5} {$i < 50} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        # Verify merge happened
        assert {[stream_node_count mystream] < 3}

        # Verify stream length: 5 + 10 + 50 = 65
        assert_equal [stream_len mystream] 65

        # Verify all entries have correct multi-field data
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 65

        # Check first 5 entries (from node 1)
        for {set i 0} {$i < 5} {incr i} {
            set entry [lindex $remaining $i 1]
            assert_equal $entry [list name user$i age $i city city$i]
        }

        # Check next 10 entries (from node 2, indices 50-59)
        for {set i 0} {$i < 10} {incr i} {
            set idx [expr {50 + $i}]
            set entry [lindex $remaining [expr {5 + $i}] 1]
            assert_equal $entry [list name user$idx age $idx city city$idx]
        }

        # Check last 50 entries (from node 3)
        for {set i 0} {$i < 50} {incr i} {
            set idx [expr {100 + $i}]
            set entry [lindex $remaining [expr {15 + $i}] 1]
            assert_equal $entry [list name user$idx age $idx city city$idx]
        }

        r config set stream-node-max-entries 100
    }

    test {GC and Merge - Memory usage consistency} {
        r DEL mystream
        r config set stream-node-max-entries 100

        # Create 3 nodes
        set ids1 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids1 [r XADD mystream * field value$i]
        }
        set ids2 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids2 [r XADD mystream * field value[expr {100+$i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids3 [r XADD mystream * field value[expr {200+$i}]]
        }

        set mem_before [r MEMORY USAGE mystream]

        # Make node 2 sparse
        for {set i 10} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids2 $i]
        }

        # Make node 1 sparse to trigger GC + merge
        for {set i 5} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        set mem_after [r MEMORY USAGE mystream]

        # Memory should decrease after deletions and compaction
        assert {$mem_after < $mem_before}

        # Verify stream is still functional
        assert_equal [stream_len mystream] 115

        # Memory should be reasonable (not zero, not negative internally)
        assert {$mem_after > 0}

        # Verify XINFO STREAM works (uses alloc_size internally)
        set info [r XINFO STREAM mystream]
        assert {[dict exists $info length]}
        assert_equal [dict get $info length] 115

        r config set stream-node-max-entries 100
    }
}

# Tests that require DEBUG commands
start_server {tags {"stream needs:debug"}} {
    test {Merge - RDB save/load preserves merged stream} {
        r DEL mystream
        r config set stream-node-max-entries 100

        # Create 3 nodes
        set ids1 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids1 [r XADD mystream * f $i]
        }
        set ids2 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids2 [r XADD mystream * f [expr {100 + $i}]]
        }
        set ids3 {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids3 [r XADD mystream * f [expr {200 + $i}]]
        }

        # Delete most entries from first node to trigger merge
        for {set i 5} {$i < 100} {incr i} {
            r XDEL mystream [lindex $ids1 $i]
        }

        set nodes_before [dict get [r XINFO STREAM mystream] radix-tree-keys]
        assert_equal [r XLEN mystream] 205

        # Save and reload
        r DEBUG RELOAD

        # Verify stream is intact
        assert_equal [r XLEN mystream] 205
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 205

        # Verify node count is preserved
        set nodes_after [dict get [r XINFO STREAM mystream] radix-tree-keys]
        assert_equal $nodes_before $nodes_after

        r config set stream-node-max-entries 100
    }

    test {GC - RDB save/load preserves compacted stream} {
        r DEL mystream
        r config set stream-node-max-entries 10

        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream * f $i]
        }

        # Delete entries to trigger GC
        for {set i 0} {$i < 6} {incr i} {
            r XDEL mystream [lindex $ids $i]
        }

        assert_equal [r XLEN mystream] 4

        # Save and reload
        r DEBUG RELOAD

        # Verify stream is intact
        assert_equal [r XLEN mystream] 4
        set remaining [r XRANGE mystream - +]
        assert_equal [llength $remaining] 4

        r config set stream-node-max-entries 100
    }
}
