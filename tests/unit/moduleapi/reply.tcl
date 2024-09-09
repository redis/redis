set testmodule [file normalize tests/modules/reply.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    
    #   test all with hello 2/3
    for {set proto 2} {$proto <= 3} {incr proto} {
        if {[lsearch $::denytags "resp3"] >= 0} {
            if {$proto == 3} {continue}
        } elseif {$::force_resp3} {
            if {$proto == 2} {continue}
        }
        r hello $proto

        test "RESP$proto: RM_ReplyWithString: an string reply" {
            # RedisString
            set string [r rw.string "Redis"]
            assert_equal "Redis" $string
            # C string
            set string [r rw.cstring]
            assert_equal "A simple string" $string
        }

        test "RESP$proto: RM_ReplyWithBigNumber: an string reply" {
            assert_equal "123456778901234567890" [r rw.bignumber "123456778901234567890"]
        }

        test "RESP$proto: RM_ReplyWithInt: an integer reply" {
            assert_equal 42 [r rw.int 42]
        }

        test "RESP$proto: RM_ReplyWithDouble: a float reply" {
            assert_equal 3.141 [r rw.double 3.141]
        }

        test "RESP$proto: RM_ReplyWithDouble: inf" {
            if {$proto == 2} {
                assert_equal "inf" [r rw.double inf]
                assert_equal "-inf" [r rw.double -inf]
            } else {
                # TCL convert inf to different results on different platforms, e.g. inf on mac
                # and Inf on others, so use readraw to verify the protocol
                r readraw 1
                assert_equal ",inf" [r rw.double inf]
                assert_equal ",-inf" [r rw.double -inf]
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithDouble: NaN" {
            if {$proto == 2} {
                assert_equal "nan" [r rw.double 0 0]
                assert_equal "nan" [r rw.double]
            } else {
                # TCL won't convert nan into a double, use readraw to verify the protocol
                r readraw 1
                assert_equal ",nan" [r rw.double 0 0]
                assert_equal ",nan" [r rw.double]
                r readraw 0
            }
        }

        set ld 0.00000000000000001
        test "RESP$proto: RM_ReplyWithLongDouble: a float reply" {
            if {$proto == 2} {
                # here the response gets to TCL as a string
                assert_equal $ld [r rw.longdouble $ld]
            } else {
                # TCL doesn't support long double and the test infra converts it to a
                # normal double which causes precision loss. so we use readraw instead
                r readraw 1
                assert_equal ",$ld" [r rw.longdouble $ld]
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithVerbatimString: a string reply" {
            assert_equal "bla\nbla\nbla" [r rw.verbatim "bla\nbla\nbla"]
        }

        test "RESP$proto: RM_ReplyWithArray: an array reply" {
            assert_equal {0 1 2 3 4} [r rw.array 5]
        }

        test "RESP$proto: RM_ReplyWithMap: an map reply" {
            set res [r rw.map 3]
            if {$proto == 2} {
                assert_equal {0 0 1 1.5 2 3} $res
            } else {
                assert_equal [dict create 0 0.0 1 1.5 2 3.0] $res
            }
        }

        test "RESP$proto: RM_ReplyWithSet: an set reply" {
            assert_equal {0 1 2} [r rw.set 3]
        }

        test "RESP$proto: RM_ReplyWithAttribute: an set reply" {
            if {$proto == 2} {
                catch {[r rw.attribute 3]} e
                assert_match "Attributes aren't supported by RESP 2" $e
            } else {
                r readraw 1
                set res [r rw.attribute 3]
                assert_equal [r read] {:0}
                assert_equal [r read] {,0}
                assert_equal [r read] {:1}
                assert_equal [r read] {,1.5}
                assert_equal [r read] {:2}
                assert_equal [r read] {,3}
                assert_equal [r read] {+OK}
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithBool: a boolean reply" {
            assert_equal {0 1} [r rw.bool]
        }

        test "RESP$proto: RM_ReplyWithNull: a NULL reply" {
            assert_equal {} [r rw.null]
        }

        test "RESP$proto: RM_ReplyWithError: an error reply" {
            catch {r rw.error} e
            assert_match "An error" $e
        }

        test "RESP$proto: RM_ReplyWithErrorFormat: error format reply" {
            catch {r rw.error_format "An error: %s" foo} e
            assert_match "An error: foo" $e  ;# Should not be used by a user, but compatible with RM_ReplyError

            catch {r rw.error_format "-ERR An error: %s" foo2} e
            assert_match "-ERR An error: foo2" $e  ;# Should not be used by a user, but compatible with RM_ReplyError (There are two hyphens, TCL removes the first one)

            catch {r rw.error_format "-WRONGTYPE A type error: %s" foo3} e
            assert_match "-WRONGTYPE A type error: foo3" $e  ;# Should not be used by a user, but compatible with RM_ReplyError (There are two hyphens, TCL removes the first one)

            catch {r rw.error_format "ERR An error: %s" foo4} e
            assert_match "ERR An error: foo4" $e

            catch {r rw.error_format "WRONGTYPE A type error: %s" foo5} e
            assert_match "WRONGTYPE A type error: foo5" $e
        }

        r hello 2
    }

    test "Unload the module - replywith" {
        assert_equal {OK} [r module unload replywith]
    }
}
