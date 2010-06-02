start_server {tags {"auth"} overrides {requirepass foobar}} {
    test {AUTH fails when a wrong password is given} {
        catch {r auth wrong!} err
        format $err
    } {ERR*invalid password}
    
    test {Arbitrary command gives an error when AUTH is required} {
        catch {r set foo bar} err
        format $err
    } {ERR*operation not permitted}

    test {AUTH succeeds when the right password is given} {
        r auth foobar
    } {OK}
}
