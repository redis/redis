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
    test {RESTORE-ASYNC-AUTH succeeds when the right password is given} {
        r restore-async-auth foobar
    } {RESTORE-ASYNC-ACK 0 *}

    test {Once RESTORE-ASYNC-AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}
}
