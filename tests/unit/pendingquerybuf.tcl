proc info_memory {r property} {
    if {[regexp "\r\n$property:(.*?)\r\n" [{*}$r info memory] _ value]} {
        set _ $value
    }
}

proc prepare_value {size} {
    set _v "c" 
    for {set i 1} {$i < $size} {incr i} {
        append _v 0   
    }
    return $_v
}

start_server {tags {"wait"}} {
start_server {} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set master [srv -1 client]
    set master_host [srv -1 host]
    set master_port [srv -1 port]

    test "pending querybuf: check size of pending_querybuf after set a big value" {
        $slave slaveof $master_host $master_port
        set _v [prepare_value [expr 32*1024*1024]]
        $master set key $_v 
        after 2000
        set m_usedmemory [info_memory $master used_memory]
        set s_usedmemory [info_memory $slave used_memory]
        if { $s_usedmemory > $m_usedmemory + 10*1024*1024 } {
            fail "the used_memory of replica is much larger than master. Master:$m_usedmemory Replica:$s_usedmemory"
        }
    }  
}}
