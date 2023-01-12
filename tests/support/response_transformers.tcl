package require Tcl 8.5

namespace eval response_transformers {}

# Transform a map response into an array of tuples (tuple = array with 2 elements)
# Used for XREAD[GROUP] 
proc transfrom_map_to_tupple_array {argv response} {
    set tuparray {}
    foreach {key val} $response {
        set tmp {}
        lappend tmp $key
        lappend tmp $val
        lappend tuparray $tmp
    }
    return $tuparray
}

# Transform an array of tuples to a flat array
# e.g. HRANDFIELD returns an array of tuples in RESP3, but a flat array in RESP2
proc transfrom_tuple_array_to_flat_array {argv response} {
    set flatarray {}
    foreach pair $response {
        lappend flatarray {*}$pair
    }
    return $flatarray
}

# With some zset commands, we only need to transfrom the response if the request had WITHSCORES
# (otherwise the returned reponse is a flat array in both RESPs)
proc transfrom_zset_withscores_command {argv response} {
    foreach ele $argv {
        if {[string compare -nocase $ele "WITHSCORES"] == 0} {
            return [transfrom_tuple_array_to_flat_array $argv $response]
        }
    }
    return $response
}

# With ZPOPMIN/ZPOPMAX, we only need to transfrom the response if the request had COUNT (3rd arg)
# (otherwise the returned reponse is a flat array in both RESPs)
proc transfrom_zpopmin_zpopmax {argv response} {
    if {[llength $argv] == 3} {
        return [transfrom_tuple_array_to_flat_array $argv $response]
    }
    return $response
}

set ::trasformer_funcs {
    XREAD transfrom_map_to_tupple_array
    XREADGROUP transfrom_map_to_tupple_array
    HRANDFIELD transfrom_tuple_array_to_flat_array
    ZRANDMEMBER transfrom_zset_withscores_command
    ZRANGE transfrom_zset_withscores_command
    ZRANGEBYSCORE transfrom_zset_withscores_command
    ZRANGEBYLEX transfrom_zset_withscores_command
    ZREVRANGE transfrom_zset_withscores_command
    ZREVRANGEBYSCORE transfrom_zset_withscores_command
    ZREVRANGEBYLEX transfrom_zset_withscores_command
    ZUNION transfrom_zset_withscores_command
    ZDIFF transfrom_zset_withscores_command
    ZINTER transfrom_zset_withscores_command
    ZPOPMIN transfrom_zpopmin_zpopmax
    ZPOPMAX transfrom_zpopmin_zpopmax
}

proc ::response_transformers::transform_response_if_needed {id argv response} {
    if {![::redis::should_transform_to_resp2 $id] || $::redis::readraw($id)} {
        return $response
    }

    set key [string toupper [lindex $argv 0]]
    if {![dict exists $::trasformer_funcs $key]} {
        return $response
    }

    set transform [dict get $::trasformer_funcs $key]

    return [$transform $argv $response]
}
