set testmodule [file normalize tests/modules/stream.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module stream add} {
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
        # check error condition
        r del mystream
        r set mystream mystring
        catch {r stream.add mystream item 1 value a} e
        assert_equal $e "ERR StreamAdd failed"
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
}
