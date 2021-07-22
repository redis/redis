set testmodule [file normalize tests/modules/reply.so]

# test all with hello 2/3

start_server {tags {"modules"}} {
    r module load $testmodule
    
    for {proto=2; proto<=3; incr proto} {
        r hello $proto

        test {RM_ReplyWithString: an string reply} {
            # RedisString
            set string [r rw.string "Redis"]
            assert_equal "Redis" $string
            # C string
            set string [r rw.cstring]
            assert_equal "A simple string" $string
        }

        test {RM_ReplyWithInt: an integer reply} {
            assert_equal 42 [r rw.int 42]
        }

        test {RM_ReplyWithDouble: a float reply} {
            assert_equal 3.141 [r rw.double 3.141]
        }

        test {RM_ReplyWithLongDouble: a float reply} {
            assert_equal 3.141 [r rw.longdouble 3.141]
        }

        test {RM_ReplyWithVerbatimString: a string reply} {
            assert_equal "bla\nbla\nbla" [r rw.verbatim "bla\nbla\nbla"]
        }

        test {RM_ReplyWithArray: an array reply} {
            assert_equal {0 1 2 3 4} [r rw.array 5]
        }

        test {RM_ReplyWithMap: an map reply} {
            set res [r rw.map 3]
            if {$proto == 2} {
                assert_equal {0 0.0 1 1.5 2 3.0} $res
            } else {
                assert_equal [dict create 0 0.0 1 1.5 2 3.0] $res
            }
        }

        test {RM_ReplyWithSet: an set reply} {
            assert_equal {0 1 2} [r rw.set 3]
        }

        test {RM_ReplyWithAttribute: an set reply} {
            set res [r rw.attribute 3]
            if {$proto == 2} {
                catch {r rw.error} e
                assert_match "Attributes aren't supported by RESP 2" $e
            } else {
                assert_equal [dict create 0 0.0 1 1.5 2 3.0] $res
            }
            assert_equal "OK" $res
        }

        test {RM_ReplyWithBool: a boolean reply} {
            assert_equal {0 1} [r rw.bool]
        }

        test {RM_ReplyWithNull: a NULL reply} {
            assert_equal {} [r rw.null]
        }

        test {RM_ReplyWithError: an error reply} {
            catch {r rw.error} e
            assert_match "An error" $e
        }
    }
}
