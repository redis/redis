set testmodule [file normalize tests/modules/mallocsize.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {MallocSize of raw bytes} {
        assert_equal [r mallocsize.setraw key 40] {OK}
        assert_morethan [memory_usage key] 0
    }
    
    test {MallocSize of string} {
        assert_equal [r mallocsize.setstr key abcdefg] {OK}
        assert_morethan [memory_usage key] 0
    }
    
    test {MallocSize of dict} {
        assert_equal [r mallocsize.setdict key f1 v1 f2 v2] {OK}
        assert_morethan [memory_usage key] 0
    }
}
