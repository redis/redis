start_server {tags {"expire"}} {
    test {EXPIRE - don't set timeouts multiple times} {
        r set x foobar
        set v1 [r expire x 5]
        set v2 [r ttl x]
        set v3 [r expire x 10]
        set v4 [r ttl x]
        list $v1 $v2 $v3 $v4
    } {1 5 0 5}

    test {EXPIRE - It should be still possible to read 'x'} {
        r get x
    } {foobar}

    test {EXPIRE - After 6 seconds the key should no longer be here} {
        after 6000
        list [r get x] [r exists x]
    } {{} 0}

    test {EXPIRE - Delete on write policy} {
        r del x
        r lpush x foo
        r expire x 1000
        r lpush x bar
        r lrange x 0 -1
    } {bar}

    test {EXPIREAT - Check for EXPIRE alike behavior} {
        r del x
        r set x foo
        r expireat x [expr [clock seconds]+15]
        r ttl x
    } {1[345]}

    test {SETEX - Set + Expire combo operation. Check for TTL} {
        r setex x 12 test
        r ttl x
    } {1[012]}

    test {SETEX - Check value} {
        r get x
    } {test}

    test {SETEX - Overwrite old key} {
        r setex y 1 foo
        r get y
    } {foo}

    test {SETEX - Wait for the key to expire} {
        after 3000
        r get y
    } {}

    test {SETEX - Wrong time parameter} {
        catch {r setex z -10 foo} e
        set _ $e
    } {*invalid expire*}
}
