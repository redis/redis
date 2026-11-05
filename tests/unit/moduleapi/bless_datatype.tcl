set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules" "bless" "external:skip"}} {
    r module load $testmodule

    test {BLESS: NO-EVICT survives RM_ModuleTypeSetValue on an existing key} {
        r flushall
        r datatype.set k 1 a
        assert_equal 1 [r bless set k no-evict]
        r datatype.set k 2 b
        assert_equal {2 b} [r datatype.get k]
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal {k} [lindex [r bless scan 0 no-evict] 1]
        assert_equal 1 [s blessed_keys]
    }

    test {BLESS: blessed module key is not evicted after its value is replaced} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-lru
        r datatype.set precious 1 a
        r bless set precious no-evict
        r datatype.set precious 2 b
        set used [s used_memory]
        r config set maxmemory [expr {$used + 1000000}]
        for {set j 0} {$j < 4000} {incr j} {
            r set flood:$j [string repeat y 2000]
        }
        r config set maxmemory 0
        assert {[s evicted_keys] > 0}
        assert_equal 1 [r exists precious]
    }
}
