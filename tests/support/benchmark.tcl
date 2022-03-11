proc redisbenchmark_tls_config {testsdir} {
    set tlsdir [file join $testsdir tls]
    set cert [file join $tlsdir client.crt]
    set key [file join $tlsdir client.key]
    set cacert [file join $tlsdir ca.crt]

    if {$::tls} {
        return [list --tls --cert $cert --key $key --cacert $cacert]
    } else {
        return {}
    }
}

proc redisbenchmark {host port {opts {}}} {
    set cmd [list src/redis-benchmark -h $host -p $port]
    lappend cmd {*}[redisbenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

proc redisbenchmarkuri {host port {opts {}}} {
    set cmd [list src/redis-benchmark -u redis://$host:$port]
    lappend cmd {*}[redisbenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

proc redisbenchmarkuriuserpass {host port user pass {opts {}}} {
    set cmd [list src/redis-benchmark -u redis://$user:$pass@$host:$port]
    lappend cmd {*}[redisbenchmark_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}
