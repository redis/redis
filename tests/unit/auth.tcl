#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

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

        foreach rdbchannel {yes no} {
            test "MASTERAUTH test with binary password rdbchannel=$rdbchannel" {
                $slave slaveof no one
                $master config set requirepass "abc\x00def"
                $master config set repl-rdb-channel $rdbchannel

                # Configure the replica with masterauth
                set loglines [count_log_lines 0]
                $slave config set masterauth "abc"
                $slave config set repl-rdb-channel $rdbchannel
                $slave slaveof $master_host $master_port

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
}
