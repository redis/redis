source tests/support/benchmark.tcl

start_server {tags {"external:skip"}} {
  # if OOM and available pending lazy-free jobs in background then clients are paused on write and
  # eviction is suspended
  # 
  # The test will apply 4 basic steps:
  # 1. Fill up DB with many small items and configures tight maxmemory
  # 2. FLUSHALL-ASYNC/UNLINK  (slow background operation by lazyfree thread)    
  # 3. Fill up quickly DB with many big items by main thread
  # 4. Expected OOM. Verify no evictions recorded
  #
  # Since maxmemory is tightly upper-bound to first db construction (step #1), and it is expected 
  # that release of memory (step #2) is slower than filling up for the DB (step #3), then reach OOM
  # (step #3) and facing eviction of keys. At that point it is expected that clients will be paused
  # on write until either used-memory be below maxmemory or no more lazyfree jobs in background. And
  # in our case, until used_mem will be below maxmemory.
  #
  # Because the second filling up of DB, at step #3, requires memory below maxmemory, it is expected
  # clients will be unpaused only when used_mem is below maxmemory and without any evictions.  
    
  set master_host [srv 0 host]
  set master_port [srv 0 port]

  test {Test client pause write during OOM and FLUSHALL ASYNC} {    
    # measure cmd1 run-alone : used_memory_human:76.57M, used_memory_peak_human:77.62M, 
    set cmd1 [redisbenchmark $master_host $master_port " -t set -n 990000 -r 100000000 -d 1 -P 150"]
    
    # measure cmd2 run-alone : used_memory_human:44.07M, used_memory_peak_human:48.01M
    # Prepare cmd2 in advance in order to run as fast as possible after flushall
    set cmd2 [redisbenchmark $master_host $master_port " -t set -n 2200 -r 100000000 -d 20000 -P 20"]

    # Restart and fill up DB, with many small items (cmd1)
    common_bench_setup $cmd1

    # Configure maxmemory to be tightly upper-bound to DB size
    r config set maxmemory [expr [s used_memory] + 1000000]
    r config set maxmemory-policy allkeys-random

    # reset evicted_keys counter
    r config resetstat
    
    # FLUSHALL expected to run slow in background due to small items.
    r flushall async

    # Fill up DB with many big items. Expected to run fast
    exec {*}$cmd2

    # Expected we won't have any evictions 
    assert_equal [s evicted_keys] 0
    
    # Expected no -OOM error
    assert_match {} [s errorstat_OOM]
    
    # Expected OOM recorded. In comment to avoid flakiness (left for manual testing)
    # assert_morethan [s used_memory_peak]  [s maxmemory]
  }
  
  test {Test client pause write during OOM and UNLINK} {
    # like previous test but this time with Unlink command. 
    # Without this feature, the test will get failed    
    
    #r select 0
    
    # Prepare cmd2 in advance in order to run as fast as possible after flushall
    # measure cmd1 run-alone : used_memory_human:54.70M, used_memory_peak_human:55.75M
    set cmd1 [redisbenchmark $master_host $master_port " -r 2147483647 -n 1000000 HSET myhash __rand_int__ 1 -P 150"]
    # measure cmd2 run-alone : used_memory_human:20.96M, used_memory_peak_human:41.71M
    set cmd2 [redisbenchmark $master_host $master_port "  -r 2147483647 -t set -n 20 -d 1000000 -P 1 -c 10"]

    # Restart and fill up DB, with many small items
    common_bench_setup $cmd1
    
    # Configure maxmemory to be tightly upper-bound to DB size
    r config set maxmemory [expr [s used_memory] + 100000] 
    r config set maxmemory-policy allkeys-random
    
    # reset evicted_keys counter
    r config resetstat
    
    # UNLINK expected to run slow in background due to small items.    
    r unlink myhash
    
    # Fill up DB fast with many big items
    exec {*}$cmd2

    # Expected we won't have any evictions 
    assert_equal [s evicted_keys] 0
    
    # Expected no -OOM error
    assert_match {} [s errorstat_OOM]
    
    # Expected OOM recorded. In comment to avoid flakiness (left for manual testing)
    # assert_morethan [s used_memory_peak]  [s maxmemory]
  }
}
