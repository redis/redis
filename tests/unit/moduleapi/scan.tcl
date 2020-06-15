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

    test {Module scan hash ziplist} {
        r hmset hh f1 v1 f2 v2
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2}}
    
    test {Module scan hash dict with int value} {
        r hmset hh1 f1 1 
        lsort [r scan.scan_key hh1]
    } {{f1 1}}

    test {Module scan hash dict} {
        r config set hash-max-ziplist-entries 2
        r hmset hh f3 v3
        lsort [r scan.scan_key hh]
    } {{f1 v1} {f2 v2} {f3 v3}}

    test {Module scan zset ziplist} {
        r zadd zz 1 f1 2 f2
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2}}

    test {Module scan zset dict} {
        r config set zset-max-ziplist-entries 2
        r zadd zz 3 f3
        lsort [r scan.scan_key zz]
    } {{f1 1} {f2 2} {f3 3}}

    test {Module scan set intset} {
        r sadd ss 1 2
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}}}

    test {Module scan set dict} {
        r config set set-max-intset-entries 2
        r sadd ss 3
        lsort [r scan.scan_key ss]
    } {{1 {}} {2 {}} {3 {}}}
}
