set testmodule [file normalize tests/modules/stream.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module stream add and delete} {
        r del mystream
        # add to empty key
        set streamid1 [r stream.add mystream item 1 value a]
        # add to existing stream
        set streamid2 [r stream.add mystream item 2 value b]
        # check result
        assert { [string match "*-*" $streamid1] }
        set items [r XRANGE mystream - +]
        assert_equal $items \
            "{$streamid1 {item 1 value a}} {$streamid2 {item 2 value b}}"
        # delete one of them and try deleting non-existing ID
        assert_equal OK [r stream.delete mystream $streamid1]
        assert_error "ERR StreamDelete*" {r stream.delete mystream 123-456}
        assert_error "Invalid stream ID*" {r stream.delete mystream foo}
        assert_equal "{$streamid2 {item 2 value b}}" [r XRANGE mystream - +]
        # check error condition: wrong type
        r del mystream
        r set mystream mystring
        assert_error "ERR StreamAdd*" {r stream.add mystream item 1 value a}
        assert_error "ERR StreamDelete*" {r stream.delete mystream 123-456}
    }

    test {Module stream add unblocks blocking xread} {
        r del mystream

        # Blocking XREAD on an empty key
        set rd1 [redis_deferring_client]
        $rd1 XREAD BLOCK 3000 STREAMS mystream $
        # wait until client is actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client is not blocked"
        }
        set id [r stream.add mystream field 1 value a]
        assert_equal "{mystream {{$id {field 1 value a}}}}" [$rd1 read]

        # Blocking XREAD on an existing stream
        set rd2 [redis_deferring_client]
        $rd2 XREAD BLOCK 3000 STREAMS mystream $
        # wait until client is actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client is not blocked"
        }
        set id [r stream.add mystream field 2 value b]
        assert_equal "{mystream {{$id {field 2 value b}}}}" [$rd2 read]
    }

    test {Module stream add benchmark (1M stream add)} {
        set n 1000000
        r del mystream
        set result [r stream.addn mystream $n field value]
        assert_equal $result $n
    }

    test {Module stream iterator} {
        r del mystream
        set streamid1 [r xadd mystream * item 1 value a]
        set streamid2 [r xadd mystream * item 2 value b]
        # range result
        set result1 [r stream.range mystream "-" "+"]
        set expect1 [r xrange mystream "-" "+"]
        assert_equal $result1 $expect1
        # reverse range
        set result_rev [r stream.range mystream "+" "-"]
        set expect_rev [r xrevrange mystream "+" "-"]
        assert_equal $result_rev $expect_rev

        # only one item: range with startid = endid
        set result2 [r stream.range mystream "-" $streamid1]
        assert_equal $result2 "{$streamid1 {item 1 value a}}"
        assert_equal $result2 [list [list $streamid1 {item 1 value a}]]
        # only one item: range with startid = endid
        set result3 [r stream.range mystream $streamid2 $streamid2]
        assert_equal $result3 "{$streamid2 {item 2 value b}}"
        assert_equal $result3 [list [list $streamid2 {item 2 value b}]]
    }

    test {Module stream iterator delete} {
        r del mystream
        set id1 [r xadd mystream * normal item]
        set id2 [r xadd mystream * selfdestruct yes]
        set id3 [r xadd mystream * another item]
        # stream.range deletes the "selfdestruct" item after returning it
        assert_equal \
            "{$id1 {normal item}} {$id2 {selfdestruct yes}} {$id3 {another item}}" \
            [r stream.range mystream - +]
        # now, the "selfdestruct" item is gone
        assert_equal \
            "{$id1 {normal item}} {$id3 {another item}}" \
            [r stream.range mystream - +]
    }

    test {Module stream trim by length} {
        r del mystream
        # exact maxlen
        r xadd mystream * item 1 value a
        r xadd mystream * item 2 value b
        r xadd mystream * item 3 value c
        assert_equal 3 [r xlen mystream]
        assert_equal 0 [r stream.trim mystream maxlen = 5]
        assert_equal 3 [r xlen mystream]
        assert_equal 2 [r stream.trim mystream maxlen = 1]
        assert_equal 1 [r xlen mystream]
        assert_equal 1 [r stream.trim mystream maxlen = 0]
        # check that there is no limit for exact maxlen
        r stream.addn mystream 20000 item x value y
        assert_equal 20000 [r stream.trim mystream maxlen = 0]
        # approx maxlen (100 items per node implies default limit 10K items)
        r stream.addn mystream 20000 item x value y
        assert_equal 20000 [r xlen mystream]
        assert_equal 10000 [r stream.trim mystream maxlen ~ 2]
        assert_equal 9900  [r stream.trim mystream maxlen ~ 2]
        assert_equal 0     [r stream.trim mystream maxlen ~ 2]
        assert_equal 100   [r xlen mystream]
        assert_equal 100   [r stream.trim mystream maxlen ~ 0]
        assert_equal 0     [r xlen mystream]
    }

    test {Module stream trim by ID} {
        r del mystream
        # exact minid
        r xadd mystream * item 1 value a
        r xadd mystream * item 2 value b
        set minid [r xadd mystream * item 3 value c]
        assert_equal 3 [r xlen mystream]
        assert_equal 0 [r stream.trim mystream minid = -]
        assert_equal 3 [r xlen mystream]
        assert_equal 2 [r stream.trim mystream minid = $minid]
        assert_equal 1 [r xlen mystream]
        assert_equal 1 [r stream.trim mystream minid = +]
        # check that there is no limit for exact minid
        r stream.addn mystream 20000 item x value y
        assert_equal 20000 [r stream.trim mystream minid = +]
        # approx minid (100 items per node implies default limit 10K items)
        r stream.addn mystream 19980 item x value y
        set minid [r xadd mystream * item x value y]
        r stream.addn mystream 19 item x value y
        assert_equal 20000 [r xlen mystream]
        assert_equal 10000 [r stream.trim mystream minid ~ $minid]
        assert_equal 9900  [r stream.trim mystream minid ~ $minid]
        assert_equal 0     [r stream.trim mystream minid ~ $minid]
        assert_equal 100   [r xlen mystream]
        assert_equal 100   [r stream.trim mystream minid ~ +]
        assert_equal 0     [r xlen mystream]
    }

    test "Unload the module - stream" {
        assert_equal {OK} [r module unload stream]
    }
}
