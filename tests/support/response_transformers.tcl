package require Tcl 8.5

namespace eval response_transformers {}

proc test_expects_resp3 {id} {
    return $::redis::testing_resp3($id)
}

# when forcing resp3 some tests that rely on resp2 can fail, so we have to translate the resp3 response to resp2
proc should_transform_to_resp2 {id} {
    return [expr {$::force_resp3 && [test_expects_resp3 $id] == 0}]
}

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

proc transfrom_withscores_command {argv response} {
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
    ZRANDMEMBER transfrom_withscores_command
    ZRANGE transfrom_withscores_command
    ZRANGEBYSCORE transfrom_withscores_command
    ZRANGEBYLEX transfrom_withscores_command
    ZREVRANGE transfrom_withscores_command
    ZREVRANGEBYSCORE transfrom_withscores_command
    ZREVRANGEBYLEX transfrom_withscores_command
    ZUNION transfrom_withscores_command
    ZDIFF transfrom_withscores_command
    ZINTER transfrom_withscores_command
    ZPOPMIN transfrom_zpopmin_zpopmax
    ZPOPMAX transfrom_zpopmin_zpopmax
}

proc ::response_transformers::transform_response_if_needed {id argv response} {
    if {![should_transform_to_resp2 $id] || $::redis::readraw($id)} {
        return $response
    }

    set key [string toupper [lindex $argv 0]]
    if {![dict exists $::trasformer_funcs $key]} {
        return $response
    }

    set transform [dict get $::trasformer_funcs $key]

    return [$transform $argv $response]
}
