proc get_reply_buffer_size {cname} {

    set clients [split [string trim [r client list]] "\r\n"]
    set c [lsearch -inline $clients *name=$cname*]
    if {![regexp rbs=(\[a-zA-Z0-9-\]+) $c - rbufsize]} {
        error "field rbus not found in $c"
    }
    puts $c
    return $rbufsize
}

start_server {tags {"replybufsize"}} {
    
    test {verify reply buffer limits} {
        if {$::accurate} {
            # Create a simple idle test client
            variable tc [redis_client]
            $tc client setname test_client
            
            # make sure the client is idle for 5 seconds to make it shrink the reply buffer
            wait_for_condition 5 1000 {
                [get_reply_buffer_size test_client] >= 1024 && [get_reply_buffer_size test_client] < 2046
            } else {
                set rbs [get_reply_buffer_size test_client]
                fail "reply buffer of idle client is $rbs after 5 seconds"
            }
            
            r set bigval [string repeat x 32768]
            
            wait_for_condition 50 100 {
                [$tc get bigval ; get_reply_buffer_size test_client] >= 16384 && [get_reply_buffer_size test_client] < 32768
            } else {
                set rbs [get_reply_buffer_size test_client]
                fail "reply buffer of busy client is $rbs after 5 seconds"
            }
            
            $tc close
        } 
    }
}
    