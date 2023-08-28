set testmodule [file normalize tests/modules/mallocsize.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {MallocSize of raw bytes} {
        assert_equal [r mallocsize.setraw key 40] {OK}
        assert_morethan [r memory usage key] 40
    }
    
    test {MallocSize of string} {
        assert_equal [r mallocsize.setstr key abcdefg] {OK}
        assert_morethan [r memory usage key] 7 ;# Length of "abcdefg"
    }
    
    test {MallocSize of dict} {
        assert_equal [r mallocsize.setdict key f1 v1 f2 v2] {OK}
        assert_morethan [r memory usage key] 8 ;# Length of "f1v1f2v2"
    }
}
