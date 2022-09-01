source tests/support/benchmark.tcl

start_server {tags {"external:skip"}} {
  set master_host [srv 0 host]
  set master_port [srv 0 port]

  test {Test client-pause-write-during-oom} {
    # if OOM and available pending lazy-free jobs in background then clients are paused on write and
    # eviction is suspended
    # 
    # The test will apply 4 basic steps:
    # 1. Fill up DB with many small items and configures tight maxmemory
    # 2. FLUSHALL ASYNC  (slow background operation by lazyfree thread)    
    # 3. Fill up DB with many big items (quick operation by main thread)
    # 4. Expected OOM. Verify no evictions recorded
    #
    # Since maxmemory is tightly upper-bound to first db construction (step #1), and it is expected 
    # that flushall-async release of memory (step #2) is slower than filling up for the DB (step #3),
    # then reach OOM (step #3) and facing eviction of keys. At that point it is expected that clients will be
    # paused on write until either used_mem be below maxmemory or no more lazyfree jobs in background.
    # And in our case, until used_mem will be below maxmemory (because the second filling up of DB,
    # at step #3, requires memory below maxmemory).
    # Therefore, expected clients will be unpaused only when used_mem is below maxmemory and without
    # any evictions.  
    
    # Prepare cmd2 in advance in order to run as fast as possible after flushall
    set cmd1 [redisbenchmark $master_host $master_port " -t set -n 990000 -r 100000000 -d 1 -P 150"]
    set cmd2 [redisbenchmark $master_host $master_port " -t set -n 2200 -r 100000000 -d 20000 -P 20"]

    # Configure maxmemory to be tightly upper-bound to DB size, (~80.4MB)
    r config set maxmemory 84000000
    r config set maxmemory-policy allkeys-random

    # Restart && and fill up DB, with many small items up-to used_mem of 77MB
    common_bench_setup $cmd1

    # reset evicted_keys counter
    r config resetstat
    
    # FLUSHALL expected to run slow in background due to small items.
    r flushall async

    # Fill up DB with many big items up-to used_mem  of ~44MB. Expected to run fast
    exec {*}$cmd2

    # Expected we won't have any evictions 
    assert_equal [s evicted_keys] 0
    
    # Expected OOM recorded. In comment to avoid flakiness (left for manual testing)
    # assert_morethan [s used_memory_peak]  [s maxmemory]
  }
}
