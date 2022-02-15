proc get_reply_buffer_size {cname} {

    set clients [split [string trim [r client list]] "\r\n"]
    set c [lsearch -inline $clients *name=$cname*]
    if {![regexp rbs=(\[a-zA-Z0-9-\]+) $c - rbufsize]} {
        error "field rbus not found in $c"
    }
    return $rbufsize
}

start_server {tags {"replybufsize"}} {
    
    test {idle client buffer maximum shrink to 1kib} {
        variable rbs 0
        
        # Create a simple idle test client
        variable tc [redis_client]
        $tc client setname test_client
        
        # make sure the client is idle for 5 seconds to make it shrink the reply buffer
        after 5000 {set rbs [get_reply_buffer_size test_client]} 
        vwait rbs
        
        assert_equal $rbs 1024
        
        $tc close
    }
    
    test {busy client buffer maximum enlarge to 16kib} {
        variable rbs 0
       
        r set bigval [string repeat x 16384]
        
        # Create a simple test client
        variable tc [redis_client]
        $tc client setname test_client
        
        # make sure the client is idle for 5 seconds to make it shrink the reply buffer
        after 5000 {set rbs [get_reply_buffer_size test_client]} 
        vwait rbs
        
        assert_equal $rbs 1024
        variable client_bussy 1
       
        after 5000 {set client_bussy 0}
       
        while { $client_bussy == 1 } {
            $tc get bigval
            after 100 {set rbs [get_reply_buffer_size test_client]}
            vwait rbs
        }
        assert_equal $rbs 16384     
        $tc close
    }
}
    