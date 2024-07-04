start_cluster 1 0 {tags {external:skip cluster}} {

    test {Eval scripts with shebangs and functions default to no cross slots} {
        # Test that scripts with shebang block cross slot operations
        assert_error "ERR Script attempted to access keys that do not hash to the same slot*" {
            r 0 eval {#!lua
                redis.call('set', 'foo', 'bar')
                redis.call('set', 'bar', 'foo')
                return 'OK'
            } 0}

        # Test the functions by default block cross slot operations
        r 0 function load REPLACE {#!lua name=crossslot
            local function test_cross_slot(keys, args)
                redis.call('set', 'foo', 'bar')
                redis.call('set', 'bar', 'foo')
                return 'OK'
            end

            redis.register_function('test_cross_slot', test_cross_slot)}
        assert_error "ERR Script attempted to access keys that do not hash to the same slot*" {r FCALL test_cross_slot 0}
    }

    test {Cross slot commands are allowed by default for eval scripts and with allow-cross-slot-keys flag} {
        # Old style lua scripts are allowed to access cross slot operations
        r 0 eval "redis.call('set', 'foo', 'bar'); redis.call('set', 'bar', 'foo')" 0

        # scripts with allow-cross-slot-keys flag are allowed
        r 0 eval {#!lua flags=allow-cross-slot-keys
            redis.call('set', 'foo', 'bar'); redis.call('set', 'bar', 'foo')
        } 0

        # Retrieve data from different slot to verify data has been stored in the correct dictionary in cluster-enabled setup
        # during cross-slot operation from the above lua script.
        assert_equal "bar" [r 0 get foo]
        assert_equal "foo" [r 0 get bar]
        r 0 del foo
        r 0 del bar

        # Functions with allow-cross-slot-keys flag are allowed
        r 0 function load REPLACE {#!lua name=crossslot
            local function test_cross_slot(keys, args)
                redis.call('set', 'foo', 'bar')
                redis.call('set', 'bar', 'foo')
                return 'OK'
            end

            redis.register_function{function_name='test_cross_slot', callback=test_cross_slot, flags={ 'allow-cross-slot-keys' }}}
        r FCALL test_cross_slot 0

        # Retrieve data from different slot to verify data has been stored in the correct dictionary in cluster-enabled setup
        # during cross-slot operation from the above lua function.
        assert_equal "bar" [r 0 get foo]
        assert_equal "foo" [r 0 get bar]
    }
    
    test {Cross slot commands are also blocked if they disagree with pre-declared keys} {
        assert_error "ERR Script attempted to access keys that do not hash to the same slot*" {
            r 0 eval {#!lua
                redis.call('set', 'foo', 'bar')
                return 'OK'
            } 1 bar}
    }

    test {Cross slot commands are allowed by default if they disagree with pre-declared keys} {
        r 0 flushall
        r 0 eval "redis.call('set', 'foo', 'bar')" 1 bar

        # Make sure the script writes to the right slot
        assert_equal 1 [r 0 cluster COUNTKEYSINSLOT 12182] ;# foo slot
        assert_equal 0 [r 0 cluster COUNTKEYSINSLOT 5061] ;# bar slot
    }

    test "Function no-cluster flag" {
        R 0 function load {#!lua name=test
            redis.register_function{function_name='f1', callback=function() return 'hello' end, flags={'no-cluster'}}
        }
        catch {R 0 fcall f1 0} e
        assert_match {*Can not run script on cluster, 'no-cluster' flag is set*} $e
    }

    test "Script no-cluster flag" {
        catch {
            R 0 eval {#!lua flags=no-cluster
                return 1
            } 0
        } e
        
        assert_match {*Can not run script on cluster, 'no-cluster' flag is set*} $e
    }
}
