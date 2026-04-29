start_server {tags {"limits network external:skip"} overrides {maxclients 10}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*I/O error*"
    }
    test {Check if maxclients works refusing connections} {
        puts "CLIENTS BEFORE LOOP: [r info clients]"

        set c 0
        catch {
            while {$c < 50} {
                puts "Connecting client $c..."
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
