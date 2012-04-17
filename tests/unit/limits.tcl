start_server {tags {"limits"} overrides {maxclients 10}} {
    test {Check if maxclients works refusing connections} {
        set c 0
        catch {
            while {$c < 50} {
                incr c
                redis_deferring_client
                after 100
            }
        } e
        assert {$c > 8 && $c <= 10}
        set e
    } {*ERR max*reached*}
}
