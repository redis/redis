start_server {tags "client test"} {
    r config set debug-rio-latency 1000
    set host [srv 0 host]
    set port [srv 0 port]

    test {kill client just finish swap} {
        set r1 [redis $host $port]
        set r2 [redis $host $port]
        $r1 blocking 0
        $r1 sadd s1 m1 m2 m3
        $r1 evict s1
        $r1 sismember s1 m1
        $r2 multi
        $r2 slaveof no one
        $r2 client kill type normal
        $r2 exec

        set r3 [redis $host $port]
        $r3 ping
    }
}
