start_server {tags {"obuf-limits"}} {
    test {Client output buffer hard limit is enforced} {
        r config set client-output-buffer-limit {pubsub 100000 0 0}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}
		set omem_max 0

        while 1 {
			if {[r publish foo bar] == 1} {
				set clients [split [r client list] "\r\n"]
				foreach client $clients {
					if {[regexp {omem=([0-9]+)} $client - omem_value]} {
						if {$omem_value > $omem_max} {
							set omem_max $omem_value
						}
					}
					if {$omem_max > 200000} break
				} 
			} else {
				break;
			}
        }
        assert {$omem_max >= 90000 && $omem_max < 200000}
        $rd1 close
    }

    test {Client output buffer soft limit is not enforced if time is not overreached} {
        r config set client-output-buffer-limit {pubsub 0 100000 10}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set start_time 0
        set time_elapsed 0
		set omem_max 0
        while 1 {
			if {[r publish foo bar] == 1} {
				set clients [split [r client list] "\r\n"]
				foreach client $clients {
					if {[regexp {omem=([0-9]+)} $client - omem_value]} {
						if {$omem_value > $omem_max} {
							set omem_max $omem_value
						}
					}
				}
				if {$omem_max > 100000} {
					if {$start_time == 0} {
						set start_time [clock seconds]
						}
					set time_elapsed [expr {[clock seconds]-$start_time}]
					if {$time_elapsed >= 5} {
						break
					}
				}
			} else {
				break;
			}
        }
        assert {$omem_max >= 100000 && $time_elapsed >= 5 && $time_elapsed <= 10}
        $rd1 close
    }

    test {Client output buffer soft limit is enforced if time is overreached} {
        r config set client-output-buffer-limit {pubsub 0 100000 3}
        set rd1 [redis_deferring_client]

        $rd1 subscribe foo
        set reply [$rd1 read]
        assert {$reply eq "subscribe foo 1"}

        set start_time 0
        set time_elapsed 0
		set omem_max 0
        while 1 {
			if {[r publish foo bar] == 1} {
				set clients [split [r client list] "\r\n"]
				foreach client $clients {
					if {[regexp {omem=([0-9]+)} $client - omem_value]} {
						if {$omem_value > $omem_max} {
							set omem_max $omem_value
						}
					}
				}
				if {$omem_max > 100000} {
	                if {$start_time == 0} {
						set start_time [clock seconds]
					}
					set time_elapsed [expr {[clock seconds]-$start_time}]
					if {$time_elapsed >= 10} {
						break
					}
				}
			} else {
				break;
			}
        }
        assert {$omem_max >= 100000 && $time_elapsed < 6}
        $rd1 close
    }
}
