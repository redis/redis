start_server {tags {"auth"}} {
    test {AUTH fails if there is no password configured server side} {
        catch {r auth foo} err
        set _ $err
    } {ERR*no password*}
}

start_server {tags {"auth"} overrides {requirepass foobar}} {
    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR*invalid password}

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
}

start_server {tags {"auth"} overrides {requirepass foo requirepass-alt bar}} {
    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        set _ $err
    } {ERR*invalid password}

    test {Arbitrary command gives an error when AUTH is required} {
        catch {r set foo bar} err
        set _ $err
    } {NOAUTH*}

    test {AUTH succeeds when the right password is given} {
        r auth foo
    } {OK}

    test {AUTH succeeds when the right alternative password is given} {
        r auth bar
    } {OK}
}

start_server {tags {"auth"} overrides {requirepass-alt foobar}} {
    test {Alternative password is not effective if requirepass is not set} {
        catch {r auth foobar} err
        set _ $err
    } {ERR*no password*}
}
