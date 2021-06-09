start_server {tags {"auth"}} {
    test {AUTH fails if there is no password configured server side} {
        catch {r auth foo} err
        set _ $err
    } {ERR*any password*}
}

start_server {tags {"auth"} overrides {requirepass foobar}} {
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
