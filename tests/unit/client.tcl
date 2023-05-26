start_server {} {
    test {CLIENT command against wrong number of arguments or wrong arguments} {
        # CLIENT Caching wrong number of arguments
        assert_error "ERR*wrong number of arguments*" {r client caching}
        # CLIENT Caching wrong argument
        assert_error "ERR*when the client is in tracking mode*" {r client caching maybe}
        # CLIENT no-evict wrong argument
        assert_error "ERR*syntax*" {r client no-evict wrongInput}
        # CLIENT reply wrong argument
        assert_error "ERR*syntax*" {r client reply wrongInput}
        # CLIENT tracking wrong argument
        assert_error "ERR*syntax*" {r client tracking wrongInput}
        # CLIENT tracking wrong option
        assert_error "ERR*syntax*" {r client tracking on wrongInput}
    }

    test {CLIENT command against caching option} {
        # CLIENT Caching OFF without optout
        assert_error "ERR*when the client is in tracking mode*" {r client caching off}
        # CLIENT Caching ON without optin
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}
        # CLIENT Caching ON with optout
        r CLIENT TRACKING ON optout
        assert_error "ERR*syntax*" {r client caching on}
        # CLIENT Caching OFF with optin
        r CLIENT TRACKING off optout
        assert_error "ERR*when the client is in tracking mode*" {r client caching on}
    }

    test {CLIENT command against kill} {
        # kill wrong address
        assert_error "ERR*No such*" {r client kill 000.123.321.567:0000}
        # kill no port
        assert_error "ERR*No such*" {r client kill 127.0.0.1:}
    }

    test {CLIENT command against pause} {
        # wrong timeout type
        assert_error "ERR*timeout is not an integer*" {r client pause abc}
        # pause negative timeout
        assert_error "ERR timeout is negative" {r client pause -1}
    }

    test {CLIENT getname check if name set correctly} {
        r client setname testName
        r client getName
    } {testName}
}
