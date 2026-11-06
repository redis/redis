proc num_unique_secrets {num_nodes} {
    set secrets [list]
    for {set i 0} {$i < $num_nodes} {incr i} {
        lappend secrets [R $i debug internal_secret]
    }
    set num_secrets [llength [lsort -unique $secrets]]
    return $num_secrets
}

proc wait_for_secret_sync {maxtries delay num_nodes} {
    wait_for_condition $maxtries $delay {
        [num_unique_secrets $num_nodes] eq 1
    } else {
        fail "Failed waiting for secrets to sync"
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    test "Test internal secret sync" {
        wait_for_secret_sync 50 100 6
    }

    
    set first_shard_host [srv 0 host]
    set first_shard_port [srv 0 port]
    
    if {$::verbose} {
        puts {cluster internal secret:}
        puts [R 1 debug internal_secret]
    }

    test "Join a node to the cluster and make sure it gets the same secret" {
        start_server {tags {"external:skip"} overrides {cluster-enabled {yes}}} {
            r cluster meet $first_shard_host $first_shard_port
            wait_for_condition 50 100 {
                [r debug internal_secret] eq [R 1 debug internal_secret]
            } else {
                puts [r debug internal_secret]
                puts [R 1 debug internal_secret]
                fail "Secrets not match"
            }
        }
    }

    test "Join another cluster, make sure clusters sync on the internal secret" {
        start_server {tags {"external:skip"} overrides {cluster-enabled {yes}}} {
            set new_shard_host [srv 0 host]
            set new_shard_port [srv 0 port]
            start_server {tags {"external:skip"} overrides {cluster-enabled {yes}}} {
                r cluster meet $new_shard_host $new_shard_port
                wait_for_condition 50 100 {
                    [r debug internal_secret] eq [r -1 debug internal_secret]
                } else {
                    puts [r debug internal_secret]
                    puts [r -1 debug internal_secret]
                    fail "Secrets not match"
                }
                if {$::verbose} {
                    puts {new cluster internal secret:}
                    puts [r -1 debug internal_secret]
                }
                r cluster meet $first_shard_host $first_shard_port
                wait_for_secret_sync 50 100 8
                if {$::verbose} {
                    puts {internal secret after join to bigger cluster:}
                    puts [r -1 debug internal_secret]
                }
            }
        }
    }
}
