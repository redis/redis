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
}
