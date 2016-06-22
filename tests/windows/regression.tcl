proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

start_server {tags {"regression"}} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Set the AUTH password
        $master config set requirepass mypwd
        $slave config set masterauth mypwd

        # Start the replication process...
        $slave slaveof $master_host $master_port

        test {Slave is able to sync with master when AUTH is on} {
            wait_for_condition 50 100 {
                [log_file_matches $slave_log "*Finished with success*"]
            } else {
                fail "Slave is not able to sync with master when AUTH is on"
            }
        }
    }
}

start_server {tags {"regression"}} {
    set A [srv 0 client]
    set A_host [srv 0 host]
    set A_port [srv 0 port]
 
    set max_clients 5
    set arg [format {overrides {maxclients %d requirepass foobar}} $max_clients]
    start_server $arg {
        set B [srv 0 client]
        set B_host [srv 0 host]
        set B_port [srv 0 port]
        
        # Make server A a slave of server B
        $A slaveof $B_host $B_port
        
        test {Master should release the connection after an AUTH failure from a Slave} {
            # Verify A changed role
            wait_for_condition 50 100 {
                [lindex [$A role] 0] eq {slave}
            } else {
                fail {"Can't turn the instance into a slave"}
            }        

            # Wait for multiple connections from A to B
            after 5000
            
            # List all the clients connected to B
            r auth foobar
            set client_count 0
            set client_list [r client list]
            foreach item $client_list {
                if ([string match "id=*" $item]) {
                    incr client_count
                }
            }
            assert {$client_count < $max_clients}
        } 
    }
}