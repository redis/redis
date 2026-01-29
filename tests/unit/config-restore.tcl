# ============================================================================
# IMPORTANT: Test Structure Guidelines
# ============================================================================
# All assertions in this file are wrapped in test blocks rather than being
# placed outside test blocks. This is critical for --only flag compatibility.
#
# Reason: When running with --only flag (e.g., --only "Cluster: Multi-replica"),
# the test framework skips tests that don't match the filter, but still executes
# all code outside test blocks. If assertions are outside test blocks, they will
# fail when their dependent tests are skipped.
#
# Example of WRONG pattern:
#   test "Modify config" { r config set maxmemory 100000000 } {} {config:restore}
#   assert_equal $baseline [lindex [r config get maxmemory] 1]  # FAILS with --only
#
# Example of CORRECT pattern:
#   test "Modify config" { r config set maxmemory 100000000 } {} {config:restore}
#   test "Verify restoration" { assert_equal $baseline [...] }  # Works with --only
# ============================================================================

# ============================================================================
# SECTION 1: BASIC FUNCTIONAL TESTS
# ============================================================================
# Purpose: Verify core functionality of config:restore tag
# ============================================================================

start_server {} {
    # Capture baseline at start of test
    set original_maxmemory [lindex [r config get maxmemory] 1]

    test "Basic: config:restore tag works - maxmemory" {
        # Change it
        set new_maxmemory [expr $original_maxmemory + 10000000]

        r config set maxmemory $new_maxmemory

        # Verify it changed
        assert_equal $new_maxmemory [lindex [r config get maxmemory] 1]

        # Don't restore manually - let the tag do it
    } {} {config:restore}

    test "Basic: Verify maxmemory was restored" {
        # Verify config was restored after previous test
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
    }

    # Capture more baselines for multi-config tests
    set original_timeout [lindex [r config get timeout] 1]

    test "Basic: config:restore tag works - multiple configs" {
        # Change multiple configs using relative values
        set new_maxmemory [expr $original_maxmemory + 20000000]
        set new_timeout [expr $original_timeout + 300]

        r config set maxmemory $new_maxmemory
        r config set timeout $new_timeout

        # Verify they changed
        assert_equal $new_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $new_timeout [lindex [r config get timeout] 1]

        # Don't restore manually - let the tag do it
    } {} {config:restore}

    test "Basic: Verify multiple configs were restored" {
        # Verify multiple configs were restored
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
    }

    test "Basic: Test WITHOUT config:restore tag - pollution should occur" {
        # Change config without restoration tag using relative value
        set polluted_maxmemory [expr $original_maxmemory + 30000000]
        r config set maxmemory $polluted_maxmemory

        # Verify it changed
        assert_equal $polluted_maxmemory [lindex [r config get maxmemory] 1]

        # This will NOT be restored automatically
    }

    test "Basic: Verify config pollution occurred" {
        # Verify config pollution occurred (no tag on previous test)
        set current_maxmemory [lindex [r config get maxmemory] 1]
        set expected_polluted [expr $original_maxmemory + 30000000]
        assert_equal $expected_polluted $current_maxmemory
    }

    # Clean up - restore to original baseline for next tests
    r config set maxmemory $original_maxmemory
    r config set timeout $original_timeout

    # Capture baseline for nested tests
    set original_tcp_keepalive [lindex [r config get tcp-keepalive] 1]

    test "Basic: Nested tests - Outer test with config:restore" {
        # This test verifies that nested test calls don't interfere with each other's
        # saved_config variables due to TCL's procedure-local variable scoping

        # Outer test changes maxmemory using relative value
        set outer_maxmemory [expr $original_maxmemory + 10000000]
        r config set maxmemory $outer_maxmemory
        assert_equal $outer_maxmemory [lindex [r config get maxmemory] 1]

        # Call a nested test that also uses config:restore
        test "Basic: Nested tests - Inner test with config:restore" {
            # Inner test changes timeout using relative value
            set inner_timeout [expr $original_timeout + 300]
            r config set timeout $inner_timeout
            assert_equal $inner_timeout [lindex [r config get timeout] 1]

            # Verify outer test's maxmemory change is still visible
            assert_equal $outer_maxmemory [lindex [r config get maxmemory] 1]

            # Inner test completes - its config:restore should restore timeout to baseline
        } {} {config:restore}

        # After inner test completes:
        # - timeout should be restored to baseline by inner test's config:restore
        # - maxmemory should still be set (outer test's change)
        set after_inner_timeout [lindex [r config get timeout] 1]
        set after_inner_maxmemory [lindex [r config get maxmemory] 1]

        assert_equal $original_timeout $after_inner_timeout "Inner test should have restored timeout"
        assert_equal $outer_maxmemory $after_inner_maxmemory "Outer test's maxmemory should still be set"

        # Outer test completes - its config:restore should restore maxmemory to baseline
    } {} {config:restore}

    # Verify nested tests restored configs correctly
    test "Basic: Verify nested tests restored configs" {
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
    }

    test "Basic: Nested tests - Verify variable isolation with multiple levels" {
        # Test 3 levels of nesting to ensure variable scoping works correctly

        # Level 1 changes maxmemory
        set level1_maxmemory [expr $original_maxmemory + 11111111]
        r config set maxmemory $level1_maxmemory

        test "Basic: Nested level 2" {
            # Level 2 changes timeout
            set level2_timeout [expr $original_timeout + 222]
            r config set timeout $level2_timeout

            test "Basic: Nested level 3" {
                # Level 3 changes tcp-keepalive
                set level3_tcp_keepalive [expr $original_tcp_keepalive + 333]
                r config set tcp-keepalive $level3_tcp_keepalive

                # All three configs should be set
                assert_equal $level1_maxmemory [lindex [r config get maxmemory] 1]
                assert_equal $level2_timeout [lindex [r config get timeout] 1]
                assert_equal $level3_tcp_keepalive [lindex [r config get tcp-keepalive] 1]
            } {} {config:restore}

            # After level 3: tcp-keepalive restored to baseline, others still set
            assert_equal $level1_maxmemory [lindex [r config get maxmemory] 1]
            assert_equal $level2_timeout [lindex [r config get timeout] 1]
            assert_equal $original_tcp_keepalive [lindex [r config get tcp-keepalive] 1]
        } {} {config:restore}

        # After level 2: timeout restored to baseline, maxmemory still set
        assert_equal $level1_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
        assert_equal $original_tcp_keepalive [lindex [r config get tcp-keepalive] 1]
    } {} {config:restore}

    # Verify 3-level nested tests restored all configs
    test "Basic: Verify 3-level nested tests restored all configs" {
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
        assert_equal $original_tcp_keepalive [lindex [r config get tcp-keepalive] 1]
    }

    test "Basic: config:restore works with config_set helper" {
        # Test that config:restore works with the config_set helper proc
        # config_set is a wrapper around "config set" from tests/support/util.tcl

        set new_maxmemory [expr $original_maxmemory + 60000000]
        set new_timeout [expr $original_timeout + 600]

        # Use config_set helper instead of "r config set"
        config_set maxmemory $new_maxmemory
        config_set timeout $new_timeout

        # Verify changes took effect
        assert_equal $new_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $new_timeout [lindex [r config get timeout] 1]

        # config:restore should restore these changes
    } {} {config:restore}

    # Verify config_set changes were restored
    test "Basic: Verify config_set changes were restored" {
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
    }

    test "Basic: config:restore works with config_get_set helper" {
        # Test that config:restore works with the config_get_set helper proc
        # config_get_set gets the current value and sets a new one (from tests/support/util.tcl)

        set new_maxmemory [expr $original_maxmemory + 70000000]
        set new_timeout [expr $original_timeout + 700]

        # Use config_get_set helper - it returns the old value and sets the new one
        set old_maxmemory [config_get_set maxmemory $new_maxmemory]
        set old_timeout [config_get_set timeout $new_timeout]

        # Verify the old values match our baseline
        assert_equal $original_maxmemory $old_maxmemory
        assert_equal $original_timeout $old_timeout

        # Verify new values were set
        assert_equal $new_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $new_timeout [lindex [r config get timeout] 1]

        # config:restore should restore these changes
    } {} {config:restore}

    # Verify config_get_set changes were restored
    test "Basic: Verify config_get_set changes were restored" {
        assert_equal $original_maxmemory [lindex [r config get maxmemory] 1]
        assert_equal $original_timeout [lindex [r config get timeout] 1]
    }
}

# ============================================================================
# SECTION 2: CLUSTER SCENARIO TESTS
# ============================================================================
# Purpose: Verify config:restore works with cluster-style access patterns
# ============================================================================

# Simple test using start_cluster to verify cluster-style access patterns
start_cluster 1 0 {tags {external:skip cluster bigredis}} {    
    # Simulate cluster-style access by using Rn helper
    set R [Rn 0]

    # Capture baseline values at start_server level
    set cluster_original_maxmemory [lindex [$R config get maxmemory] 1]
    set cluster_original_timeout [lindex [$R config get timeout] 1]

    test "Cluster: Config change via \$R WITHOUT restore tag" {
        # Change config using $R (cluster style)
        set polluted_maxmemory [expr $cluster_original_maxmemory + 50000000]
        $R config set maxmemory $polluted_maxmemory

        # Verify it changed
        assert_equal $polluted_maxmemory [lindex [$R config get maxmemory] 1]

        # This change will persist to next test (no restore tag)
    }

    test "Cluster: Verify pollution occurred" {
        # Verify pollution occurred (no restore tag)
        set expected_polluted [expr $cluster_original_maxmemory + 50000000]
        assert_equal $expected_polluted [lindex [$R config get maxmemory] 1]

        # Clean up - restore to original baseline
        $R config set maxmemory $cluster_original_maxmemory
    }

    test "Cluster: Config change via \$R WITH restore tag" {
        # Change config using $R (cluster style)
        set new_maxmemory [expr $cluster_original_maxmemory + 100000000]
        set new_timeout [expr $cluster_original_timeout + 150]

        $R config set maxmemory $new_maxmemory
        $R config set timeout $new_timeout

        # Verify changes took effect
        assert_equal $new_maxmemory [lindex [$R config get maxmemory] 1]
        assert_equal $new_timeout [lindex [$R config get timeout] 1]

        # These will be automatically restored by the tag
    } {} {config:restore}

    test "Cluster: Verify restoration worked" {
        # Verify restoration worked (via $R)
        assert_equal $cluster_original_maxmemory [lindex [$R config get maxmemory] 1]
        assert_equal $cluster_original_timeout [lindex [$R config get timeout] 1]
    }

    test "Cluster: config:restore works with config_set helper" {
        # Test config_set helper with cluster-style $R access
        set new_maxmemory [expr $cluster_original_maxmemory + 180000000]

        # Use config_set helper (uses implicit 'r' which is equivalent to $R in single-node cluster)
        config_set maxmemory $new_maxmemory
        assert_equal $new_maxmemory [lindex [$R config get maxmemory] 1]
    } {} {config:restore}

    test "Cluster: Verify config_set changes were restored" {
        # Verify config_set changes were restored
        assert_equal $cluster_original_maxmemory [lindex [$R config get maxmemory] 1]
    }

    test "Cluster: config:restore works with config_get_set helper" {
        # Test config_get_set helper with cluster-style $R access
        set new_timeout [expr $cluster_original_timeout + 800]

        # Use config_get_set helper (uses implicit 'r' which is equivalent to $R in single-node cluster)
        set old_timeout [config_get_set timeout $new_timeout]
        assert_equal $cluster_original_timeout $old_timeout
        assert_equal $new_timeout [lindex [$R config get timeout] 1]
    } {} {config:restore}

    test "Cluster: Verify config_get_set changes were restored" {
        # Verify config_get_set changes were restored
        assert_equal $cluster_original_timeout [lindex [$R config get timeout] 1]
    }
}

# Real cluster test with master and replicas
start_cluster 1 2 {tags {external:skip cluster bigredis}} {
    # Create a cluster with 1 master and 2 replicas
    # This tests that config:restore works correctly across all nodes in a real cluster

    # Get clients for master and replicas at cluster level
    # In a 1 master + 2 replicas cluster:
    # - R 0 = master
    # - R 1 = replica 1
    # - R 2 = replica 2
    set master [Rn 0]
    set replica1 [Rn 1]
    set replica2 [Rn 2]

    # Capture baseline configs for all nodes at cluster level (before any tests modify them)
    set master_baseline_maxmemory [lindex [$master config get maxmemory] 1]
    set master_baseline_timeout [lindex [$master config get timeout] 1]

    set replica1_baseline_maxmemory [lindex [$replica1 config get maxmemory] 1]
    set replica1_baseline_timeout [lindex [$replica1 config get timeout] 1]

    set replica2_baseline_maxmemory [lindex [$replica2 config get maxmemory] 1]
    set replica2_baseline_timeout [lindex [$replica2 config get timeout] 1]

    #
    # Tests demonstrating config pollution WITHOUT config:restore tag
    #
    test "Cluster: Pollution demo - Modify configs WITHOUT config:restore tag" {
        # Modify configs on all nodes WITHOUT using config:restore tag
        # This will cause pollution that persists to subsequent tests
        $master config set maxmemory [expr $master_baseline_maxmemory + 50000000]
        $replica1 config set maxmemory [expr $replica1_baseline_maxmemory + 60000000]
        $replica2 config set maxmemory [expr $replica2_baseline_maxmemory + 70000000]

        # Verify changes took effect
        assert_equal [expr $master_baseline_maxmemory + 50000000] [lindex [$master config get maxmemory] 1]
        assert_equal [expr $replica1_baseline_maxmemory + 60000000] [lindex [$replica1 config get maxmemory] 1]
        assert_equal [expr $replica2_baseline_maxmemory + 70000000] [lindex [$replica2 config get maxmemory] 1]
    }
    # Note: No config:restore tag - changes will persist!

    test "Cluster: Pollution demo - Verify pollution persists (expected behavior)" {
        # This test verifies that WITHOUT config:restore, changes persist
        # The configs should still be at the polluted values, NOT the baseline
        set master_current [lindex [$master config get maxmemory] 1]
        set replica1_current [lindex [$replica1 config get maxmemory] 1]
        set replica2_current [lindex [$replica2 config get maxmemory] 1]

        # Verify pollution: values should NOT equal baseline
        assert {$master_current != $master_baseline_maxmemory}
        assert {$replica1_current != $replica1_baseline_maxmemory}
        assert {$replica2_current != $replica2_baseline_maxmemory}

        # Verify pollution: values should equal the polluted values
        assert_equal [expr $master_baseline_maxmemory + 50000000] $master_current
        assert_equal [expr $replica1_baseline_maxmemory + 60000000] $replica1_current
        assert_equal [expr $replica2_baseline_maxmemory + 70000000] $replica2_current
    }

    test "Cluster: Pollution demo - Manually restore to baseline for subsequent tests" {
        # Manually restore configs to baseline so subsequent tests start clean
        $master config set maxmemory $master_baseline_maxmemory
        $replica1 config set maxmemory $replica1_baseline_maxmemory
        $replica2 config set maxmemory $replica2_baseline_maxmemory

        # Verify restoration
        assert_equal $master_baseline_maxmemory [lindex [$master config get maxmemory] 1]
        assert_equal $replica1_baseline_maxmemory [lindex [$replica1 config get maxmemory] 1]
        assert_equal $replica2_baseline_maxmemory [lindex [$replica2 config get maxmemory] 1]
    }

    #
    # Tests demonstrating config:restore tag working correctly
    #
    test "Cluster: Multi-replica - Configure master and all replicas" {
        # Configure each node with different values
        $master config set maxmemory [expr $master_baseline_maxmemory + 100000000]
        $master config set timeout [expr $master_baseline_timeout + 100]

        $replica1 config set maxmemory [expr $replica1_baseline_maxmemory + 200000000]
        $replica1 config set timeout [expr $replica1_baseline_timeout + 200]

        $replica2 config set maxmemory [expr $replica2_baseline_maxmemory + 300000000]
        $replica2 config set timeout [expr $replica2_baseline_timeout + 300]

        # Verify all changes took effect
        assert_equal [expr $master_baseline_maxmemory + 100000000] [lindex [$master config get maxmemory] 1]
        assert_equal [expr $master_baseline_timeout + 100] [lindex [$master config get timeout] 1]

        assert_equal [expr $replica1_baseline_maxmemory + 200000000] [lindex [$replica1 config get maxmemory] 1]
        assert_equal [expr $replica1_baseline_timeout + 200] [lindex [$replica1 config get timeout] 1]

        assert_equal [expr $replica2_baseline_maxmemory + 300000000] [lindex [$replica2 config get maxmemory] 1]
        assert_equal [expr $replica2_baseline_timeout + 300] [lindex [$replica2 config get timeout] 1]
    } {} {config:restore}

    # Verify restoration worked for all nodes
    test "Cluster: Multi-replica - Verify all nodes restored" {
        # Capture current configs (should be restored to baseline)
        set master_current_maxmemory [lindex [$master config get maxmemory] 1]
        set master_current_timeout [lindex [$master config get timeout] 1]

        set replica1_current_maxmemory [lindex [$replica1 config get maxmemory] 1]
        set replica1_current_timeout [lindex [$replica1 config get timeout] 1]

        set replica2_current_maxmemory [lindex [$replica2 config get maxmemory] 1]
        set replica2_current_timeout [lindex [$replica2 config get timeout] 1]

        # Verify master was restored to baseline (captured before previous test)
        assert_equal $master_baseline_maxmemory $master_current_maxmemory
        assert_equal $master_baseline_timeout $master_current_timeout

        # Verify replica1 was restored to baseline
        assert_equal $replica1_baseline_maxmemory $replica1_current_maxmemory
        assert_equal $replica1_baseline_timeout $replica1_current_timeout

        # Verify replica2 was restored to baseline
        assert_equal $replica2_baseline_maxmemory $replica2_current_maxmemory
        assert_equal $replica2_baseline_timeout $replica2_current_timeout
    }

    test "Cluster: Multi-replica - Partial modification (only some nodes)" {
        # Test that restoration works correctly when only some nodes are modified
        # Note: Uses baselines captured at cluster level

        # Modify only master and replica2, leave replica1 unchanged
        $master config set maxmemory [expr $master_baseline_maxmemory + 150000000]
        $replica2 config set maxmemory [expr $replica2_baseline_maxmemory + 350000000]
        # replica1 not modified

        # Verify changes
        assert_equal [expr $master_baseline_maxmemory + 150000000] [lindex [$master config get maxmemory] 1]
        assert_equal $replica1_baseline_maxmemory [lindex [$replica1 config get maxmemory] 1]
        assert_equal [expr $replica2_baseline_maxmemory + 350000000] [lindex [$replica2 config get maxmemory] 1]
    } {} {config:restore}

    # Verify selective restoration
    test "Cluster: Multi-replica - Verify selective restoration" {
        # Get current values (should all be restored to baseline captured at cluster level)
        set master_current_maxmemory [lindex [$master config get maxmemory] 1]
        set replica1_current_maxmemory [lindex [$replica1 config get maxmemory] 1]
        set replica2_current_maxmemory [lindex [$replica2 config get maxmemory] 1]

        # All should be at baseline (including the ones that were modified)
        assert_equal $master_baseline_maxmemory $master_current_maxmemory
        assert_equal $replica1_baseline_maxmemory $replica1_current_maxmemory
        assert_equal $replica2_baseline_maxmemory $replica2_current_maxmemory
    }
}

# ============================================================================
# SECTION 3: MULTI-SERVER TESTS
# ============================================================================
# Purpose: Verify config:restore works with nested servers
# ============================================================================

#
# Multi-Server: Demonstrate config pollution WITHOUT config:restore tag
#

start_server {tags {external:skip}} {
    start_server {tags {external:skip}} {
        start_server {tags {external:skip}} {
            # Capture baselines at server level (before any tests modify them)
            set server0 [Rn 0]
            set server1 [Rn 1]
            set server2 [Rn 2]
            set baseline0 [lindex [$server0 config get maxmemory] 1]
            set baseline1 [lindex [$server1 config get maxmemory] 1]
            set baseline2 [lindex [$server2 config get maxmemory] 1]

            test "MultiServer: Pollution demo - Modify configs WITHOUT config:restore tag" {
                # Modify configs on all servers WITHOUT using config:restore tag
                # This will cause pollution that persists to subsequent tests
                $server0 config set maxmemory [expr $baseline0 + 50000000]
                $server1 config set maxmemory [expr $baseline1 + 60000000]
                $server2 config set maxmemory [expr $baseline2 + 70000000]

                # Verify changes took effect
                assert_equal [expr $baseline0 + 50000000] [lindex [$server0 config get maxmemory] 1]
                assert_equal [expr $baseline1 + 60000000] [lindex [$server1 config get maxmemory] 1]
                assert_equal [expr $baseline2 + 70000000] [lindex [$server2 config get maxmemory] 1]
            } {} {}
            # Note: No config:restore tag - changes will persist!

            test "MultiServer: Pollution demo - Verify pollution persists (expected behavior)" {
                # This test verifies that WITHOUT config:restore, changes persist
                # The configs should still be at the polluted values, NOT the baseline
                set current0 [lindex [$server0 config get maxmemory] 1]
                set current1 [lindex [$server1 config get maxmemory] 1]
                set current2 [lindex [$server2 config get maxmemory] 1]

                # Verify pollution: values should NOT equal baseline
                assert {$current0 != $baseline0}
                assert {$current1 != $baseline1}
                assert {$current2 != $baseline2}

                # Verify pollution: values should equal the polluted values
                assert_equal [expr $baseline0 + 50000000] $current0
                assert_equal [expr $baseline1 + 60000000] $current1
                assert_equal [expr $baseline2 + 70000000] $current2
            } {} {}

            test "MultiServer: Pollution demo - Manually restore to baseline" {
                # Manually restore configs to baseline so subsequent tests start clean
                $server0 config set maxmemory $baseline0
                $server1 config set maxmemory $baseline1
                $server2 config set maxmemory $baseline2

                # Verify restoration
                assert_equal $baseline0 [lindex [$server0 config get maxmemory] 1]
                assert_equal $baseline1 [lindex [$server1 config get maxmemory] 1]
                assert_equal $baseline2 [lindex [$server2 config get maxmemory] 1]
            } {} {}
        }
    }
}

#
# Multi-Server: Two servers with config changes on inner and/or outer servers
#

start_server {tags {external:skip}} {
    start_server {tags {external:skip}} {
        # Capture baselines at start_server level (before any tests modify them)
        set outer_server [Rn 1]
        set inner_server [Rn 0]
        set baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
        set baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
        set baseline_outer_timeout [lindex [$outer_server config get timeout] 1]
        set baseline_inner_timeout [lindex [$inner_server config get timeout] 1]

        test "Two servers - both inner and outer modified" {
            # Modify configs on BOTH servers
            $outer_server config set maxmemory [expr $baseline_outer_maxmem + 100000000]
            $inner_server config set maxmemory [expr $baseline_inner_maxmem + 50000000]
            $outer_server config set timeout [expr $baseline_outer_timeout + 100]
            $inner_server config set timeout [expr $baseline_inner_timeout + 200]

            # Verify changes took effect
            assert_equal [expr $baseline_outer_maxmem + 100000000] [lindex [$outer_server config get maxmemory] 1]
            assert_equal [expr $baseline_inner_maxmem + 50000000] [lindex [$inner_server config get maxmemory] 1]
            assert_equal [expr $baseline_outer_timeout + 100] [lindex [$outer_server config get timeout] 1]
            assert_equal [expr $baseline_inner_timeout + 200] [lindex [$inner_server config get timeout] 1]
        } {} {config:restore}

        test "Verify both inner and outer servers were restored" {
            # Verify configs were restored to baseline (captured before previous test)
            assert_equal $baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
            assert_equal $baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
            assert_equal $baseline_outer_timeout [lindex [$outer_server config get timeout] 1]
            assert_equal $baseline_inner_timeout [lindex [$inner_server config get timeout] 1]
        } {} {}

        test "Two servers - only outer server modified" {
            # Modify config ONLY on the outer server
            $outer_server config set maxmemory [expr $baseline_outer_maxmem + 75000000]

            # Verify change took effect on outer server
            assert_equal [expr $baseline_outer_maxmem + 75000000] [lindex [$outer_server config get maxmemory] 1]
            # Verify inner server was NOT modified
            assert_equal $baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
        } {} {config:restore}

        test "Verify outer server was restored and inner server unchanged" {
            # Verify outer server was restored to baseline
            assert_equal $baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
            # Verify inner server still has its original baseline value
            assert_equal $baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
        } {} {}

        test "Two servers - only inner server modified" {
            # Modify config ONLY on the inner server
            $inner_server config set maxmemory [expr $baseline_inner_maxmem + 80000000]

            # Verify change took effect on inner server
            assert_equal [expr $baseline_inner_maxmem + 80000000] [lindex [$inner_server config get maxmemory] 1]
            # Verify outer server was NOT modified
            assert_equal $baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
        } {} {config:restore}

        test "Verify inner server was restored and outer server unchanged" {
            # Verify inner server was restored to baseline
            assert_equal $baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
            # Verify outer server still has its original baseline value
            assert_equal $baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
        } {} {}
    }
}

#
# Multi-Server: Two servers with config changes, outer server paused during test
# Tests that config:restore skips unresponsive servers (using ping_server_with_timeout)
# and doesn't hang. The paused server is resumed before leaving the start_server block.
#

start_server {tags {external:skip}} {
    start_server {tags {external:skip}} {
        # Capture baselines at start_server level (before any tests modify them)
        set outer_server [Rn 1]
        set inner_server [Rn 0]
        set baseline_outer_maxmem [lindex [$outer_server config get maxmemory] 1]
        set baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]
        set outer_pid [srv -1 pid]

        test "Two servers - outer server paused during test" {
            # Modify configs on BOTH servers
            $outer_server config set maxmemory [expr $baseline_outer_maxmem + 100000000]
            $inner_server config set maxmemory [expr $baseline_inner_maxmem + 50000000]

            # Verify changes took effect
            assert_equal [expr $baseline_outer_maxmem + 100000000] [lindex [$outer_server config get maxmemory] 1]
            assert_equal [expr $baseline_inner_maxmem + 50000000] [lindex [$inner_server config get maxmemory] 1]

            # Pause the outer server (simulates hung/unresponsive server)
            pause_process $outer_pid

            # Verify outer server is paused (not responding to ping)
            set outer_host [srv -1 host]
            set outer_port [srv -1 port]
            if {[ping_server_with_timeout $outer_host $outer_port 100]} {
                fail "Outer server should not respond after pause"
            }
        } {} {config:restore}

        # If we reach here, config:restore didn't hang (it skipped the paused server)
        # Resume the outer server so we can verify and cleanup properly
        # Check if process is in stopped state before resuming
        if {[string match "T*" [get_proc_state $outer_pid]]} {
            resume_process $outer_pid
        }

        test "Verify inner server restored, outer server still modified after pause" {
            # Verify inner server was restored to baseline
            assert_equal $baseline_inner_maxmem [lindex [$inner_server config get maxmemory] 1]

            # Outer server was paused during restore, so its config was NOT restored
            # It should still have the modified value
            assert_equal [expr $baseline_outer_maxmem + 100000000] [lindex [$outer_server config get maxmemory] 1]

            # Manually restore outer server config for clean state
            $outer_server config set maxmemory $baseline_outer_maxmem
        } {OK} {}
    }
}

# Multi-Server: Test with 0 config changes (verify no restoration occurs)
start_server {tags {external:skip}} {
    start_server {tags {external:skip}} {
        # Capture baselines at start_server level (before any tests modify them)
        set server0 [Rn 0]
        set server1 [Rn 1]
        set baseline0 [lindex [$server0 config get maxmemory] 1]
        set baseline1 [lindex [$server1 config get maxmemory] 1]

        test "Two Servers: No config changes - verify no restoration" {
            # Don't modify any configs, just do some data operations
            $server0 set test:key "value"
            $server1 set test:key "value"

            assert_equal "value" [$server0 get test:key]
            assert_equal "value" [$server1 get test:key]

            # No configs were changed, so restoration should be skipped
        } {} {config:restore}

        test "Two Servers: Verify configs unchanged" {
            # Verify configs remain at baseline (no changes were made in previous test)
            assert_equal $baseline0 [lindex [$server0 config get maxmemory] 1]
            assert_equal $baseline1 [lindex [$server1 config get maxmemory] 1]
        } {} {}
    }
}

################################################################################
# SECTION 4: Failure Handling Validation Tests
################################################################################
#
# PURPOSE:
# These tests INTENTIONALLY FAIL to validate that the config:restore mechanism
# in test.tcl (restore_server_configs call at end of test proc) correctly
# restores server configuration even when tests fail.
#
# HOW TO RUN:
#   ./runtest --single unit/config-restore --only "/EXPECTED_FAILURE.*"
#
# EXPECTED OUTPUT:
# - Expected failure count: 8 tests (7 with config:restore, 1 without)
# - Expected pass count: 8 tests (7 restoration verifications, 1 pollution verification)
#
# WHAT THESE TESTS VALIDATE:
# - ✅ That config:restore restores config even when tests fail
# - ✅ That different failure mechanisms all trigger restoration
# - ✅ That non-assertion errors (re-raise path) also trigger restoration
# - ✅ That WITHOUT config:restore tag, config pollution occurs
################################################################################

# Only run failure validation tests when explicitly requested
if {[search_pattern_list "EXPECTED_FAILURE" $::only_tests]} {
    start_server {} {
        #
        # Save baseline config values at the TOP - used by all tests
        # Note: Using maxmemory, timeout, hz, tcp-keepalive (safe configs with no OS limits)
        #
        set ::baseline_maxmemory [lindex [r config get maxmemory] 1]
        set ::baseline_timeout [lindex [r config get timeout] 1]
        set ::baseline_hz [lindex [r config get hz] 1]
        set ::baseline_tcp_keepalive [lindex [r config get tcp-keepalive] 1]

        # ================================================================
        # Test 1: fail() procedure - explicit test failure
        # ================================================================
        test "EXPECTED_FAILURE: fail() procedure" {
            r config set maxmemory [expr {$::baseline_maxmemory + 1000000}]
            fail "Intentional failure to test config:restore"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after fail()" {
            assert_equal $::baseline_maxmemory [lindex [r config get maxmemory] 1]
        } {} {}

        # ================================================================
        # Test 2: assert_equal failure - assertion mismatch
        # ================================================================
        test "EXPECTED_FAILURE: assert_equal mismatch" {
            r config set timeout [expr {$::baseline_timeout + 12345}]
            assert_equal "intentionally_wrong" "this_will_never_match"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after assert_equal" {
            assert_equal $::baseline_timeout [lindex [r config get timeout] 1]
        } {} {}

        # ================================================================
        # Test 3: assert_match failure - pattern mismatch
        # ================================================================
        test "EXPECTED_FAILURE: assert_match pattern mismatch" {
            r config set hz [expr {$::baseline_hz + 5}]
            assert_match "pattern_*_never_matches" "actual_value_here"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after assert_match" {
            assert_equal $::baseline_hz [lindex [r config get hz] 1]
        } {} {}

        # ================================================================
        # Test 4: TCL error command - generic exception
        # ================================================================
        test "EXPECTED_FAILURE: TCL error command" {
            r config set maxmemory [expr {$::baseline_maxmemory + 2000000}]
            error "assertion:Intentional TCL error to test config:restore"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after TCL error" {
            assert_equal $::baseline_maxmemory [lindex [r config get maxmemory] 1]
        } {} {}

        # ================================================================
        # Test 4b: Non-assertion TCL error (re-raise path)
        # This tests the fix in test.tcl where restore_server_configs is
        # called BEFORE re-raising non-assertion errors (in the else branch
        # of the assertion check within the catch block). Without the fix,
        # the error command would propagate immediately and skip the normal
        # restoration code at the end of the test proc.
        #
        # NOTE: We wrap the test call in catch{} to prevent the re-raised
        # error from terminating the test client. This allows us to run
        # WITHOUT --durable, ensuring the re-raise path is actually executed.
        # ================================================================
        set caught [catch {
            test "EXPECTED_FAILURE: non-assertion error re-raise path" {
                r config set hz [expr {$::baseline_hz + 5}]
                # This error does NOT start with "assertion:" so it triggers
                # the re-raise path when ::durable is false
                error "Non-assertion error to test config restoration before re-raise"
            } {} {config:restore}
        } err]

        test "EXPECTED_FAILURE: verify restoration after non-assertion error" {
            # Verify the error was actually re-raised and caught
            assert_equal 1 $caught "Expected catch to return 1 (error caught)"
            # Verify config was restored before the error was re-raised
            assert_equal $::baseline_hz [lindex [r config get hz] 1]
        } {} {}

        # ================================================================
        # Test 5: assert failure - boolean condition
        # ================================================================
        test "EXPECTED_FAILURE: assert boolean failure" {
            r config set timeout [expr {$::baseline_timeout + 54321}]
            assert {1 == 0}
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after assert" {
            assert_equal $::baseline_timeout [lindex [r config get timeout] 1]
        } {} {}

        # ================================================================
        # Test 6: Multiple config changes before failure
        # ================================================================
        test "EXPECTED_FAILURE: multiple config changes" {
            r config set maxmemory [expr {$::baseline_maxmemory + 3000000}]
            r config set timeout [expr {$::baseline_timeout + 7777}]
            r config set hz [expr {$::baseline_hz + 3}]
            fail "Intentional failure after multiple config changes"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after multiple changes" {
            assert_equal $::baseline_maxmemory [lindex [r config get maxmemory] 1]
            assert_equal $::baseline_timeout [lindex [r config get timeout] 1]
            assert_equal $::baseline_hz [lindex [r config get hz] 1]
        } {} {}

        # ================================================================
        # Test 7: Pollution test - fail WITHOUT config:restore tag
        # This proves that without the tag, config changes persist (pollution)
        # ================================================================
        test "EXPECTED_FAILURE: no config:restore tag causes pollution" {
            r config set tcp-keepalive [expr {$::baseline_tcp_keepalive + 999}]
            fail "Intentional failure WITHOUT config:restore tag"
        } {} {}

        test "EXPECTED_FAILURE: verify pollution occurred (no restoration)" {
            # Config should NOT be restored - proving pollution without the tag
            set current [lindex [r config get tcp-keepalive] 1]
            set expected [expr {$::baseline_tcp_keepalive + 999}]
            assert_equal $expected $current
            # Manually restore for cleanup
            r config set tcp-keepalive $::baseline_tcp_keepalive
        } {OK} {}
    }
} else {
    if {$::verbose} {
        puts "\nSkipping failure handling tests because --only \"/EXPECTED_FAILURE.*\" was not specified\n"
    }
}

# ============================================================================
# SECTION 5: PERFORMANCE BENCHMARKS
# ============================================================================
# Purpose: Measure performance overhead of config:restore feature
#
# HOW TO RUN ALL BENCHMARKS:
#   ./runtest --single unit/config-restore --only "/BENCHMARK:.*"
#
# HOW TO RUN A SPECIFIC BENCHMARK:
#   ./runtest --single unit/config-restore --only "/BENCHMARK:tag-overhead.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:with-changes.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:2servers.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:3servers-all.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:3servers-2mod.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:3servers-0mod.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:cluster.*"
#
# Standalone benchmark (no inner tests):
#   ./runtest --single unit/config-restore --only "/BENCHMARK:config-get.*"
# ============================================================================

# Helper to check if benchmarks should run based on --only patterns.
# Returns 1 if any pattern in ::only_tests targets benchmark tests.
# This handles both exact match patterns (e.g., "BENCHMARK:config-get")
# and regex patterns (e.g., "/BENCHMARK:.*" or "/BENCHMARK:tag-overhead.*").
proc should_run_benchmarks {} {
    if {[llength $::only_tests] == 0} {
        return 0  ;# No --only filter, don't run benchmarks by default
    }
    foreach pattern $::only_tests {
        # Check if pattern contains "BENCHMARK:" (works for both literal and regex patterns)
        if {[string match "*BENCHMARK:*" $pattern]} {
            return 1
        }
    }
    return 0
}

# Helper procedure for running config:restore overhead benchmarks
# Reduces code duplication by encapsulating the common benchmark pattern.
#
# Arguments:
#   name          - Benchmark name prefix (used in test names)
#   title         - Title for output display
#   iterations    - Number of iterations to run
#   test_body     - Code block to execute in each iteration (has access to $i)
#   description   - Description of operations (for output, e.g., "2 CONFIG SET")
#   expected      - Expected return value from test body (default: "OK")
#   extra_note    - Optional extra note to display in output (default: "")
proc run_config_restore_benchmark {name title iterations test_body description {expected "OK"} {extra_note ""}} {
    # Measure WITHOUT config:restore tag
    # Use uplevel to run in caller's context so test_body can access caller's variables
    # Set $i in caller's context before running each test so the test body can access it
    set start [clock milliseconds]
    for {set i 0} {$i < $iterations} {incr i} {
        uplevel 1 [list set i $i]
        uplevel 1 [list test "BENCHMARK:${name}:NoRestore-$i" $test_body $expected {}]
    }
    set end [clock milliseconds]
    set total_without [expr {$end - $start}]

    # Measure WITH config:restore tag
    set start [clock milliseconds]
    for {set i 0} {$i < $iterations} {incr i} {
        uplevel 1 [list set i $i]
        uplevel 1 [list test "BENCHMARK:${name}:WithRestore-$i" $test_body $expected {config:restore}]
    }
    set end [clock milliseconds]
    set total_with [expr {$end - $start}]

    # Calculate and display results
    set avg_without [expr {double($total_without) / $iterations}]
    set avg_with [expr {double($total_with) / $iterations}]
    set overhead [expr {$avg_with - $avg_without}]

    puts "\n========================================="
    puts "$title:"
    puts "========================================="
    puts "Iterations: $iterations"
    puts "Average without tag: [format %.2f $avg_without] ms ($description)"
    puts "Average with tag:    [format %.2f $avg_with] ms ($description + restore)"
    puts "Overhead:            [format %.2f $overhead] ms"
    if {$extra_note ne ""} {
        puts "Note: $extra_note"
    }
    puts "=========================================\n"
}

if {[should_run_benchmarks]} {
    start_server {} {
        test "BENCHMARK:config-get - CONFIG GET * performance" {
            set iterations 100
            set param_count 0

            set start [clock milliseconds]
            for {set i 0} {$i < $iterations} {incr i} {
                set config [r config get *]

                if {$i == 0} {
                    set param_count [expr {[llength $config] / 2}]
                }
            }
            set end [clock milliseconds]
            set total_time [expr {$end - $start}]
            set avg_time [expr {double($total_time) / $iterations}]

            puts "\n========================================="
            puts "CONFIG GET * Benchmark Results:"
            puts "========================================="
            puts "Iterations: $iterations"
            puts "Total config parameters: $param_count"
            puts "Average time per CONFIG GET *: [format %.2f $avg_time] ms"
            puts "Total time for $iterations iterations: $total_time ms"
            puts "=========================================\n"
        }

        test "BENCHMARK:tag-overhead - config:restore tag overhead" {
            # Benchmark: Pure overhead of config:restore tag (empty tests)
            run_config_restore_benchmark "tag-overhead" \
                "Config:Restore Pure Overhead (Empty Tests)" \
                10 \
                {
                    # Empty test body
                } \
                "empty test" \
                {}
        }

        test "BENCHMARK:with-changes - Overhead with actual config changes" {
            # Benchmark: Overhead with actual config changes
            run_config_restore_benchmark "with-changes" \
                "Config:Restore Overhead (With Changes)" \
                10 \
                {
                    # Modify multiple configs with different values each iteration
                    # Note: Avoid maxclients as it depends on OS file descriptor limits
                    set mem [expr {100000000 + ($i * 10000000)}]
                    set timeout [expr {100 + ($i * 10)}]
                    set hz [expr {10 + $i}]
                    r config set maxmemory $mem
                    r config set timeout $timeout
                    r config set hz $hz
                } \
                "3 CONFIG SET"
        }
    }

    # Multi-server performance benchmarks
    start_server {tags {external:skip}} {
        start_server {tags {external:skip}} {
            test "BENCHMARK:2servers - Multi-server 2 servers both modified" {
                set server0 [Rn 0]
                set server1 [Rn 1]

                run_config_restore_benchmark "2servers" \
                    "Multi-Server Benchmark (2 servers, both modified)" \
                    10 \
                    {
                        # Modify configs on both servers with different values each iteration
                        set mem0 [expr {100000000 + ($i * 10000000)}]
                        set mem1 [expr {200000000 + ($i * 10000000)}]
                        $server0 config set maxmemory $mem0
                        $server1 config set maxmemory $mem1
                    } \
                    "2 CONFIG SET"
            }
        }
    }

    start_server {tags {external:skip}} {
        start_server {tags {external:skip}} {
            start_server {tags {external:skip}} {
                test "BENCHMARK:3servers-all - Multi-server 3 servers all modified" {
                    set server0 [Rn 0]
                    set server1 [Rn 1]
                    set server2 [Rn 2]

                    run_config_restore_benchmark "3servers-all" \
                        "Multi-Server Benchmark (3 servers, all modified)" \
                        10 \
                        {
                            # Modify all servers with different values each iteration
                            set mem0 [expr {100000000 + ($i * 10000000)}]
                            set mem1 [expr {200000000 + ($i * 10000000)}]
                            set mem2 [expr {300000000 + ($i * 10000000)}]
                            $server0 config set maxmemory $mem0
                            $server1 config set maxmemory $mem1
                            $server2 config set maxmemory $mem2
                        } \
                        "3 CONFIG SET"
                }

                test "BENCHMARK:3servers-2mod - Multi-server 3 servers 2 modified" {
                    set server0 [Rn 0]
                    set server1 [Rn 1]
                    set server2 [Rn 2]

                    run_config_restore_benchmark "3servers-2mod" \
                        "Multi-Server Benchmark (3 servers, 2 modified)" \
                        10 \
                        {
                            # Modify only 2 servers with different values each iteration
                            set mem0 [expr {100000000 + ($i * 10000000)}]
                            set mem2 [expr {300000000 + ($i * 10000000)}]
                            $server0 config set maxmemory $mem0
                            $server2 config set maxmemory $mem2
                            # server1 not modified
                        } \
                        "2 CONFIG SET"
                }

                test "BENCHMARK:3servers-0mod - Multi-server 3 servers 0 modified" {
                    set server0 [Rn 0]
                    set server1 [Rn 1]
                    set server2 [Rn 2]

                    run_config_restore_benchmark "3servers-0mod" \
                        "Multi-Server Benchmark (3 servers, 0 modified)" \
                        10 \
                        {
                            # Don't modify any servers - just do data operations
                            $server0 set test:key "value"
                            $server1 set test:key "value"
                            $server2 set test:key "value"
                        } \
                        "0 CONFIG SET" \
                        "OK" \
                        "Tests diff-based optimization (no restoration needed)"
                }
            }
        }
    }

    # Cluster benchmark
    start_cluster 1 0 {tags {external:skip}} {
        set R [Rn 0]

        test "BENCHMARK:cluster - Cluster config:restore overhead" {
            run_config_restore_benchmark "cluster" \
                "Cluster Benchmark (1 master)" \
                10 \
                {
                    # Typical cluster test operations with different maxmemory each iteration
                    set mem [expr {90000000 + ($i * 10000000)}]
                    $R config set maxmemory $mem
                    $R set cluster:perf:key:$i "value:$i"
                    $R get cluster:perf:key:$i
                    $R del cluster:perf:key:$i
                } \
                "1 CONFIG SET + data ops" \
                {1}
        }
    }
}