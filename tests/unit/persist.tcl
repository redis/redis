start_server {tags {"persist"}} {
    test {PERSIST - Key does not exist} {
        r persist x
    } {0}

    test {PERSIST - Key exists but no expiry} {
        r set x foobar
        r persist x
    } {0}

    test {PERSIST - Key exists with expiry} {
        r set x foobar
        r expire x 10
        r persist x
    } {1}

    test {MPERSIST - Keys does not exist} {
        r mpersist x y z
    } {0}

    test {MPERSIST - Key exists but no expiry} {
        r set x foobar
        r set y foobar2
        r mpersist x y
    } {0}

    test {MPERSIST - Key exists with expiry} {
        r set x foobar
        r set y foobar2
        r expire x 10
        r expire y 10
        r mpersist x y
    } {2}

    test {MPERSIST - Some keys don't have expiry} {
        r set x foobar
        r set y foobar2
        r set z foobar3
        r expire x 10
        r expire y 10
        r mpersist x y z
    } {2} 

    test {MPERSIST - Some keys don't exist} {
        r set x foobar
        r set y foobar2
        r expire x 10
        r expire y 10
        r mpersist x y z
    } {2}   
}