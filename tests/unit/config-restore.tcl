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

    # Clean up pollution from previous test for next tests
    r config set maxmemory $original_maxmemory
    r config set timeout $original_timeout

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

}

# ============================================================================
# SECTION 2: CLUSTER SCENARIO TESTS
# ============================================================================
# Purpose: Verify config:restore works with cluster-style access patterns
# ============================================================================

# Single-node cluster: verify config:restore works with cluster-style access
start_cluster 1 0 {tags {external:skip cluster bigredis}} {
    set R [Rn 0]
    set cluster_original_maxmemory [lindex [$R config get maxmemory] 1]
    set cluster_original_timeout [lindex [$R config get timeout] 1]

    test "Cluster: Config change via \$R WITH restore tag" {
        set new_maxmemory [expr $cluster_original_maxmemory + 100000000]
        set new_timeout [expr $cluster_original_timeout + 150]

        $R config set maxmemory $new_maxmemory
        $R config set timeout $new_timeout

        assert_equal $new_maxmemory [lindex [$R config get maxmemory] 1]
        assert_equal $new_timeout [lindex [$R config get timeout] 1]
    } {} {config:restore}

    test "Cluster: Verify restoration worked" {
        assert_equal $cluster_original_maxmemory [lindex [$R config get maxmemory] 1]
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

################################################################################
# SECTION 4: Failure Handling Validation Tests
################################################################################
#
# PURPOSE:
# These tests INTENTIONALLY FAIL to validate that the config:restore mechanism
# correctly restores server configuration even when tests fail.
# Tests cover the two distinct error paths in the test proc:
# 1. Assertion errors (handled by durable mode logic)
# 2. Non-assertion errors (re-raised after restoration)
#
# HOW TO RUN:
#   ./runtest --single unit/config-restore --only "/EXPECTED_FAILURE.*"
#
# EXPECTED OUTPUT:
# - Expected failure count: 3 tests
# - Expected pass count: 3 verification tests
################################################################################

# Only run failure validation tests when explicitly requested
if {[search_pattern_list "EXPECTED_FAILURE" $::only_tests]} {
    start_server {} {
        set ::baseline_maxmemory [lindex [r config get maxmemory] 1]
        set ::baseline_timeout [lindex [r config get timeout] 1]
        set ::baseline_hz [lindex [r config get hz] 1]

        # Test assertion error path (fail() generates an "assertion:" error)
        test "EXPECTED_FAILURE: fail() procedure" {
            r config set maxmemory [expr {$::baseline_maxmemory + 1000000}]
            fail "Intentional failure to test config:restore"
        } {} {config:restore}

        test "EXPECTED_FAILURE: verify restoration after fail()" {
            assert_equal $::baseline_maxmemory [lindex [r config get maxmemory] 1]
        } {} {}

        # Test non-assertion error path (re-raise branch in test proc).
        # Wrapped in catch{} to prevent the re-raised error from terminating
        # the test client, ensuring the re-raise path is actually executed.
        set caught [catch {
            test "EXPECTED_FAILURE: non-assertion error re-raise path" {
                r config set hz [expr {$::baseline_hz + 5}]
                error "Non-assertion error to test config restoration before re-raise"
            } {} {config:restore}
        } err]

        test "EXPECTED_FAILURE: verify restoration after non-assertion error" {
            assert_equal 1 $caught "Expected catch to return 1 (error caught)"
            assert_equal $::baseline_hz [lindex [r config get hz] 1]
        } {} {}

        # Test multiple config changes before failure
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
#   ./runtest --single unit/config-restore --only "/BENCHMARK:config-get.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:tag-overhead.*"
#   ./runtest --single unit/config-restore --only "/BENCHMARK:with-changes.*"
# ============================================================================

proc should_run_benchmarks {} {
    if {[llength $::only_tests] == 0} {
        return 0
    }
    foreach pattern $::only_tests {
        if {[string match "*BENCHMARK:*" $pattern]} {
            return 1
        }
    }
    return 0
}

# Helper for running config:restore overhead benchmarks.
# Runs test_body N times without and with the config:restore tag, then reports.
proc run_config_restore_benchmark {name title iterations test_body description {expected "OK"}} {
    set start [clock milliseconds]
    for {set i 0} {$i < $iterations} {incr i} {
        uplevel 1 [list set i $i]
        uplevel 1 [list test "BENCHMARK:${name}:NoRestore-$i" $test_body $expected {}]
    }
    set total_without [expr {[clock milliseconds] - $start}]

    set start [clock milliseconds]
    for {set i 0} {$i < $iterations} {incr i} {
        uplevel 1 [list set i $i]
        uplevel 1 [list test "BENCHMARK:${name}:WithRestore-$i" $test_body $expected {config:restore}]
    }
    set total_with [expr {[clock milliseconds] - $start}]

    set avg_without [expr {double($total_without) / $iterations}]
    set avg_with [expr {double($total_with) / $iterations}]
    set overhead [expr {$avg_with - $avg_without}]

    puts "\n========================================="
    puts "$title:"
    puts "  Iterations: $iterations"
    puts "  Without tag: [format %.2f $avg_without] ms ($description)"
    puts "  With tag:    [format %.2f $avg_with] ms ($description + restore)"
    puts "  Overhead:    [format %.2f $overhead] ms"
    puts "=========================================\n"
}

if {[should_run_benchmarks]} {
    start_server {} {
        test "BENCHMARK:config-get - CONFIG GET * performance" {
            set iterations 100
            set start [clock milliseconds]
            for {set i 0} {$i < $iterations} {incr i} {
                set config [r config get *]
            }
            set avg_time [expr {double([clock milliseconds] - $start) / $iterations}]
            set param_count [expr {[llength $config] / 2}]

            puts "\n========================================="
            puts "CONFIG GET * Benchmark:"
            puts "  Params: $param_count, Avg: [format %.2f $avg_time] ms"
            puts "=========================================\n"
        }

        test "BENCHMARK:tag-overhead - config:restore tag overhead" {
            run_config_restore_benchmark "tag-overhead" \
                "Config:Restore Pure Overhead (Empty Tests)" \
                10 {} "empty test" {}
        }

        test "BENCHMARK:with-changes - Overhead with actual config changes" {
            run_config_restore_benchmark "with-changes" \
                "Config:Restore Overhead (With Changes)" \
                10 \
                {
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
}