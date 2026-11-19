source tests/support/benchmark.tcl

# Place all 16384 slots on master 0 so master 1 stays a slot-less master
# while the cluster as a whole still reaches "ok" state. This is exactly
# the input shape that drives fetchClusterConfiguration() through the
# slots_count == 0 branch in src/redis-benchmark.c.
proc all_slots_to_first_master {masters} {
    R 0 cluster addslotsrange 0 16383
}

start_cluster 2 0 {tags {external:skip cluster}} {
    test "redis-benchmark --cluster: skipping a no-slots master does not leak" {
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        # -n 1 -t set keeps the run short. We do not assert on the benchmark
        # exit code; the only thing under test is fetchClusterConfiguration()'s
        # handling of a master entry whose slot list is empty.
        set cmd [redisbenchmark $master_host $master_port "--cluster -c 1 -n 1 -t set"]
        set output ""
        catch { exec {*}$cmd 2>@1 } output
        # The warning proves the slots_count == 0 branch executed against the
        # second master, so the test is genuinely exercising the fixed path.
        assert_match {*has no slots, skipping*} $output
        # Under the address sanitizer build, an unfreed allocation in that
        # branch would surface as a leak report on stderr. Outside sanitizer
        # builds the regex never matches, so the assertion is a no-op there.
        if {[regexp -nocase {sanitizer.*(leak|error)|detected memory leak} $output]} {
            fail "redis-benchmark leaked memory on no-slots master path: $output"
        }
    }
} all_slots_to_first_master
