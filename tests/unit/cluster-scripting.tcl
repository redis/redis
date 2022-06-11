# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

start_server {overrides {cluster-enabled yes} tags {external:skip cluster}} {
    r 0 cluster addslotsrange 0 16383
    wait_for_condition 50 100 {
        [csi 0 cluster_state] eq "ok"
    } else {
        fail "Cluster never became 'ok'"
    }

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

        # Functions with allow-cross-slot-keys flag are allowed
        r 0 function load REPLACE {#!lua name=crossslot
            local function test_cross_slot(keys, args)
                redis.call('set', 'foo', 'bar')
                redis.call('set', 'bar', 'foo')
                return 'OK'
            end

            redis.register_function{function_name='test_cross_slot', callback=test_cross_slot, flags={ 'allow-cross-slot-keys' }}}
        r FCALL test_cross_slot 0
    }
    
    test {Cross slot commands are also blocked if they disagree with pre-declared keys} {
        assert_error "ERR Script attempted to access keys that do not hash to the same slot*" {
            r 0 eval {#!lua
                redis.call('set', 'foo', 'bar')
                return 'OK'
            } 1 bar}
    }
}

set ::singledb $old_singledb
