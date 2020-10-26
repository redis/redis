proc redisbenchmark {host port {opts {}}} {
    set cmd [list src/redis-benchmark -h $host -p $port]
    lappend cmd {*}$opts
    return $cmd
}
