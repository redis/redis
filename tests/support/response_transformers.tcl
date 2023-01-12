package require Tcl 8.5

namespace eval response_transformers {}

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

proc transfrom_map_or_tuple_array_to_flat_array {argv response} {
    set flatarray {}
    foreach pair $response {
        lappend flatarray {*}$pair
    }
    return $flatarray
}

proc transfrom_zset_withscores_command {argv response} {
    foreach ele $argv {
        if {[string compare -nocase $ele "WITHSCORES"] == 0} {
            return [transfrom_map_or_tuple_array_to_flat_array $argv $response]
        }
    }
    return $response
}

proc transfrom_zpopmin_zpopmax {argv response} {
    if {[llength $argv] == 3} {
        return [transfrom_map_or_tuple_array_to_flat_array $argv $response]
    }
    return $response
}

set ::trasformer_funcs {
    XREAD transfrom_map_to_tupple_array
    XREADGROUP transfrom_map_to_tupple_array
    HRANDFIELD transfrom_map_or_tuple_array_to_flat_array
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
