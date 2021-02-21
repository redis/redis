set testmodule [file normalize tests/modules/reply.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {RM_ReplyWithInt: an integer reply} {
        assert_equal 42 [r rw.int 42]
    }

    test {RM_ReplyWithDouble: a float reply} {
        assert_equal 3.141 [r rw.double 3.141]
    }

    test {RM_ReplyWithArray: an array reply} {
        assert_equal {0 1 2 3 4} [r rw.array 5]
    }

    test {RM_ReplyWithMap: an map reply} {
        assert_equal {0 0 1 1.5 2 3} [r rw.map 3]
    }

    test {RM_ReplyWithSet: an set reply} {
        assert_equal {0 1 2} [r rw.map 3]
    }

    test {RM_ReplyWithBool: a boolean reply} {
        assert_equal {0 1} [r rw.bool]
    }

    test {RM_ReplyWithNull: a NULL reply} {
        assert_equal {} [r rw.null]
    }
}

