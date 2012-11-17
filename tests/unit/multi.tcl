start_server {tags {"multi"}} {
    test {MUTLI / EXEC basics} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r multi
        set v1 [r lrange mylist 0 -1]
        set v2 [r ping]
        set v3 [r exec]
        list $v1 $v2 $v3
    } {QUEUED QUEUED {{a b c} PONG}}

    test {DISCARD} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r multi
        set v1 [r del mylist]
        set v2 [r discard]
        set v3 [r lrange mylist 0 -1]
        list $v1 $v2 $v3
    } {QUEUED OK {a b c}}

    test {Nested MULTI are not allowed} {
        set err {}
        r multi
        catch {[r multi]} err
        r exec
        set _ $err
    } {*ERR MULTI*}

    test {MULTI where commands alter argc/argv} {
        r sadd myset a
        r multi
        r spop myset
        list [r exec] [r exists myset]
    } {a 0}

    test {WATCH inside MULTI is not allowed} {
        set err {}
        r multi
        catch {[r watch x]} err
        r exec
        set _ $err
    } {*ERR WATCH*}

    test {EXEC works on WATCHed key not modified} {
        r watch x y z
        r watch k
        r multi
        r ping
        r exec
    } {PONG}

    test {EXEC fail on WATCHed key modified (1 key of 1 watched)} {
        r set x 30
        r watch x
        r set x 40
        r multi
        r ping
        r exec
    } {}

    test {EXEC fail on WATCHed key modified (1 key of 5 watched)} {
        r set x 30
        r watch a b x k z
        r set x 40
        r multi
        r ping
        r exec
    } {}

    test {EXEC fail on WATCHed key modified by SORT with STORE even if the result is empty} {
        r flushdb
        r lpush foo bar
        r watch foo
        r sort emptylist store foo
        r multi
        r ping
        r exec
    } {}

    test {After successful EXEC key is no longer watched} {
        r set x 30
        r watch x
        r multi
        r ping
        r exec
        r set x 40
        r multi
        r ping
        r exec
    } {PONG}

    test {After failed EXEC key is no longer watched} {
        r set x 30
        r watch x
        r set x 40
        r multi
        r ping
        r exec
        r set x 40
        r multi
        r ping
        r exec
    } {PONG}

    test {It is possible to UNWATCH} {
        r set x 30
        r watch x
        r set x 40
        r unwatch
        r multi
        r ping
        r exec
    } {PONG}

    test {UNWATCH when there is nothing watched works as expected} {
        r unwatch
    } {OK}

    test {FLUSHALL is able to touch the watched keys} {
        r set x 30
        r watch x
        r flushall
        r multi
        r ping
        r exec
    } {}

    test {FLUSHALL does not touch non affected keys} {
        r del x
        r watch x
        r flushall
        r multi
        r ping
        r exec
    } {PONG}

    test {FLUSHDB is able to touch the watched keys} {
        r set x 30
        r watch x
        r flushdb
        r multi
        r ping
        r exec
    } {}

    test {FLUSHDB does not touch non affected keys} {
        r del x
        r watch x
        r flushdb
        r multi
        r ping
        r exec
    } {PONG}

    test {WATCH is able to remember the DB a key belongs to} {
        r select 5
        r set x 30
        r watch x
        r select 1
        r set x 10
        r select 5
        r multi
        r ping
        r exec
    } {PONG}

    test {WATCH will consider touched keys target of EXPIRE} {
        r del x
        r set x foo
        r watch x
        r expire x 10
        r multi
        r ping
        r exec
    } {}

    test {WATCH will not consider touched expired keys} {
        r del x
        r set x foo
        r expire x 1
        r watch x
        after 1100
        r multi
        r ping
        r exec
    } {PONG}

    test {DISCARD should clear the WATCH dirty flag on the client} {
        r watch x
        r set x 10
        r multi
        r discard
        r multi
        r incr x
        r exec
    } {11}

    test {DISCARD should UNWATCH all the keys} {
        r watch x
        r set x 10
        r multi
        r discard
        r set x 10
        r multi
        r incr x
        r exec
    } {11}
}
