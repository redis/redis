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

    test {Once RESTORE-ASYNC-AUTH succeeded we can actually send commands to the server} {
        assert_equal OK [r set foo 100]
        assert_equal {101} [r incr foo]
    }
}

start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-SELECT can change database} {
        assert_equal OK [r select 0]
        assert_equal OK [r set foo 100]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {101} [r incr foo]

        assert_equal OK [r select 1]
        assert_equal OK [r set foo 200]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {201} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {102} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {202} [r incr foo]
    }

    test {RESTORE-ASYNC DELETE against a single item} {
        assert_equal OK [r set foo "hello"]
        assert_equal {hello} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]
    }
}
