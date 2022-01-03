set testmodule [file normalize tests/modules/datatype2.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "datatype2: test mem alloc and free" {
        r flushall

        r select 0
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal 2 [r mem.alloc k2 2]

        r select 1
        assert_equal 1 [r mem.alloc k1 1]
        assert_equal 5 [r mem.alloc k2 5]

        r select 0 
        assert_equal 1 [r mem.free k1]
        assert_equal 1 [r mem.free k2]

        r select 1
        assert_equal 1 [r mem.free k1]
        assert_equal 1 [r mem.free k2]
    }

    test "datatype2: test del and unlink" {
        r flushall

        assert_equal 100 [r mem.alloc k1 100]
        assert_equal 60 [r mem.alloc k2 60]

        assert_equal 1 [r unlink k1]
        assert_equal 1 [r del k2]
    }

    test "datatype2: test read and write" {
        r flushall

        assert_equal 3 [r mem.alloc k1 3]
        
        set data datatype2
        assert_equal [string length $data] [r mem.write k1 0 $data]
        assert_equal $data [r mem.read k1 0]
    }

    test "datatype2: test rdb save and load" {
        r flushall

        r select 0
        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]

        set data k2
        assert_equal 2 [r mem.alloc k2 2]
        assert_equal [string length $data] [r mem.write k2 0 $data]

        r select 1
        set data k3
        assert_equal 3 [r mem.alloc k3 3]
        assert_equal [string length $data] [r mem.write k3 1 $data]

        set data k4
        assert_equal 2 [r mem.alloc k4 2]
        assert_equal [string length $data] [r mem.write k4 0 $data]

        r bgsave
        waitForBgsave r
        r debug reload
 
        r select 0
        assert_equal k1 [r mem.read k1 1]
        assert_equal k2 [r mem.read k2 0]

        r select 1
        assert_equal k3 [r mem.read k3 1]
        assert_equal k4 [r mem.read k4 0]
    }

    test "datatype2: test aof rewrite" {
        r flushall

        r select 0
        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]

        set data k2
        assert_equal 2 [r mem.alloc k2 2]
        assert_equal [string length $data] [r mem.write k2 0 $data]

        r select 1
        set data k3
        assert_equal 3 [r mem.alloc k3 3]
        assert_equal [string length $data] [r mem.write k3 1 $data]

        set data k4
        assert_equal 2 [r mem.alloc k4 2]
        assert_equal [string length $data] [r mem.write k4 0 $data]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
 
        r select 0
        assert_equal k1 [r mem.read k1 1]
        assert_equal k2 [r mem.read k2 0]

        r select 1
        assert_equal k3 [r mem.read k3 1]
        assert_equal k4 [r mem.read k4 0]
    }

    test "datatype2: test copy" {
        r flushall

        r select 0
        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]
        assert_equal $data [r mem.read k1 1]

        set data k2
        assert_equal 2 [r mem.alloc k2 2]
        assert_equal [string length $data] [r mem.write k2 0 $data]
        assert_equal $data [r mem.read k2 0]

        r select 1
        set data k3
        assert_equal 3 [r mem.alloc k3 3]
        assert_equal [string length $data] [r mem.write k3 1 $data]

        set data k4
        assert_equal 2 [r mem.alloc k4 2]
        assert_equal [string length $data] [r mem.write k4 0 $data]

        assert_equal {total 5 used 2} [r mem.usage 0]
        assert_equal {total 5 used 2} [r mem.usage 1]

        r select 0
        assert_equal 1 [r copy k1 k3]
        assert_equal k1 [r mem.read k3 1]
        assert_equal {total 8 used 3} [r mem.usage 0]
        assert_equal 1 [r copy k2 k1 db 1]

        r select 1
        assert_equal k2 [r mem.read k1 0]
        assert_equal {total 8 used 3} [r mem.usage 0]
        assert_equal {total 7 used 3} [r mem.usage 1]
    }

    test "datatype2: test swapdb" {
        r flushall

        r select 0
        set data k1
        assert_equal 5 [r mem.alloc k1 5]
        assert_equal [string length $data] [r mem.write k1 1 $data]
        assert_equal $data [r mem.read k1 1]

        set data k2
        assert_equal 4 [r mem.alloc k2 4]
        assert_equal [string length $data] [r mem.write k2 0 $data]
        assert_equal $data [r mem.read k2 0]

        r select 1
        set data k1
        assert_equal 3 [r mem.alloc k3 3]
        assert_equal [string length $data] [r mem.write k3 1 $data]

        set data k2
        assert_equal 2 [r mem.alloc k4 2]
        assert_equal [string length $data] [r mem.write k4 0 $data]

        assert_equal {total 9 used 2} [r mem.usage 0]
        assert_equal {total 5 used 2} [r mem.usage 1]

        assert_equal OK [r swapdb 0 1]
        assert_equal {total 9 used 2} [r mem.usage 1]
        assert_equal {total 5 used 2} [r mem.usage 0]
    }

    test "datatype2: test digest" {
        r flushall

        r select 0
        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]
        assert_equal $data [r mem.read k1 1]

        set data k2
        assert_equal 2 [r mem.alloc k2 2]
        assert_equal [string length $data] [r mem.write k2 0 $data]
        assert_equal $data [r mem.read k2 0]

        r select 1
        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]
        assert_equal $data [r mem.read k1 1]

        set data k2
        assert_equal 2 [r mem.alloc k2 2]
        assert_equal [string length $data] [r mem.write k2 0 $data]
        assert_equal $data [r mem.read k2 0]

        r select 0
        set digest0 [debug_digest]

        r select 1
        set digest1 [debug_digest]

        assert_equal $digest0 $digest1
    }

    test "datatype2: test memusage" {
        r flushall

        set data k1
        assert_equal 3 [r mem.alloc k1 3]
        assert_equal [string length $data] [r mem.write k1 1 $data]
        assert_equal $data [r mem.read k1 1]

        set data k2
        assert_equal 3 [r mem.alloc k2 3]
        assert_equal [string length $data] [r mem.write k2 0 $data]
        assert_equal $data [r mem.read k2 0]

        assert_equal [memory_usage k1] [memory_usage k2] 
    }
}