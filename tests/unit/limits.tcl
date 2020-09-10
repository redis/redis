start_server {tags {"limits"} overrides {maxclients 10}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*ERR max*reached*"
    }
    r config set requirepass 123
    test {Check if maxclients works refusing connections} {
        set c 0
        catch {
            while {$c < 50} {
                set rd [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
                $rd auth 123
                $rd read
                incr c
                after 100
            }
        } e
        assert {$c > 8 && $c <= 10}
        set e
    } $expected_code
}

start_server {tags {"maxclients"} overrides {maxclients 10}} {
    r config set requirepass 123
    test {Only admin commands based on the last connection} {
        for {set c 0} {$c < 9} {incr c} {
            set rd [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
            $rd auth 123
            $rd read
            after 100
        }
        # Can't execute 'set' command
        set admin_set [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
        $admin_set auth 123
        $admin_set read
        catch {
            $admin_set set a b
            $admin_set read
        } e
        assert_match {*only can execute administrative commands*} $e

        # Execute 'config' command, and we can create a new connection
        # because last connection is closed by server
        set admin_config [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
        $admin_config auth 123
        $admin_config read
        # Incr maxclients
        $admin_config config set maxclients 20
        $admin_config read

        # We can create a new connection to execute 'set' command
        set rd [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
        $rd auth 123
        $rd read
        $rd set a b
        $rd read

        # Only extra 9 connection remaining
        if {$::tls} {
            set expected_code "*I/O error*"
        } else {
            set expected_code "*ERR max*reached*"
        }
        set c 0
        catch {
            while {$c < 50} {
                set rd [redis [srv 0 "host"] [srv 0 "port"] 1 $::tls]
                $rd auth 123
                $rd read
                incr c
                after 100
            }
        } e
        assert {$c == 9}
        assert_match $expected_code $e
    }
}
