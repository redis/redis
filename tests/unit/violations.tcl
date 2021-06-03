# These tests consume massive amounts of memory, and are not
# suitable to be executed as part of the normal test suite
set ::str500 [string repeat x 500000000] ;# 500mb

# Utility function to write big argument into redis client connection
proc write_big_bulk {size} {
    r write "\$$size\r\n"
    while {$size >= 500000000} {
        r write $::str500
        incr size -500000000
    }
    if {$size > 0} {
        r write [string repeat x $size]
    }
    r write "\r\n"
}

# One XADD with one huge 5GB field
# Expected to fail resulting in an empty stream
start_server [list overrides [list save ""] ] {
    test {XADD one huge field} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*5\r\n\$4\r\nXADD\r\n\$2\r\nS1\r\n\$1\r\n*\r\n"
        r write "\$1\r\nA\r\n"
        write_big_bulk 5000000000 ;#5gb
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r xlen S1
    } {0}
}

# One XADD with one huge (exactly nearly) 4GB field
# This uncovers the overflow in lpEncodeGetType
# Expected to fail resulting in an empty stream
start_server [list overrides [list save ""] ] {
    test {XADD one huge field - 1} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*5\r\n\$4\r\nXADD\r\n\$2\r\nS1\r\n\$1\r\n*\r\n"
        r write "\$1\r\nA\r\n"
        write_big_bulk 4294967295 ;#4gb-1
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r xlen S1
    } {0}
}

# Gradually add big stream fields using repeated XADD calls
start_server [list overrides [list save ""] ] {
    test {several XADD big fields} {
        r config set stream-node-max-bytes 0
        for {set j 0} {$j<10} {incr j} {
            r xadd stream * 1 $::str500 2 $::str500
        }
        r ping
        r xlen stream
    } {10}
}

# Add over 4GB to a single stream listpack (one XADD command)
# Expected to fail resulting in an empty stream
start_server [list overrides [list save ""] ] {
    test {single XADD big fields} {
        r write "*23\r\n\$4\r\nXADD\r\n\$1\r\nS\r\n\$1\r\n*\r\n"
        for {set j 0} {$j<10} {incr j} {
            r write "\$1\r\n$j\r\n"
            write_big_bulk 500000000 ;#500mb
        }
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r xlen S
    } {0}
}

# Gradually add big hash fields using repeated HSET calls
# This reproduces the overflow in the call to ziplistResize
# Object will be converted to hashtable encoding
start_server [list overrides [list save ""] ] {
    r config set hash-max-ziplist-value 1000000000 ;#1gb
    test {hash with many big fields} {
        for {set j 0} {$j<10} {incr j} {
            r hset h $j $::str500
        }
        r object encoding h
    } {hashtable}
}

# Add over 4GB to a single hash field (one HSET command)
# Object will be converted to hashtable encoding
start_server [list overrides [list save ""] ] {
    test {hash with one huge field} {
        catch {r config set hash-max-ziplist-value 10000000000} ;#10gb
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*4\r\n\$4\r\nHSET\r\n\$2\r\nH1\r\n"
        r write "\$1\r\nA\r\n"
        write_big_bulk 5000000000 ;#5gb
        r flush
        r read
        r object encoding H1
    } {hashtable}
}

# Add over 4GB to a single list member (one LPUSH command)
# Currently unsupported, and expected to fail rather than being truncated
# Expected to fail resulting in a non-existing list
start_server [list overrides [list save ""] ] {
    test {list with one huge field} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*3\r\n\$5\r\nLPUSH\r\n\$2\r\nL1\r\n"
        write_big_bulk 5000000000 ;#5gb
        r flush
        catch {r read} err
        assert_match {*too large*} $err
        r exists L1
    } {0}
}

# SORT which attempts to store an element larger than 4GB into a list.
# Currently unsupported and results in an assertion instead of truncation
start_server [list overrides [list save ""] ] {
    test {SORT adds huge field to list} {
        r config set proto-max-bulk-len 10000000000 ;#10gb
        r config set client-query-buffer-limit 10000000000 ;#10gb
        r write "*3\r\n\$3\r\nSET\r\n\$2\r\nS1\r\n"
        write_big_bulk 5000000000 ;#5gb
        r flush
        r read
        assert_equal [r strlen S1] 5000000000
        r set S2 asdf
        r sadd myset 1 2
        r mset D1 1 D2 2
        catch {r sort myset by D* get S* store mylist}
        # assert_equal [count_log_message 0 "crashed by signal"] 0   - not suitable for 6.0
        assert_equal [count_log_message 0 "ASSERTION FAILED"] 1
    }
}

# SORT which stores an integer encoded element into a list.
# Just for coverage, no news here.
start_server [list overrides [list save ""] ] {
    test {SORT adds integer field to list} {
        r set S1 asdf
        r set S2 123 ;# integer encoded
        assert_encoding "int" S2
        r sadd myset 1 2
        r mset D1 1 D2 2
        r sort myset by D* get S* store mylist
        r llen mylist
    } {2}
}
