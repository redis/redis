start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-AUTH fails if there is no password configured server side} {
        r restore-async-auth foo
    } {RESTORE-ASYNC-ACK 1 *}
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-AUTH fails when a wrong password is given} {
        r restore-async-auth wrong_passwd
    } {RESTORE-ASYNC-ACK 1 *}
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-SELECT fails when password is not given} {
        catch {r restore-async-select 1} err
        set _ $err
    } {NOAUTH*}

    test {RESTORE-ASYNC-AUTH succeeds when the right password is given} {
        r restore-async-auth foobar
    } {RESTORE-ASYNC-ACK 0 *}

    test {Once RESTORE-ASYNC-AUTH succeeded we can actually send commands to the server} {
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 9]
        r set foo 100
        r incr foo
    } {101}
}

start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-SELECT can change database} {
        r select 0
        r set foo 100

        r select 1
        r set foo 200

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {101} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {201} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {102} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {202} [r incr foo]
    }
}
