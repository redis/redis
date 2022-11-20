
start_server {tags {"swap string"}} {
    r config set swap-debug-evict-keys 0
    test {swap out string} {
        set host [srv 0 host]
        set port [srv 0 port]
        set load_handler [start_run_load  $host $port 0 100 {
            # random 2 command pipeline
            randpath {
                $r write [format_command set k v]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command flushdb]
            } 
            randpath {
                $r write [format_command set k v]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command flushdb]
            } 
            $r flush
        }]
        set load_handler2 [start_run_load  $host $port 0 100 {
            randpath {
                $r write [format_command set k v]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command flushdb]
            } 
            randpath {
                $r write [format_command set k v]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command flushdb]
            } 
            $r flush
        }]
        after 5000
        stop_write_load $load_handler
        stop_write_load $load_handler2
        wait_load_handlers_disconnected
        r info keyspace
    }
} 


start_server {tags {"swap hash"}} {
    r config set swap-debug-evict-keys 0
    test {swap hash} {
        set host [srv 0 host]
        set port [srv 0 port]
        set load_handler [start_run_load  $host $port 0 100 {
            # random 2 command pipeline
            randpath {
                $r write [format_command hset h k v k1 v1]
            } {
                $r write [format_command hget h k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command hdel h k]
            } {
                $r write [format_command flushdb]
            } 
            randpath {
                $r write [format_command hset h k v k1 v1]
            } {
                $r write [format_command hget h k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command hdel h k]
            } {
                $r write [format_command del h ]
            } {
                $r write [format_command flushdb]
            } 
            $r flush
            after 100
        }]
        set load_handler2 [start_run_load  $host $port 0 100 {
            randpath {
                $r write [format_command set k v k1 v1]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command hdel h k]
            }  {
                $r write [format_command del k]
            } {
                $r write [format_command flushdb]
            } 
            randpath {
                $r write [format_command set k v k1 v1]
            } {
                $r write [format_command get k]
            } {
                $r write [format_command swap.evict k]
            } {
                $r write [format_command del k]
            } {
                $r write [format_command hdel h k]
            } {
                $r write [format_command flushdb]
            } 
            $r flush
            after 100
        }]
        after 5000
        stop_write_load $load_handler
        stop_write_load $load_handler2
        wait_load_handlers_disconnected
        r info keyspace
    }
} 
