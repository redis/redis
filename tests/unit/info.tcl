proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

proc errorstat {cmd} {
    return [errorrstat $cmd r]
}

proc latency_percentiles_usec {cmd} {
    return [latencyrstat_percentiles $cmd r]
}

start_server {tags {"info" "external:skip"}} {
    start_server {} {

        test {latencystats: disable/enable} {
            r config resetstat
            r CONFIG SET latency-tracking no
            r set a b
            assert_match {} [latency_percentiles_usec set]
            r CONFIG SET latency-tracking yes
            r set a b
            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec set]
            r config resetstat
            assert_match {} [latency_percentiles_usec set]
        }

        test {latencystats: configure percentiles} {
            r config resetstat
            assert_match {} [latency_percentiles_usec set]
            r CONFIG SET latency-tracking yes
            r SET a b
            r GET a
            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec set]
            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec get]
            r CONFIG SET latency-tracking-info-percentiles "0.0 50.0 100.0"
            assert_match [r config get latency-tracking-info-percentiles] {latency-tracking-info-percentiles {0 50 100}}
            assert_match {*p0=*,p50=*,p100=*} [latency_percentiles_usec set]
            assert_match {*p0=*,p50=*,p100=*} [latency_percentiles_usec get]
            r config resetstat
            assert_match {} [latency_percentiles_usec set]
        }

        test {latencystats: bad configure percentiles} {
            r config resetstat
            set configlatencyline [r config get latency-tracking-info-percentiles]
            catch {r CONFIG SET latency-tracking-info-percentiles "10.0 50.0 a"} e
            assert_match {ERR CONFIG SET failed*} $e
            assert_equal [s total_error_replies] 1
            assert_match [r config get latency-tracking-info-percentiles] $configlatencyline
            catch {r CONFIG SET latency-tracking-info-percentiles "10.0 50.0 101.0"} e
            assert_match {ERR CONFIG SET failed*} $e
            assert_equal [s total_error_replies] 2
            assert_match [r config get latency-tracking-info-percentiles] $configlatencyline
            r config resetstat
            assert_match {} [errorstat ERR]
        }

        test {latencystats: blocking commands} {
            r config resetstat
            r CONFIG SET latency-tracking yes
            r CONFIG SET latency-tracking-info-percentiles "50.0 99.0 99.9"
            set rd [redis_deferring_client]
            r del list1{t}

            $rd blpop list1{t} 0
            wait_for_blocked_client
            r lpush list1{t} a
            assert_equal [$rd read] {list1{t} a}
            $rd blpop list1{t} 0
            wait_for_blocked_client
            r lpush list1{t} b
            assert_equal [$rd read] {list1{t} b}
            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec blpop]
            $rd close
        }

        test {latencystats: subcommands} {
            r config resetstat
            r CONFIG SET latency-tracking yes
            r CONFIG SET latency-tracking-info-percentiles "50.0 99.0 99.9"
            r client id

            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec client\\|id]
            assert_match {*p50=*,p99=*,p99.9=*} [latency_percentiles_usec config\\|set]
        }

        test {latencystats: measure latency} {
            r config resetstat
            r CONFIG SET latency-tracking yes
            r CONFIG SET latency-tracking-info-percentiles "50.0"
            r DEBUG sleep 0.05
            r SET k v
            set latencystatline_debug [latency_percentiles_usec debug]
            set latencystatline_set [latency_percentiles_usec set]
            regexp "p50=(.+\..+)" $latencystatline_debug -> p50_debug
            regexp "p50=(.+\..+)" $latencystatline_set -> p50_set
            assert {$p50_debug >= 50000}
            assert {$p50_set >= 0}
            assert {$p50_debug >= $p50_set}
        } {} {needs:debug}

        test {errorstats: failed call authentication error} {
            r config resetstat
            assert_match {} [errorstat ERR]
            assert_equal [s total_error_replies] 0
            catch {r auth k} e
            assert_match {ERR AUTH*} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat ERR]
        }

        test {errorstats: failed call within MULTI/EXEC} {
            r config resetstat
            assert_match {} [errorstat ERR]
            assert_equal [s total_error_replies] 0
            r multi
            r set a b
            r auth a
            catch {r exec} e
            assert_match {ERR AUTH*} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat set]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat exec]
            assert_equal [s total_error_replies] 1

            # MULTI/EXEC command errors should still be pinpointed to him
            catch {r exec} e
            assert_match {ERR EXEC without MULTI} $e
            assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat exec]
            assert_match {*count=2*} [errorstat ERR]
            assert_equal [s total_error_replies] 2
        }

        test {errorstats: failed call within LUA} {
            r config resetstat
            assert_match {} [errorstat ERR]
            assert_equal [s total_error_replies] 0
            catch {r eval {redis.pcall('XGROUP', 'CREATECONSUMER', 's1', 'mygroup', 'consumer') return } 0} e
            assert_match {*count=1*} [errorstat ERR]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat xgroup\\|createconsumer]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat eval]

            # EVAL command errors should still be pinpointed to him
            catch {r eval a} e
            assert_match {ERR wrong*} $e
            assert_match {*calls=1,*,rejected_calls=1,failed_calls=0} [cmdstat eval]
            assert_match {*count=2*} [errorstat ERR]
            assert_equal [s total_error_replies] 2
        }

        test {errorstats: failed call NOSCRIPT error} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat NOSCRIPT]
            catch {r evalsha NotValidShaSUM 0} e
            assert_match {NOSCRIPT*} $e
            assert_match {*count=1*} [errorstat NOSCRIPT]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat evalsha]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat NOSCRIPT]
        }

        test {errorstats: failed call NOGROUP error} {
            r config resetstat
            assert_match {} [errorstat NOGROUP]
            r del mystream
            r XADD mystream * f v
            catch {r XGROUP CREATECONSUMER mystream mygroup consumer} e
            assert_match {NOGROUP*} $e
            assert_match {*count=1*} [errorstat NOGROUP]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat xgroup\\|createconsumer]
            r config resetstat
            assert_match {} [errorstat NOGROUP]
        }

        test {errorstats: rejected call unknown command} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat ERR]
            catch {r asdf} e
            assert_match {ERR unknown*} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat ERR]
        }

        test {errorstats: rejected call within MULTI/EXEC} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat ERR]
            r multi
            catch {r set} e
            assert_match {ERR wrong number of arguments for 'set' command} $e
            catch {r exec} e
            assert_match {EXECABORT*} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_match {*count=1*} [errorstat EXECABORT]
            assert_equal [s total_error_replies] 2
            assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat multi]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat exec]
            assert_equal [s total_error_replies] 2
            r config resetstat
            assert_match {} [errorstat ERR]
        }

        test {errorstats: rejected call due to wrong arity} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat ERR]
            catch {r set k} e
            assert_match {ERR wrong number of arguments for 'set' command} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            # ensure that after a rejected command, valid ones are counted properly
            r set k1 v1
            r set k2 v2
            assert_match {calls=2,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            assert_equal [s total_error_replies] 1
        }

        test {errorstats: rejected call by OOM error} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat OOM]
            r config set maxmemory 1
            catch {r set a b} e
            assert_match {OOM*} $e
            assert_match {*count=1*} [errorstat OOM]
            assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat OOM]
            r config set maxmemory 0
        }

        test {errorstats: rejected call by authorization error} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat NOPERM]
            r ACL SETUSER alice on >p1pp0 ~cached:* +get +info +config
            r auth alice p1pp0
            catch {r set a b} e
            assert_match {NOPERM*} $e
            assert_match {*count=1*} [errorstat NOPERM]
            assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat NOPERM]
            r auth default ""
        }

        test {errorstats: blocking commands} {
            r config resetstat
            set rd [redis_deferring_client]
            $rd client id
            set rd_id [$rd read]
            r del list1{t}

            $rd blpop list1{t} 0
            wait_for_blocked_client
            r client unblock $rd_id error
            assert_error {UNBLOCKED*} {$rd read}
            assert_match {*count=1*} [errorstat UNBLOCKED]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat blpop]
            assert_equal [s total_error_replies] 1
            $rd close
        }

    }
}
