start_server {tags {"auth external:skip"}} {
    test {AUTH fails if there is no password configured server side} {
        catch {r auth foo} err
        set _ $err
    } {ERR *any password*}

    test {Arity check for auth command} {
        catch {r auth a b c} err
        set _ $err
    } {*syntax error*}
}

start_server {tags {"auth external:skip"} overrides {requirepass foobar}} {
    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        set _ $err
    } {WRONGPASS*}

    test {Arbitrary command gives an error when AUTH is required} {
        catch {r set foo bar} err
        set _ $err
    } {NOAUTH*}

    test {AUTH succeeds when the right password is given} {
        r auth foobar
    } {OK}

    test {Once AUTH succeeded we can actually send commands to the server} {
        r set foo 100
        r incr foo
    } {101}

    test {For unauthenticated clients multibulk and bulk length are limited} {
        set rr [redis [srv "host"] [srv "port"] 0 $::tls]
        $rr write "*100\r\n"
        $rr flush
        catch {[$rr read]} e
        assert_match {*unauthenticated multibulk length*} $e
        $rr close

        set rr [redis [srv "host"] [srv "port"] 0 $::tls]
        $rr write "*1\r\n\$100000000\r\n"
        $rr flush
        catch {[$rr read]} e
        assert_match {*unauthenticated bulk length*} $e
        $rr close
    }

    test {Default max-auth-age is 0} {
        assert {[r config get max-auth-age] == "max-auth-age 0"}
    }

    test {Authenticated clients gets disconnected after max-auth-age} {
        assert_equal {OK} [r config set max-auth-age 1]
        after 2000
        catch {r set foo bar} e
        assert_match {I/O error reading reply} $e
    }

    test {Reconnect and reauthentication brings back access} {
        assert_equal {OK} [r auth foobar]
        assert_equal {OK} [r set foo bar]
        assert_match {server*} [r hello]
    }
}

start_server {tags {"auth_binary_password external:skip"}} {
    test {AUTH fails when binary password is wrong} {
        r config set requirepass "abc\x00def"
        catch {r auth abc} err
        set _ $err
    } {WRONGPASS*}

    test {AUTH succeeds when binary password is correct} {
        r config set requirepass "abc\x00def"
        r auth "abc\x00def"
    } {OK}

    start_server {tags {"masterauth"}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {MASTERAUTH test with binary password} {
            $master config set requirepass "abc\x00def"

            # Configure the replica with masterauth
            set loglines [count_log_lines 0]
            $slave slaveof $master_host $master_port
            $slave config set masterauth "abc"

            # Verify replica is not able to sync with master
            wait_for_log_messages 0 {"*Unable to AUTH to MASTER*"} $loglines 1000 10
            assert_equal {down} [s 0 master_link_status]
            
            # Test replica with the correct masterauth
            $slave config set masterauth "abc\x00def"
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Can't turn the instance into a replica"
            }
        }
    }
}

start_server {tags {"auth external:skip"}} {
    test {Default user can access Redis} {
        assert_equal {OK} [r set foo bar]
    }

    test {Enabling max-auth-age does not un-authenticates default users} {
        assert_equal {OK} [r config set max-auth-age 1]
        after 2000
        assert_equal {OK} [r set foo bar]
        assert_match {server*} [r hello]
    }
}
