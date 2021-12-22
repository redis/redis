proc info_memory {r property} {
    if {[regexp "\r\n$property:(.*?)\r\n" [{*}$r info memory] _ value]} {
        set _ $value
    }
}

start_server {tags {"wait external:skip"}} {
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
        wait_for_ofs_sync $master $slave

        wait_for_condition 50 100 {
            [info_memory $slave used_memory] <= [info_memory $master used_memory] + 10*1024*1024
        } else {
            fail "the used_memory of replica is much larger than master."
        }
    }  
}}
