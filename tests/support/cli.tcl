proc rediscli_tls_config {testsdir} {
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

# Returns command line for executing redis-cli
proc rediscli {host port {opts {}}} {
    if {[info exists ::env(REDIS_BIN_DIR)]} { set binpath [list $::env(REDIS_BIN_DIR)/redis-cli.exe] } else { set binpath [list src/redis-cli] }
    set cmd [list {*}$binpath -h $host -p $port]
    lappend cmd {*}[rediscli_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

# Returns command line for executing redis-cli with a unix socket address
proc rediscli_unixsocket {unixsocket {opts {}}} {
    if {[info exists ::env(REDIS_BIN_DIR)]} { set binpath [list $::env(REDIS_BIN_DIR)/redis-cli.exe] } else { set binpath [list src/redis-cli] }
    return [list {*}$binpath -s $unixsocket {*}$opts]
}

# Run redis-cli with specified args on the server of specified level.
# Returns output broken down into individual lines.
proc rediscli_exec {level args} {
    set cmd [rediscli [srv $level host] [srv $level port] $args]
    set fd [open "|$cmd" "r"]
    set ret [lrange [split [read $fd] "\n"] 0 end-1]
    close $fd

    return $ret
}
