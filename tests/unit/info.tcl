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

        test {errorstats: limit errors will not increase indefinitely} {
            r config resetstat
            for {set j 1} {$j <= 1100} {incr j} {
                assert_error "$j my error message" {
                    r eval {return redis.error_reply(string.format('%s my error message', ARGV[1]))} 0 $j
                }
            }

            assert_equal [count_log_message 0 "Errorstats stopped adding new errors"] 1
            assert_equal [count_log_message 0 "Current errors code list"] 1
            assert_equal "count=1" [errorstat ERRORSTATS_DISABLED]

            # Since we currently have no metrics exposed for server.errors, we use lazyfree
            # to verify that we only have 128 errors.
            wait_for_condition 50 100 {
                [s lazyfreed_objects] eq 128
            } else {
                fail "errorstats resetstat lazyfree error"
            }
        }

        test {stats: eventloop metrics} {
            set info1 [r info stats]
            set cycle1 [getInfoProperty $info1 eventloop_cycles]
            set el_sum1 [getInfoProperty $info1 eventloop_duration_sum]
            set cmd_sum1 [getInfoProperty $info1 eventloop_duration_cmd_sum]
            assert_morethan $cycle1 0
            assert_morethan $el_sum1 0
            assert_morethan $cmd_sum1 0
            after 110 ;# default hz is 10, wait for a cron tick. 
            set info2 [r info stats]
            set cycle2 [getInfoProperty $info2 eventloop_cycles]
            set el_sum2 [getInfoProperty $info2 eventloop_duration_sum]
            set cmd_sum2 [getInfoProperty $info2 eventloop_duration_cmd_sum]
            if {$::verbose} { puts "eventloop metrics cycle1: $cycle1, cycle2: $cycle2" }
            assert_morethan $cycle2 $cycle1
            assert_lessthan $cycle2 [expr $cycle1+10] ;# we expect 2 or 3 cycles here, but allow some tolerance
            if {$::verbose} { puts "eventloop metrics el_sum1: $el_sum1, el_sum2: $el_sum2" }
            assert_morethan $el_sum2 $el_sum1
            assert_lessthan $el_sum2 [expr $el_sum1+30000] ;# we expect roughly 100ms here, but allow some tolerance
            if {$::verbose} { puts "eventloop metrics cmd_sum1: $cmd_sum1, cmd_sum2: $cmd_sum2" }
            assert_morethan $cmd_sum2 $cmd_sum1
            assert_lessthan $cmd_sum2 [expr $cmd_sum1+15000] ;# we expect about tens of ms here, but allow some tolerance
        }

        test {stats: instantaneous metrics} {
            r config resetstat
            set retries 0
            for {set retries 1} {$retries < 4} {incr retries} {
                after 1600 ;# hz is 10, wait for 16 cron tick so that sample array is fulfilled
                set value [s instantaneous_eventloop_cycles_per_sec]
                if {$value > 0} break
            }

            assert_lessthan $retries 4
            if {$::verbose} { puts "instantaneous metrics instantaneous_eventloop_cycles_per_sec: $value" }
            assert_morethan $value 0
            assert_lessthan $value [expr $retries*15] ;# default hz is 10
            set value [s instantaneous_eventloop_duration_usec]
            if {$::verbose} { puts "instantaneous metrics instantaneous_eventloop_duration_usec: $value" }
            assert_morethan $value 0
            assert_lessthan $value [expr $retries*22000] ;# default hz is 10, so duration < 1000 / 10, allow some tolerance
        }

        test {stats: debug metrics} {
            # make sure debug info is hidden
            set info [r info]
            assert_equal [getInfoProperty $info eventloop_duration_aof_sum] {}
            set info_all [r info all]
            assert_equal [getInfoProperty $info_all eventloop_duration_aof_sum] {}

            set info1 [r info debug]

            set aof1 [getInfoProperty $info1 eventloop_duration_aof_sum]
            assert {$aof1 >= 0}
            set cron1 [getInfoProperty $info1 eventloop_duration_cron_sum]
            assert {$cron1 > 0}
            set cycle_max1 [getInfoProperty $info1 eventloop_cmd_per_cycle_max]
            assert {$cycle_max1 > 0}
            set duration_max1 [getInfoProperty $info1 eventloop_duration_max]
            assert {$duration_max1 > 0}

            after 110 ;# hz is 10, wait for a cron tick.
            set info2 [r info debug]

            set aof2 [getInfoProperty $info2 eventloop_duration_aof_sum]
            assert {$aof2 >= $aof1} ;# AOF is disabled, we expect $aof2 == $aof1, but allow some tolerance.
            set cron2 [getInfoProperty $info2 eventloop_duration_cron_sum]
            assert_morethan $cron2 $cron1
            set cycle_max2 [getInfoProperty $info2 eventloop_cmd_per_cycle_max]
            assert {$cycle_max2 >= $cycle_max1}
            set duration_max2 [getInfoProperty $info2 eventloop_duration_max]
            assert {$duration_max2 >= $duration_max1}
        }

        test {stats: client input and output buffer limit disconnections} {
            r config resetstat
            set info [r info stats]
            assert_equal [getInfoProperty $info client_query_buffer_limit_disconnections] {0}
            assert_equal [getInfoProperty $info client_output_buffer_limit_disconnections] {0}
            # set qbuf limit to minimum to test stat
            set org_qbuf_limit [lindex [r config get client-query-buffer-limit] 1]
            r config set client-query-buffer-limit 1048576
            catch {r set key [string repeat a 1048576]}
            set info [r info stats]
            assert_equal [getInfoProperty $info client_query_buffer_limit_disconnections] {1}
            r config set client-query-buffer-limit $org_qbuf_limit
            # set outbuf limit to just 10 to test stat
            set org_outbuf_limit [lindex [r config get client-output-buffer-limit] 1]
            r config set client-output-buffer-limit "normal 10 0 0"
            r set key [string repeat a 100000] ;# to trigger output buffer limit check this needs to be big
            catch {r get key}
            set info [r info stats]
            assert_equal [getInfoProperty $info client_output_buffer_limit_disconnections] {1}
            r config set client-output-buffer-limit $org_outbuf_limit
        } {OK} {logreqres:skip} ;# same as obuf-limits.tcl, skip logreqres

        test {clients: pubsub clients} {
            set info [r info clients]
            assert_equal [getInfoProperty $info pubsub_clients] {0}
            set rd1 [redis_deferring_client]
            set rd2 [redis_deferring_client]
            # basic count
            assert_equal {1} [ssubscribe $rd1 {chan1}]
            assert_equal {1} [subscribe $rd2 {chan2}]
            set info [r info clients]
            assert_equal [getInfoProperty $info pubsub_clients] {2}
            # unsubscribe non existing channel
            assert_equal {1} [unsubscribe $rd2 {non-exist-chan}]
            set info [r info clients]
            assert_equal [getInfoProperty $info pubsub_clients] {2}
            # count change when client unsubscribe all channels
            assert_equal {0} [unsubscribe $rd2 {chan2}]
            set info [r info clients]
            assert_equal [getInfoProperty $info pubsub_clients] {1}
            # non-pubsub clients should not be involved
            assert_equal {0} [unsubscribe $rd2 {non-exist-chan}]
            set info [r info clients]
            assert_equal [getInfoProperty $info pubsub_clients] {1}
            # close all clients
            $rd1 close
            $rd2 close
            wait_for_condition 100 50 {
                [getInfoProperty [r info clients] pubsub_clients] eq {0}
            } else {
                fail "pubsub clients did not clear"
            }
        }

        test {clients: watching clients} {
            set r2 [redis_client]
            assert_equal [s watching_clients] 0
            assert_equal [s total_watched_keys] 0
            assert_match {*watch=0*} [r client info]
            assert_match {*watch=0*} [$r2 client info]
            # count after watch key
            $r2 watch key
            assert_equal [s watching_clients] 1
            assert_equal [s total_watched_keys] 1
            assert_match {*watch=0*} [r client info]
            assert_match {*watch=1*} [$r2 client info]
            # the same client watch the same key has no effect
            $r2 watch key
            assert_equal [s watching_clients] 1
            assert_equal [s total_watched_keys] 1
            assert_match {*watch=0*} [r client info]
            assert_match {*watch=1*} [$r2 client info]
            # different client watch different key
            r watch key2
            assert_equal [s watching_clients] 2
            assert_equal [s total_watched_keys] 2
            assert_match {*watch=1*} [$r2 client info]
            assert_match {*watch=1*} [r client info]
            # count after unwatch
            r unwatch
            assert_equal [s watching_clients] 1
            assert_equal [s total_watched_keys] 1
            assert_match {*watch=0*} [r client info]
            assert_match {*watch=1*} [$r2 client info]
            $r2 unwatch
            assert_equal [s watching_clients] 0
            assert_equal [s total_watched_keys] 0
            assert_match {*watch=0*} [r client info]
            assert_match {*watch=0*} [$r2 client info]

            # count after watch/multi/exec
            $r2 watch key
            assert_equal [s watching_clients] 1
            $r2 multi
            $r2 exec
            assert_equal [s watching_clients] 0
            # count after watch/multi/discard
            $r2 watch key
            assert_equal [s watching_clients] 1
            $r2 multi
            $r2 discard
            assert_equal [s watching_clients] 0
            # discard without multi has no effect
            $r2 watch key
            assert_equal [s watching_clients] 1
            catch {$r2 discard} e
            assert_equal [s watching_clients] 1
            # unwatch without watch has no effect
            r unwatch
            assert_equal [s watching_clients] 1
            # after disconnect, since close may arrive later, or the client may
            # be freed asynchronously, we use a wait_for_condition
            $r2 close
            wait_for_watched_clients_count 0
        }
    }
}

start_server {tags {"info" "external:skip"}} {
    test {memory: database and pubsub overhead and rehashing dict count} {
        r flushall
        set info_mem [r info memory]
        set mem_stats [r memory stats]
        assert_equal [getInfoProperty $info_mem mem_overhead_db_hashtable_rehashing] {0}
        assert_equal [dict get $mem_stats overhead.db.hashtable.lut] {0}
        assert_equal [dict get $mem_stats overhead.db.hashtable.rehashing] {0}
        assert_equal [dict get $mem_stats db.dict.rehashing.count] {0}
        # Initial dict expand is not rehashing
        r set a b
        set info_mem [r info memory]
        set mem_stats [r memory stats]
        assert_equal [getInfoProperty $info_mem mem_overhead_db_hashtable_rehashing] {0}
        assert_range [dict get $mem_stats overhead.db.hashtable.lut] 1 64
        assert_equal [dict get $mem_stats overhead.db.hashtable.rehashing] {0}
        assert_equal [dict get $mem_stats db.dict.rehashing.count] {0}
        # set 4 more keys to trigger rehashing
        # get the info within a transaction to make sure the rehashing is not completed
        r multi 
        r set b c
        r set c d
        r set d e
        r set e f
        r info memory
        r memory stats
        set res [r exec]
        set info_mem [lindex $res 4]
        set mem_stats [lindex $res 5]
        assert_range [getInfoProperty $info_mem mem_overhead_db_hashtable_rehashing] 1 64
        assert_range [dict get $mem_stats overhead.db.hashtable.lut] 1 192
        assert_range [dict get $mem_stats overhead.db.hashtable.rehashing] 1 64
        assert_equal [dict get $mem_stats db.dict.rehashing.count] {1}
    }
}
