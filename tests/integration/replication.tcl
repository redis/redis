start_server default.conf {} {
    r set mykey foo
    
    start_server default.conf {} {
        test {Second server should have role master at first} {
            s role
        } {master}
        
        test {SLAVEOF should start with link status "down"} {
            r slaveof [srv -1 host] [srv -1 port]
            s master_link_status
        } {down}
        
        test {The role should immediately be changed to "slave"} {
            s role
        } {slave}

        wait_for_sync r
        test {Sync should have transferred keys from master} {
            r get mykey
        } {foo}
        
        test {The link status should be up} {
            s master_link_status
        } {up}
        
        test {SET on the master should immediately propagate} {
            r -1 set mykey bar
            r  0 get mykey
        } {bar}
    }
}
