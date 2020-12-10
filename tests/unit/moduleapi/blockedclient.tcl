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
    	r lpush l test
    	assert_equal [r do_rm_call blpop l 0] {l test}
    	
    	r lpush l test
    	assert_equal [r do_rm_call brpop l 0] {l test}
    	
    	r lpush l1 test
    	assert_equal [r do_rm_call brpoplpush l1 l2 0] {test}
    	assert_equal [r do_rm_call brpop l2 0] {l2 test}

    	r lpush l1 test
    	assert_equal [r do_rm_call blmove l1 l2 LEFT LEFT 0] {test}
    	assert_equal [r do_rm_call brpop l2 0] {l2 test}

    	r ZADD zset1 0 a 1 b 2 c
    	assert_equal [r do_rm_call bzpopmin zset1 0] {zset1 a 0}
    	assert_equal [r do_rm_call bzpopmax zset1 0] {zset1 c 2}

    	r xgroup create s g $ MKSTREAM
    	r xadd s * foo bar
    	assert {[r do_rm_call xread BLOCK 0 STREAMS s 0-0] ne {}}
    	assert {[r do_rm_call xreadgroup group g c BLOCK 0 STREAMS s >] ne {}}

    	assert {[r do_rm_call blpop empty_list 0] eq {}}
        assert {[r do_rm_call brpop empty_list 0] eq {}}
        assert {[r do_rm_call brpoplpush empty_list1 empty_list2 0] eq {}}
        assert {[r do_rm_call blmove empty_list1 empty_list2 LEFT LEFT 0] eq {}}
        
        assert {[r do_rm_call bzpopmin empty_zset 0] eq {}}
        assert {[r do_rm_call bzpopmax empty_zset 0] eq {}}
       
        r xgroup create empty_stream g $ MKSTREAM
        assert {[r do_rm_call xread BLOCK 0 STREAMS empty_stream $] eq {}}
        assert {[r do_rm_call xreadgroup group g c BLOCK 0 STREAMS empty_stream >] eq {}}

    }

    test {Monitor disallow inside RM_Call} {
        set e {}
        catch {
            r do_rm_call monitor
        } e
        set e
    } {*ERR*DENY BLOCKING*}

    test {subscribe disallow inside RM_Call} {
        set e {}
        catch {
            r do_rm_call subscribe x
        } e
        set e
    } {*ERR*DENY BLOCKING*}

    test {RM_Call from blocked client} {
        r hset hash foo bar
        r do_bg_rm_call hgetall hash
    } {foo bar}

    test {blocked client reaches client output buffer limit} {
        r hset hash big [string repeat x 50000]
        r hset hash bada [string repeat x 50000]
        r hset hash boom [string repeat x 50000]
        r config set client-output-buffer-limit {normal 100000 0 0}
        r client setname myclient
        catch {r do_bg_rm_call hgetall hash} e
        assert_match "*I/O error*" $e
        reconnect
        set clients [r client list]
        assert_no_match "*name=myclient*" $clients
    }
}
