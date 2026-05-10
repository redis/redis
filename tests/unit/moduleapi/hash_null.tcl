set testmodule [file normalize tests/modules/hash_null.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test "Hash module API get with valid field" {
        r HSET myhash f1 v1 f2 v2 f3 v3
        assert_equal "v1" [r hashnull.normalget myhash f1]
        assert_equal "v2" [r hashnull.normalget myhash f2]
    }

    test "Hash module API get nonexistent field returns null" {
        assert_equal {} [r hashnull.normalget myhash nonexistent]
    }

    test "Hash module API get with long long field" {
        r HSET myhash 1 one 2 two
        assert_equal "one" [r hashnull.getfromll myhash 1]
        assert_equal "two" [r hashnull.getfromll myhash 2]
    }

    test "Hash module API get with double field" {
        r HSET myhash 1.5 onefive 2.5 twofive
        assert_equal "onefive" [r hashnull.getfromdouble myhash 1.5]
    }
}
