# source tests/support/util.tcl

set testmodule [file normalize tests/modules/blockedclient.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Locked GIL acquisition} {
        assert_match "OK" [r acquire_gil]
    }

    test {Locked GIL acquisition during multi} {
    	r multi
    	r acquire_gil
    	assert_equal {{Blocked client is not supported inside multi}} [r exec]
    }

    test {Locked GIL acquisition from RM_Call} {
    	assert_equal {Blocked client is not allowed} [r do_rm_call acquire_gil]
    }

    test {Blocking command are not block the client on RM_Call} {
    	assert {[r do_rm_call blpop empty_list 10] eq {}}
        assert {[r do_rm_call brpop empty_list 10] eq {}}
        assert {[r do_rm_call brpoplpush empty_list1 empty_list2 10] eq {}}
        assert {[r do_rm_call blmove empty_list1 empty_list2 LEFT LEFT 10] eq {}}
        assert {[r do_rm_call bzpopmin empty_zset 10] eq {}}
        assert {[r do_rm_call bzpopmax empty_zset 10] eq {}}
    }
}
