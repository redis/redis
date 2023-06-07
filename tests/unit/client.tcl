start_server {} {
    test {CLIENT command against wrong number of arguments or wrong arguments} {
        assert_error "ERR*wrong number of arguments*" {r client caching}
        assert_error "ERR*when the client is in tracking mode*" {r client caching maybe}
        assert_error "ERR*syntax*" {r client no-evict wrongInput}
        assert_error "ERR*syntax*" {r client reply wrongInput}
        assert_error "ERR*syntax*" {r client tracking wrongInput}
        assert_error "ERR*syntax*" {r client tracking on wrongInput}
    }

    test {CLIENT command against caching option} {
        assert_error "ERR*when the client is in tracking mode*" {r client caching off}
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}

        r CLIENT TRACKING ON optout
        assert_error "ERR*syntax*" {r client caching on}

        r CLIENT TRACKING off optout
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}
    }

    test {CLIENT command against kill} {
        assert_error "ERR*No such*" {r client kill 000.123.321.567:0000}
        assert_error "ERR*No such*" {r client kill 127.0.0.1:}
    }

    test {CLIENT command against pause} {
        assert_error "ERR*timeout is not an integer*" {r client pause abc}
        assert_error "ERR timeout is negative" {r client pause -1}
    }

    test {CLIENT getname check if name set correctly} {
        r client setname testName
        r client getName
    } {testName}
}
