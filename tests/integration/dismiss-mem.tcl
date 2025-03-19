# The tests of this file aim to get coverage on all the "dismiss" methods
# that dismiss all data-types memory in the fork child. like client query
# buffer, client output buffer and replication backlog.
# Actually, we may not have many asserts in the test, since we just check for
# crashes and the dump file inconsistencies.

start_server {tags {"dismiss external:skip"}} {
    # In other tests, although we test child process dumping RDB file, but
    # memory allocations of key/values are usually small, they couldn't cover
    # the "dismiss" object methods, in this test, we create big size key/values
    # to satisfy the conditions for release memory pages, especially, we assume
    # the page size of OS is 4KB in some cases.
    test {dismiss all data types memory} {
        set bigstr [string repeat A 8192]
        set 64bytes [string repeat A 64]

        # string
        populate 100 bigstring 8192

        # list
        r lpush biglist1 $bigstr            ; # uncompressed ziplist node
        r config set list-compress-depth 1  ; # compressed ziplist nodes
        for {set i 0} {$i < 16} {incr i} {
            r lpush biglist2 $bigstr
        }

        # set
        r sadd bigset1 $bigstr              ; # hash encoding
        set biginteger [string repeat 1 19]
        for {set i 0} {$i < 512} {incr i} {
            r sadd bigset2 $biginteger      ; # intset encoding
        }

        # zset
        r zadd bigzset1 1.0 $bigstr         ; # skiplist encoding
        for {set i 0} {$i < 128} {incr i} {
            r zadd bigzset2 1.0 $64bytes    ; # ziplist encoding
        }

        # hash
        r hset bighash1 field1 $bigstr      ; # hash encoding
        for {set i 0} {$i < 128} {incr i} {
            r hset bighash2 $i $64bytes     ; # ziplist encoding
        }

        # stream
        r xadd bigstream * entry1 $bigstr entry2 $bigstr

        set digest [debug_digest]
        r config set aof-use-rdb-preamble no
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
    }

    test {dismiss client output buffer} {
        # Big output buffer
        set item [string repeat "x" 100000]
        for {set i 0} {$i < 100} {incr i} {
            r lpush mylist $item
        }
        set rd [redis_deferring_client]
        $rd lrange mylist 0 -1
        $rd flush
        after 100

        r bgsave
        waitForBgsave r
        assert_equal $item [r lpop mylist]
    }

    test {dismiss client query buffer} {
        # Big pending query buffer
        set bigstr [string repeat A 8192]
        set rd [redis_deferring_client]
        $rd write "*2\r\n\$8192\r\n"
        $rd write $bigstr\r\n
        $rd flush
        after 100

        r bgsave
        waitForBgsave r
    }

    test {dismiss replication backlog} {
        set master [srv 0 client]
        start_server {} {
            r slaveof [srv -1 host] [srv -1 port]
            wait_for_sync r
            waitForBgsave $master

            set bigstr [string repeat A 8192]
            for {set i 0} {$i < 20} {incr i} {
                $master set $i $bigstr
            }
            $master bgsave
            waitForBgsave $master
        }
    }
}
