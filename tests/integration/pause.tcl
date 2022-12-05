source tests/support/benchmark.tcl

# If OOM and available pending lazy-free jobs in background then clients are paused on write and
# eviction is suspended
#
# The test will apply 4 basic steps:
# 1. Fill up DB with many small items
# 2. Configure tight maxmemory
# 3. Background: FLUSHALL-ASYNC/UNLINK  (slow background operation by lazyfree thread)
#    Foreground: Fill up quickly DB with many big items by main thread
# 4. Expected OOM. Verify no evictions recorded
#
# Since maxmemory is tightly upper-bound (step #2) to first db construction (step #1), and (at step #3)
# it is expected that release of memory is slower than filling up for the DB, then reach OOM and facing
# eviction of keys. At that point it is expected that clients will be paused on write until either
# used memory be below maxmemory or no more lazyfree jobs in background.
#
# Because the second filling up of DB, at step #3, requires peak-memory below maxmemory, it is
# expected clients will be unpaused only when used_mem is below maxmemory and without any
# evictions.
proc run_slow_bg_release_and_fast_fill {benchmark_cmd1 release_cmd benchmark_cmd2 exp_evict_lessthan {report 0}} {
    r select 0

    # Since resetstat cannot reset mem_peak, let's run first 2nd benchmark
    # Its memory peak must to be lower than used memory of the 1st benchmark (See description above)
    common_bench_setup $benchmark_cmd2
    set cmd2_used [s used_memory]
    set cmd2_peak [s used_memory_peak]

    # Restart and fill up DB, with many small items (cmd1)
    common_bench_setup $benchmark_cmd1
    set cmd1_used [s used_memory]

    # Before continue the test, verify used_mem of first benchmark is bigger than 2nd one memory peak
    assert_morethan $cmd1_used $cmd2_peak "used_mem of 1st benchmark must be bigger than mem_peak of 2nd benchmark"

    # Configure maxmemory to be tightly upper-bound to 1st one benchmark
    set maxmemory [expr $cmd1_used + 1000000]
    r config set maxmemory $maxmemory
    r config set maxmemory-policy allkeys-random

    # reset evicted_keys counter
    r config resetstat

    r {*}$release_cmd

    # Fill up DB with many big items. Expected to run fast
    exec {*}$benchmark_cmd2

    # Expected we won't have any evictions
    assert_lessthan [s evicted_keys] $exp_evict_lessthan

    # Expected no -OOM error
    assert_match {} [s errorstat_OOM]

    # Expected client paused during OOM. In comment to avoid flakiness (left for manual testing)
    # assert_morethan [s total_client_paused_during_oom_time] 0

    if {$report} {
        puts " Test Report -- (Setup: TBD)"
        puts " 1) benchmark command:                          $benchmark_cmd1"
        puts "          used_mem:                             $cmd1_used"
        puts " 2) Configure (tight) max_mem:                  $maxmemory"
        puts " 3) <BG>  Release data command:                 $release_cmd"
        puts "    <FG>  benchmark command:                    $benchmark_cmd2"
        puts "          used_mem (isolated run):              $cmd2_used"
        puts "          used_mem_peak (isolated run):         $cmd2_peak"
        puts "    used_mem_peak:                              [s used_memory_peak]"
        puts " 4) Stat: Total evicted keys:                   [s evicted_keys] (expected <= $exp_evict_lessthan)"
        puts "          Total time clients paused during OOM: [s total_client_paused_during_oom_time]"
    }

    r flushall
    # can reset used_memory_peak only by restart server
    restart_server 0 true false
    # restore defaults
    r config set maxmemory 0
    r config set maxmemory-policy noeviction
}

start_server {tags {"external:skip"}} {

    set master_host [srv 0 host]
    set master_port [srv 0 port]

    test {Test client pause write during OOM and FLUSHALL ASYNC} {
        # Test Report -- (Setup: ThinkPad-T14-Gen-3 20.04.1-Ubuntu)
        # 1) benchmark command:                          src/redis-benchmark -h 127.0.0.1 -p 21111 -t set -n 990000 -r 100000000 -d 1 -P 150
        #          used_mem:                             80326088
        # 2) Configure (tight) max_mem:                  81326088
        # 3) <BG>  Release data command:                 FLUSHALL ASYNC
        #    <FG>  benchmark command:                    src/redis-benchmark -h 127.0.0.1 -p 21111 -t set -n 2200 -r 100000000 -d 20000 -P 20
        #          used_mem (isolated run):              46251984
        #          used_mem_peak (isolated run):         50406568
        #    used_mem_peak:                              83205568
        # 4) Stat: Total evicted keys:                   0 (expected <= 3)
        #          Total time clients paused during OOM: 88
        set benchmark_cmd1 [redisbenchmark $master_host $master_port " -t set -n 990000 -r 100000000 -d 1 -P 150"]
        set benchmark_cmd2 [redisbenchmark $master_host $master_port " -t set -n 2200 -r 100000000 -d 20000 -P 20"]
        run_slow_bg_release_and_fast_fill $benchmark_cmd1 "flushall async" $benchmark_cmd2 3

        # Test Report -- (Setup: ThinkPad-T14-Gen-3 20.04.1-Ubuntu)
        # 1) benchmark command:                          src/redis-benchmark -h 127.0.0.1 -p 21111 -t set -n 990000 -r 100000000 -d 1 -P 150
        #          used_mem:                             80315272
        # 2) Configure (tight) max_mem:                  81315272
        # 3) <BG>  Release data command:                 flushall async
        #    <FG>  benchmark command:                    src/redis-benchmark -h 127.0.0.1 -p 21111 -t set -n 300 -r 100000000 -d 200000 -P 50 -c 5
        #          used_mem (isolated run):              69844016
        #          used_mem_peak (isolated run):         71076592
        #    used_mem_peak:                              82217648
        # 4) Stat: Total evicted keys:                   0 (expected <= 3)
        #          Total time clients paused during OOM: 115
        #set benchmark_cmd1 [redisbenchmark $master_host $master_port " -t set -n 990000 -r 100000000 -d 1 -P 150"]
        #set benchmark_cmd2 [redisbenchmark $master_host $master_port " -t set -n 300 -r 100000000 -d 200000 -P 50 -c 5"]
        #run_slow_bg_release_and_fast_fill $benchmark_cmd1 "flushall async" $benchmark_cmd2 3
    } {OK} {needs:config-maxmemory}

    test {Test client pause write during OOM and lazyfree (UNLINK)} {
        # Test Report -- (Setup: ThinkPad-T14-Gen-3 20.04.1-Ubuntu)
        # 1) benchmark command:                          src/redis-benchmark -h 127.0.0.1 -p 21111 -r 2147483647 -n 1000000 HSET myhash __rand_int__ 1 -P 150
        #          used_mem:                             57368720
        # 2) Configure (tight) max_mem:                  58368720
        # 3) <BG>  Release data command:                 unlink myhash
        #    <FG>  benchmark command:                    src/redis-benchmark -h 127.0.0.1 -p 21111 -r 2147483647 -t set -n 300 -d 100000 -P 20 -c 10
        #          used_mem (isolated run):              35437744
        #          used_mem_peak (isolated run):         36758328
        #    used_mem_peak:                              58571080
        # 4) Stat: Total evicted keys:                   0 (expected <= 3)
        #          Total time clients paused during OOM: 161
        set benchmark_cmd1 [redisbenchmark $master_host $master_port " -r 2147483647 -n 1000000 HSET myhash __rand_int__ 1 -P 150"]
        set benchmark_cmd2 [redisbenchmark $master_host $master_port "  -r 2147483647 -t set -n 300 -d 100000 -P 20 -c 10"]
        run_slow_bg_release_and_fast_fill $benchmark_cmd1 "unlink myhash" $benchmark_cmd2 3

        # Test Report -- (Setup: ThinkPad-T14-Gen-3 20.04.1-Ubuntu)
        # 1) benchmark command:                          src/redis-benchmark -h 127.0.0.1 -p 21111 -r 2147483647 -n 1000000 HSET myhash __rand_int__ 1 -P 150
        #          used_mem:                             57367728
        # 2) Configure (tight) max_mem:                  58367728
        # 3) <BG>  Release data command:                 unlink myhash
        #    <FG>  benchmark command:                    src/redis-benchmark -h 127.0.0.1 -p 21111 -r 2147483647 -t set -n 30 -d 1000000 -P 20 -c 10
        #          used_mem (isolated run):              42952240
        #          used_mem_peak (isolated run):         45223088
        #    used_mem_peak:                              59043856
        # 4) Stat: Total evicted keys:                   0 (expected <= 3)
        #          Total time clients paused during OOM: 84
        #set benchmark_cmd1 [redisbenchmark $master_host $master_port " -r 2147483647 -n 1000000 HSET myhash __rand_int__ 1 -P 150"]
        #set benchmark_cmd2 [redisbenchmark $master_host $master_port "  -r 2147483647 -t set -n 30 -d 1000000 -P 20 -c 10"]
        #run_slow_bg_release_and_fast_fill $benchmark_cmd1 "unlink myhash" $benchmark_cmd2 3
    } {OK} {needs:config-maxmemory}
} 

