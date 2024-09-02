set testmodule [file normalize tests/modules/scan.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module scan keyspace} {
        # the module create a scan command with filtering which also return values
        r set x 1
        r set y 2
        r set z 3
        r hset h f v
        lsort [r scan.scan_strings]
    } {{x 1} {y 2} {z 3}}

    test {Module scan hash listpack} {
        r hmset hh f1 v1 f2 v2
        assert_encoding listpack hh
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}}

    test {Module scan hash listpack with int value} {
        r hmset hh1 f1 1
        assert_encoding listpack hh1
        lsort [r scan.scan_key hh1]
    } {{f1 1}}

    test {Module scan hash listpack with hexpire} {
        r debug set-active-expire 0
        r hmset hh f1 v1 f2 v2 f3 v3
        r hexpire hh 100000 fields 1 f1
        r hpexpire hh 1 fields 1 f3
        after 10
        assert_range [r httl hh fields 1 f1] 10000 100000
        assert_encoding listpackex hh
        r debug set-active-expire 1
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}} {needs:debug}

    test {Module scan hash dict} {
        r config set hash-max-ziplist-entries 2
        r hmset hh f3 v3
        assert_encoding hashtable hh
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan hash dict with hexpire} {
        r config set hash-max-listpack-entries 1
        r del hh
        r hmset hh f1 v1 f2 v2 f3 v3
        r hexpire hh 100000 fields 1 f2
        r hpexpire hh 5 fields 1 f3
        assert_range [r httl hh fields 1 f2] 10000 100000
        assert_encoding hashtable hh
        after 10
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}}

    test {Module scan hash with hexpire can return no items} {
        r del hh
        r debug set-active-expire 0
        r hmset hh f1 v1 f2 v2 f3 v3
        r hpexpire hh 1 fields 3 f1 f2 f3
        after 10
        assert_equal [r scan.scan_key hh] {}
        r debug set-active-expire 1
    } {OK} {needs:debug}

    test {Module scan zset listpack} {
        r zadd zz 1 f1 2 f2
        assert_encoding listpack zz
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2}}

    test {Module scan zset skiplist} {
        r config set zset-max-ziplist-entries 2
        r zadd zz 3 f3
        assert_encoding skiplist zz
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan set intset} {
        r sadd ss 1 2
        assert_encoding intset ss
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}}}

    test {Module scan set dict} {
        r config set set-max-intset-entries 2
        r sadd ss 3
        assert_encoding hashtable ss
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}} {3 {}}}

    test {Module scan set listpack} {
        r sadd ss1 a b c
        assert_encoding listpack ss1
        lsort [r scan.scan_key ss1]
    } {{a {}} {b {}} {c {}}}

    test "Unload the module - scan" {
        assert_equal {OK} [r module unload scan]
    }
}