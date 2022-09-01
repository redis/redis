source tests/support/benchmark.tcl

start_server {tags {"external:skip"}} {
  set master_host [srv 0 host]
  set master_port [srv 0 port]

  test {Test client-pause-write-during-oom} {
    # Scenario:
    # When client-pause-write-during-oom is configured, if OOM and available pending 
    # lazy-free jobs in background, then (clients are paused and) eviction is 
    # suspended. The test will:
    # - Fill up DB with many small items and configures tight maxmemory
    # - FLUSHALL ASYNC  (slow background operation by lazyfree thread)    
    # - Fill up DB with many big items (quick operation by main thread)
    #  
    # It is expected that filling up DB for the second time will be quicker than the
    # FLUSHALL ASYNC release and will cause to OOM, and if client-pause-write-during-oom
    # is configured then there won't be any eviction or only few 

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

        # reset evicted_keys counter
        r config resetstat
        
        # FLUSHALL expected to run slow in background due to small items.
        r flushall async

        # Fill up DB with many big items up-to used_mem  of ~44MB. Expected to run fast
        exec {*}$cmd2

        set evicted($configVal) [s evicted_keys]
    }

    # Expected that when feature is enabled we won't have any evictions. In an
    # attempt to avoid from flaky test, we will assert that there are at least 
    # four times more evictions when feature is disabled  
    assert_lessthan_equal [expr 4 * $evicted($FEATURE_ENABLED)] $evicted($FEATURE_DISABLED)
        
  }
}
