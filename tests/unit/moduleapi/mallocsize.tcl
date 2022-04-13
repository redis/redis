set testmodule [file normalize tests/modules/mallocsize.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {MallocSize raw} {
        assert_equal [r mallocsize.raw 7] 8
        assert_equal [r mallocsize.raw 32] 32
        assert_equal [r mallocsize.raw 33] 40
    }
    
    test {MallocSize string} {
        assert_equal [r mallocsize.string 123] 24
        assert_equal [r mallocsize.string 1234567890] 32
        assert_equal [r mallocsize.string 12345678901234567890] 40
    }
    
    test {MallocSize dict} {
        assert_equal [r mallocsize.dict k v] 520
        assert_equal [r mallocsize.dict kkkkkkkkk vvvvvvvvv] 520
        assert_equal [r mallocsize.dict k v k2 v2] 764
        assert_equal [r mallocsize.dict k1 v1 k2 v2] 1008  ;# The former is smaller because it has less rax nodes
    }
}
