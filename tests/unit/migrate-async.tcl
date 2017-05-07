start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-AUTH fails if there is no password configured server side} {
        assert_match {RESTORE-ASYNC-ACK 1 *} [r restore-async-auth foo]
    }
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-AUTH fails when a wrong password is given} {
        assert_match {RESTORE-ASYNC-ACK 1 *} [r restore-async-auth wrong_passwd]
    }
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-SELECT fails when password is not given} {
        catch {r restore-async-select 1} err
        assert_match {NOAUTH*} $err
    }

    test {RESTORE-ASYNC-AUTH succeeds when the right password is given} {
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-auth foobar]
    }

    test {RESTORE-ASYNC-AUTH succeeded then we can actually send commands to the server} {
        assert_equal OK [r set foo 100]
        assert_equal {101} [r incr foo]
    }
}

start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-SELECT can change database} {
        r select 0
        r set foo 100

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {101} [r incr foo]

        r select 1
        r set foo 200

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {201} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {102} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {202} [r incr foo]
    }

    test {RESTORE-ASYNC DELETE against a single item} {
        r set foo hello
        assert_equal {hello} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]
    }
}

start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC STRING against a string item} {
        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string foo 0 hello]
        assert_equal {hello} [r get foo]
        assert_equal {-1} [r pttl foo]

        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string foo 5000 world]
        assert_equal {world} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl >= 3000 && $ttl <= 5000}

        r del bar
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string bar 0 10000]
        assert_equal {10001} [r incr bar]
    }

    test {RESTORE-ASYNC STRING against a string item (already exists)} {
        r set var exists
        assert_match {RESTORE-ASYNC-ACK 1 *} [r restore-async string var 0 payload]
    }
}
