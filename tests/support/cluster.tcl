# Tcl redis cluster client as a wrapper of redis.rb.
# Copyright (C) 2014 Salvatore Sanfilippo
# Released under the BSD license like Redis itself
#
# Example usage:
#
# set c [redis_cluster 127.0.0.1 6379 127.0.0.1 6380]
# $c set foo
# $c get foo
# $c close

package require Tcl 8.5
package provide redis_cluster 0.1

namespace eval redis_cluster {}
set ::redis_cluster::id 0
array set ::redis_cluster::start_nodes {}
array set ::redis_cluster::nodes {}
array set ::redis_cluster::slots {}

# List of "plain" commands, which are commands where the sole key is always
# the first argument.
set ::redis_cluster::plain_commands {
    get set setnx setex psetex append strlen exists setbit getbit
    setrange getrange substr incr decr rpush lpush rpushx lpushx
    linsert rpop lpop brpop llen lindex lset lrange ltrim lrem
    sadd srem sismember scard spop srandmember smembers sscan zadd
    zincrby zrem zremrangebyscore zremrangebyrank zremrangebylex zrange
    zrangebyscore zrevrangebyscore zrangebylex zrevrangebylex zcount
    zlexcount zrevrange zcard zscore zrank zrevrank zscan hset hsetnx
    hget hmset hmget hincrby hincrbyfloat hdel hlen hkeys hvals
    hgetall hexists hscan incrby decrby incrbyfloat getset move
    expire expireat pexpire pexpireat type ttl pttl persist restore
    dump bitcount bitpos pfadd pfcount
}

proc redis_cluster {nodes} {
    set id [incr ::redis_cluster::id]
    set ::redis_cluster::start_nodes($id) $nodes
    set ::redis_cluster::nodes($id) {}
    set ::redis_cluster::slots($id) {}
    set handle [interp alias {} ::redis_cluster::instance$id {} ::redis_cluster::__dispatch__ $id]
    $handle refresh_nodes_map
    return $handle
}

proc ::redis_cluster::__dispatch__ {id method args} {
    if {[info command ::redis_cluster::__method__$method] eq {}} {
        # Get the keys from the command.
        set keys [::redis_cluster::get_keys_from_command $method $args]
        if {$keys eq {}} {
            error "Redis command '$method' is not supported by redis_cluster."
        }

        # Resolve the keys in the corresponding hash slot they hash to.
        set slot [::redis_cluster::get_slot_from_keys $keys]
        if {$slot eq {}} {
            error "Invalid command: multiple keys not hashing to the same slot."
        }

        # Get the node mapped to this slot.
        set node_id [dict get $::redis_cluster::slots($id) $slot]
        if {$node_id eq {}} {
            error "No mapped node for slot $slot."
        }

        # Execute the command in the node we think is the slot owner.
        set node [dict get $::redis_cluster::nodes($id) $node_id]
        set link [dict get $node link]
        if {[catch {$link $method {*}$args} e]} {
            # TODO: trap redirection error
        }
        return $e
    } else {
        uplevel 1 [list ::redis_cluster::__method__$method $id $fd] $args
    }
}

proc ::redis_cluster::get_keys_from_command {cmd argv} {
    set cmd [string tolower $cmd]
    # Most Redis commands get just one key as first argument.
    if {[lsearch -exact $::redis_cluster::plain_commands $cmd] != -1} {
        return [list [lindex $argv 0]]
    }

    # Special handling for other commands
    switch -exact $cmd {
        mget {return $argv}
    }

    # All the other commands are not handled.
    return {}
}
