proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

proc errorstat {cmd} {
    return [errorrstat $cmd r]
}

start_server {tags {"info"}} {
    start_server {} {

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
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat xgroup]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat eval]

            # EVAL command errors should still be pinpointed to him
            catch {r eval a} e
            assert_match {ERR wrong*} $e
            assert_match {*calls=1,*,rejected_calls=1,failed_calls=0} [cmdstat eval]
            assert_match {*count=2*} [errorstat ERR]
            assert_equal [s total_error_replies] 2
        }

        test {errorstats: failed call NOGROUP error} {
            r config resetstat
            assert_match {} [errorstat NOGROUP]
            r del mystream
            r XADD mystream * f v
            catch {r XGROUP CREATECONSUMER mystream mygroup consumer} e
            assert_match {NOGROUP*} $e
            assert_match {*count=1*} [errorstat NOGROUP]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat xgroup]
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
            assert_match {ERR wrong number of arguments*} $e
            catch {r exec} e
            assert_match {EXECABORT*} $e
            assert_match {*count=1*} [errorstat ERR]
            assert_equal [s total_error_replies] 1
            assert_match {*calls=0,*,rejected_calls=1,failed_calls=0} [cmdstat set]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat multi]
            assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat exec]
            assert_equal [s total_error_replies] 1
            r config resetstat
            assert_match {} [errorstat ERR]
        }

        test {errorstats: rejected call due to wrong arity} {
            r config resetstat
            assert_equal [s total_error_replies] 0
            assert_match {} [errorstat ERR]
            catch {r set k} e
            assert_match {ERR wrong number of arguments*} $e
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
        }
    }

    start_server {} {
        test {Unsafe command names are sanitized in INFO output} {
            catch {r host:} e
            set info [r info commandstats]
            assert_match {*cmdstat_host_:calls=1*} $info
        }
    }
}
