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
