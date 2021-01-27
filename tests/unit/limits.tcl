start_server {tags {"limits network"} overrides {maxclients 10}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*ERR max*reached*"
    }
    test {Check if maxclients works refusing connections} {
        set c 0
        catch {
            while {$c < 50} {
                incr c
                set rd [redis_deferring_client]
                $rd ping
                $rd read
                after 100
            }
        } e
        assert {$c > 8 && $c <= 10}
        set e
    } $expected_code
}
