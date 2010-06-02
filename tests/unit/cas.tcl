start_server {tags {"cas"}} {
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
}
