source tests/support/benchmark.tcl

start_server {} {
  set master_host [srv 0 host]
  set master_port [srv 0 port]

  test {Test client-pause-write-during-oom} {
    # Scenario:
    # This test fills up DB with many small items, configures tight maxmemory,
    # and simulates FLUSHALL ASYNC that run slowly in the background due to small
    # items. Whereas the next commands to follow, reconstruct DB quickly with many
    # big items to store. It is expected that when client-pause-write-during-oom is 
    # is configured won't be eviction or only few, unlike when when feature is 
    # disabled 

    set FEATURE_DISABLED no
    set FEATURE_ENABLED yes

    # Prepare cmd2 in advance in order to run as fast as possible after flushall
    set cmd1 [redisbenchmark $master_host $master_port " -t set -n 990000 -r 100000000 -d 1 -P 150"]
    set cmd2 [redisbenchmark $master_host $master_port " -t set -n 2200 -r 100000000 -d 20000 -P 20"]

    foreach configVal [list $FEATURE_DISABLED $FEATURE_ENABLED] {
        # Configure maxmemory to be tightly upper-bound to DB size, (~80.4MB)
        r config set maxmemory 84000000
        r config set client-pause-write-during-oom $configVal
        r config set maxmemory-policy allkeys-random

        # Restart && and fill up DB, with many small items up-to used_mem of 77MB
        common_bench_setup $cmd1

        # FLUSHALL expected to run slow in background due to small items.
        r flushall async

        # Fill up DB with many big items up-to used_mem  of ~44MB. Expected to run fast
        exec {*}$cmd2

        set evicted($configVal) [s evicted_keys]
    }

    # In an attempt to avoid from flaky test, we shall apply the following logic:
    # If: there are only few evictions when feature is disabled 
    # then: expected to see no eviction at all when feature is enabled
    # else: expected at least four times more evictions when feature is disabled  
    if { $evicted($FEATURE_DISABLED) < 12 } {
        assert_equal $evicted($FEATURE_ENABLED) 0
    } else {    
        assert_lessthan [expr 4 * $evicted($FEATURE_ENABLED)] $evicted($FEATURE_DISABLED)
    }
    
  }
}
