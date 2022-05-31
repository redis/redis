

start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    set host [srv 0 host]
    set port [srv 0 port]
    test {swap out string} {
        set load_handler [start_run_load  $host $port 5 {
            randpath {
                $r set k v
            } {
                $r get k 
            } {
                $r evict k
            } {
                $r del k
            }
        }]
        set load_handler2 [start_run_load  $host $port 5 {
            randpath {
                $r set k v2
            } {
                $r get k 
            } {
                $r evict k
            } {
                $r del k
            }
        }]
        after 5000
        stop_write_load $load_handler
        stop_write_load $load_handler2
        wait_load_handlers_disconnected
        r info keyspace
    }
} 




start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    set host [srv 0 host]
    set port [srv 0 port]
    test {swap out string} {
        set load_handler [start_run_load  $host $port 5 {
            randpath {
                $r hset h k1 v1 
            } {
                $r hget h k1
            } {
                $r evict k
            } {
                $r del k
            } {
                $r hdel h k1
            }
        }]
        set load_handler2 [start_run_load  $host $port 5 {
            randpath {
                $r hset h k1 v2 
            } {
                $r hget h k1
            } {
                $r evict h
            } {
                $r del h
            } {
                $r hdel h k1
            }
        }]
        after 5000
        stop_write_load $load_handler
        stop_write_load $load_handler2
        wait_load_handlers_disconnected
        r info keyspace
    }
} 
