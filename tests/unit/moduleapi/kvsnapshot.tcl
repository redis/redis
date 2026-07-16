set testmodule [file normalize tests/modules/kvsnapshot.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {keyspace value-MVCC snapshot: values frozen across mutation and delete} {
        assert_equal OK [r kvsnap.test]
    }

    test {keyspace snapshot version advances with writes} {
        set v1 [dict get [r debug kvsnapshot stats] keyspace_version]
        r set somerandomkey someval
        set v2 [dict get [r debug kvsnapshot stats] keyspace_version]
        assert {$v2 > $v1}
    }

    test {snapshot read from a background thread (no concurrent write)} {
        r del doc
        r hset doc f original
        set rd [redis_deferring_client]
        $rd kvsnap.threadget doc f 50
        assert_equal original [$rd read]
        $rd close
    }

    test {snapshot read from a background thread is consistent across a concurrent write} {
        # NOTE: r and the deferring client must be on the same DB — snapshots are
        # DB-scoped. Both default to the test DB here.
        r del doc
        r hset doc f original
        set rd [redis_deferring_client]
        # Blocks: the snapshot is created now (main thread), the worker sleeps
        # 300ms and then reads doc.f as-of the snapshot under the GIL.
        $rd kvsnap.threadget doc f 300
        after 80             ;# let the command create the snapshot and block
        r hset doc f changed ;# concurrent write on the main thread during the worker's sleep
        assert_equal original [$rd read]   ;# the worker saw the pre-write value
        assert_equal changed [r hget doc f];# live reflects the write
        $rd close
    }
}
