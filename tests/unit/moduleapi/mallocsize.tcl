set testmodule [file normalize tests/modules/mallocsize.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {MallocSize of raw, string and dict} {
        assert_equal [r mallocsize.set key 12 abc k1 v1 k2 v2] {OK}
        assert_equal [r memory usage key] 168
    }
}
