tags {"bitmap" "external:skip"} {
    test {redis-bitmap-migrate fake RESP integration tests} {
        set python [auto_execok python3]
        if {$python eq ""} {
            set python [auto_execok python]
        }
        if {$python eq ""} {
            error "python not available"
        }

        set output [exec {*}$python tests/integration/redis-bitmap-migrate.py 2>@1]
        assert_match {*Ran 6 tests*OK*} $output
    }
}
