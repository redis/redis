start_server {tags "clients"} {
    r config set swap-debug-rio-delay 1000
    set host [srv 0 host]
    set port [srv 0 port]

    test {kill client just finish swap} {
        set r1 [redis $host $port]
        set r2 [redis $host $port]
        $r1 blocking 0
        $r1 sadd s1 m1 m2 m3
        $r1 swap.evict s1
        $r1 sismember s1 m1
        $r2 multi
        $r2 slaveof no one
        $r2 client kill type normal
        $r2 exec

        set r3 [redis $host $port]
        $r3 ping
    }
}

start_server {tags "clients"} {
    set host [srv 0 host]
    set port [srv 0 port]

    test {server kill conns} {
        set rounds 50
        set load 10
        for {set round 0} {$round < $rounds} {incr round} {
            if {0 == $round%10} { puts "test rounds $round" }
            set conns {}
            for {set i 0} {$i < $load} {incr i} {
                lappend conns [start_write_load_ignore_err $host $port 10]
            }

            set r [redis $host $port]
            wait_for_condition 10 10 {
                [llength [regexp -inline -all {name=LOAD_HANDLER} [$r client list]]] == $load
            } else {
                fail "start client too slow"
            }
            $r client kill type normal

            set r [redis $host $port]
            wait_for_condition 10 100 {
                ![string match {*name=LOAD_HANDLER*} [$r client list]]
            } else {
                puts [$r client list]
                fail "load_handler(s) still connected after too long time."
            }
            foreach conn $conns {
                stop_write_load $conn
            }
            $r ping
        }
    }

    test {client kill conns} {
        set rounds 50
        set load 10
        for {set round 0} {$round < $rounds} {incr round} {
            if {0 == $round%10} { puts "test rounds $round" }
            set conns {}
            set r [redis $host $port]
            for {set i 0} {$i < $load} {incr i} {
                lappend conns [start_write_load $host $port 10]
            }

            wait_for_condition 10 10 {
                [llength [regexp -inline -all {name=LOAD_HANDLER} [$r client list]]] == $load
            } else {
                fail "start client too slow"
            }
            foreach conn $conns {
                stop_write_load $conn
            }

            wait_for_condition 100 1000 {
                ![string match {*name=LOAD_HANDLER*} [$r client list]]
            } else {
                puts [$r client list]
                fail "load_handler(s) still connected after too long time."
            }
            $r ping
        }
    }

}
