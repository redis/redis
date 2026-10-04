source tests/support/benchmark.tcl
source tests/support/cli.tcl

proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

# common code to reset stats, flush the db and run redis-benchmark
proc common_bench_setup {cmd} {
    r config resetstat
    r flushall
    if {[catch { exec {*}$cmd } error]} {
        set first_line [lindex [split $error "\n"] 0]
        puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
        fail "redis-benchmark non zero code. first line: $first_line"
    }
}

# we use this extra asserts on a simple set,get test for features like uri parsing
# and other simple flag related tests
proc default_set_get_checks {} {
    assert_match  {*calls=10,*} [cmdstat set]
    assert_match  {*calls=10,*} [cmdstat get]
    # assert one of the non benchmarked commands is not present
    assert_match  {} [cmdstat lrange]
}

tags {"benchmark network external:skip logreqres:skip"} {
    start_server {} {
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        r select 0

        test {benchmark: set,get} {
            set cmd [redisbenchmark $master_host $master_port "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: connecting using URI set,get} {
            set cmd [redisbenchmarkuri $master_host $master_port "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: connecting using URI with authentication set,get} {
            r config set masterauth pass
            set cmd [redisbenchmarkuriuserpass $master_host $master_port "default" pass "-c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks
        }

        test {benchmark: full test suite} {
            set cmd [redisbenchmark $master_host $master_port "-c 10 -n 100"]
            common_bench_setup $cmd

            # ping total calls are 2*issued commands per test due to PING_INLINE and PING_MBULK
            assert_match  {*calls=200,*} [cmdstat ping]
            assert_match  {*calls=100,*} [cmdstat set]
            assert_match  {*calls=100,*} [cmdstat get]
            assert_match  {*calls=100,*} [cmdstat incr]
            # lpush total calls are 2*issued commands per test due to the lrange tests
            assert_match  {*calls=200,*} [cmdstat lpush]
            assert_match  {*calls=100,*} [cmdstat rpush]
            assert_match  {*calls=100,*} [cmdstat lpop]
            assert_match  {*calls=100,*} [cmdstat rpop]
            assert_match  {*calls=100,*} [cmdstat sadd]
            assert_match  {*calls=100,*} [cmdstat hset]
            assert_match  {*calls=100,*} [cmdstat spop]
            assert_match  {*calls=100,*} [cmdstat zadd]
            assert_match  {*calls=100,*} [cmdstat zpopmin]
            assert_match  {*calls=400,*} [cmdstat lrange]
            assert_match  {*calls=100,*} [cmdstat mset]
            # assert one of the non benchmarked commands is not present
            assert_match {} [cmdstat rpoplpush]
        }

        test {benchmark: multi-thread set,get} {
            set cmd [redisbenchmark $master_host $master_port "--threads 10 -c 5 -n 10 -t set,get"]
            common_bench_setup $cmd
            default_set_get_checks

            # ensure only one key was populated
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        }

        test {benchmark: pipelined full set,get} {
            set cmd [redisbenchmark $master_host $master_port "-P 5 -c 10 -n 10010 -t set,get"]
            common_bench_setup $cmd
            assert_match  {*calls=10010,*} [cmdstat set]
            assert_match  {*calls=10010,*} [cmdstat get]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat lrange]

            # ensure only one key was populated
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        }

        test {benchmark: arbitrary command} {
            set cmd [redisbenchmark $master_host $master_port "-c 5 -n 150 INCRBYFLOAT mykey 10.0"]
            common_bench_setup $cmd
            assert_match  {*calls=150,*} [cmdstat incrbyfloat]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat get]

            # ensure only one key was populated
            assert_match  {1} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        }

        test {benchmark: target request rate} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "-c 8 -n 32 --rate 32 PING"]
            set start [clock milliseconds]
            exec {*}$cmd
            set elapsed [expr {[clock milliseconds] - $start}]

            # An empty bucket needs roughly one second for 32 requests at 32 RPS.
            # Leave room for timer granularity and process startup differences.
            assert_morethan_equal $elapsed 600
            assert_lessthan $elapsed 5000
            assert_match {*calls=32,*} [cmdstat ping]
        }

        test {benchmark: target request rate is shared across threads} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "--threads 4 -c 8 -n 32 --rate 32 PING"]
            set start [clock milliseconds]
            exec {*}$cmd
            set elapsed [expr {[clock milliseconds] - $start}]

            assert_morethan_equal $elapsed 600
            assert_lessthan $elapsed 5000
            assert_match {*calls=32,*} [cmdstat ping]
        }

        test {benchmark: pipeline consumes request rate per command} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "-c 8 -P 4 -n 32 --rate 32 PING"]
            set start [clock milliseconds]
            exec {*}$cmd
            set elapsed [expr {[clock milliseconds] - $start}]

            assert_morethan_equal $elapsed 600
            assert_lessthan $elapsed 5000
            assert_match {*calls=32,*} [cmdstat ping]
        }

        test {benchmark: target request rate with fewer requests than clients} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "-c 8 -n 4 --rate 8 PING"]
            set start [clock milliseconds]
            exec {*}$cmd
            set elapsed [expr {[clock milliseconds] - $start}]

            assert_lessthan $elapsed 5000
            assert_match {*calls=4,*} [cmdstat ping]
        }

        test {benchmark: target request rate without keepalive} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "-k 0 -c 4 -n 8 --rate 16 PING"]
            set start [clock milliseconds]
            exec {*}$cmd 2>@1
            set elapsed [expr {[clock milliseconds] - $start}]

            assert_lessthan $elapsed 5000
            assert_match {*calls=8,*} [cmdstat ping]
        }

        test {benchmark: target request rate resets for each test} {
            r config resetstat
            set cmd [redisbenchmark $master_host $master_port "-c 8 -n 4 --rate 8 -t ping"]
            set start [clock milliseconds]
            exec {*}$cmd
            set elapsed [expr {[clock milliseconds] - $start}]

            # -t ping runs both PING_INLINE and PING_MBULK at the target rate.
            assert_morethan_equal $elapsed 600
            assert_lessthan $elapsed 5000
            assert_match {*calls=8,*} [cmdstat ping]
        }

        test {benchmark: target request rate rejects invalid values} {
            foreach value {0 -1 abc} {
                set cmd [redisbenchmark $master_host $master_port "-n 1 --rate $value PING"]
                if {![catch {exec {*}$cmd 2>@1} error]} {
                    fail "redis-benchmark accepted invalid --rate value '$value'"
                }
                assert_match *rate* [string tolower $error]
            }

            set cmd [redisbenchmark $master_host $master_port "--rate"]
            if {![catch {exec {*}$cmd 2>@1} error]} {
                fail "redis-benchmark accepted --rate without a value"
            }
            assert_match *rate* [string tolower $error]
        }

        test {benchmark: target request rate rejects idle mode} {
            set cmd [redisbenchmark $master_host $master_port "-I --rate 1"]
            if {![catch {exec {*}$cmd 2>@1} error]} {
                fail "redis-benchmark accepted --rate with idle mode"
            }
            assert_match {*--rate cannot be used with idle mode (-I).*} $error
        }

        test {benchmark: keyspace length} {
            set cmd [redisbenchmark $master_host $master_port "-r 50 -t set -n 1000"]
            common_bench_setup $cmd
            assert_match  {*calls=1000,*} [cmdstat set]
            # assert one of the non benchmarked commands is not present
            assert_match  {} [cmdstat get]

            # ensure the keyspace has the desired size
            assert_match  {50} [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        }
        
        test {benchmark: clients idle mode should return error when reached maxclients limit} {
            set cmd [redisbenchmark $master_host $master_port "-c 10 -I"]
            set original_maxclients [lindex [r config get maxclients] 1]
            r config set maxclients 5
            catch { exec {*}$cmd } error
            assert_match "*Error*" $error
            r config set maxclients $original_maxclients
        }

        test {benchmark: read last argument from stdin} {
            set base_cmd [redisbenchmark $master_host $master_port "-x -n 10 set key"]
            set cmd "printf arg | $base_cmd"
            common_bench_setup $cmd
            r get key
        } {arg}

        test {benchmark: no NaN or Inf in latency report with fast requests} {
            # With -n 1 on localhost, totlatency can round to 0 ms. Verify showLatencyReport() handles this gracefully.
            set cmd [redisbenchmark $master_host $master_port "-c 1 -n 1 -t set"]
            set output [exec {*}$cmd 2>@1]
            if {[regexp -nocase {nan|(?:^|[^a-z])inf(?:[^o]|$)} $output]} {
                fail "redis-benchmark output contains NaN or Inf: $output"
            }
        }

        # tls specific tests
        if {$::tls} {
            test {benchmark: specific tls-ciphers} {
                set cmd [redisbenchmark $master_host $master_port "-r 50 -t set -n 1000 --tls-ciphers \"DEFAULT:-AES128-SHA256\""]
                common_bench_setup $cmd
                assert_match  {*calls=1000,*} [cmdstat set]
                # assert one of the non benchmarked commands is not present
                assert_match  {} [cmdstat get]
            }

            test {benchmark: specific tls-groups} {
                r flushall
                r config resetstat
                set cmd [redisbenchmark $master_host $master_port "-r 50 -t set -n 1000 --tls-groups prime256v1"]
                common_bench_setup $cmd
                assert_match  {*calls=1000,*} [cmdstat set]
                # assert one of the non benchmarked commands is not present
                assert_match  {} [cmdstat get]
            }

            test {benchmark: tls connecting using URI with authentication set,get} {
                r config set masterauth pass
                set cmd [redisbenchmarkuriuserpass $master_host $master_port "default" pass "-c 5 -n 10 -t set,get"]
                common_bench_setup $cmd
                default_set_get_checks
            }

            test {benchmark: specific tls-ciphersuites} {
                r flushall
                r config resetstat
                set ciphersuites_supported 1
                set cmd [redisbenchmark $master_host $master_port "-r 50 -t set -n 1000 --tls-ciphersuites \"TLS_AES_128_GCM_SHA256\""]
                if {[catch { exec {*}$cmd } error]} {
                    set first_line [lindex [split $error "\n"] 0]
                    if {[string match "*Invalid option*" $first_line]} {
                        set ciphersuites_supported 0
                        if {$::verbose} {
                            puts "Skipping test, TLSv1.3 not supported."
                        }
                    } else {
                        puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
                        fail "redis-benchmark non zero code. first line: $first_line"
                    }
                }
                if {$ciphersuites_supported} {
                    assert_match  {*calls=1000,*} [cmdstat set]
                    # assert one of the non benchmarked commands is not present
                    assert_match  {} [cmdstat get]
                }
            }
        }
    }
}
