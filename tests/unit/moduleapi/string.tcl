set testmodule [file normalize tests/modules/string.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module string set} {
        r set k mystring
        assert_equal mystring [r get k]
        assert_equal OK [r string.set k 123]
        r get k
    } {123}

    test {Module string set not keep ttl} {
        r set k mystring
        assert_equal mystring [r get k]
        r expire k 2
        assert_equal OK [r string.set k 123]
        after 3000
        r get k
    } {123}

    test {Module string get_interger work} {
        r set k 456
        assert_equal 456 [r get k]
        r string.get_interger k
    } {456}

    test {Module string get_interger not an interger} {
        r set k abc
        assert_equal abc [r get k]
        assert_error "ERR not an integer" {r string.get_interger k}
        r string.set k def
        assert_equal def [r get k]
        assert_error "ERR not an integer" {r string.get_interger k}
    }

    test "Unload the module - string" {
        assert_equal {OK} [r module unload string]
    }
}