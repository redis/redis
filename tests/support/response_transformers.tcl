# Tcl client library - used by the Redis test
#
# Copyright (C) 2009-Present, Redis Ltd.
# All Rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
#
# This file contains a bunch of commands whose purpose is to transform
# a RESP3 response to RESP2
# Why is it needed?
# When writing the reply_schema part in COMMAND DOCS we decided to use
# the existing tests in order to verify the schemas (see logreqres.c)
# The problem was that many tests were relying on the RESP2 structure
# of the response (e.g. HRANDFIELD WITHVALUES in RESP2: {f1 v1 f2 v2}
# vs. RESP3: {{f1 v1} {f2 v2}}).
# Instead of adjusting the tests to expect RESP3 responses (a lot of
# changes in many files) we decided to transform the response to RESP2
# when running with --force-resp3

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
proc transfrom_tuple_array_to_flat_array {argv response} {
    set flatarray {}
    foreach pair $response {
        lappend flatarray {*}$pair
    }
    return $flatarray
}

# With HRANDFIELD, we only need to transform the response if the request had WITHVALUES
# (otherwise the returned response is a flat array in both RESPs)
proc transfrom_hrandfield_command {argv response} {
    foreach ele $argv {
        if {[string compare -nocase $ele "WITHVALUES"] == 0} {
            return [transfrom_tuple_array_to_flat_array $argv $response]
        }
    }
    return $response
}

# With some zset commands, we only need to transform the response if the request had WITHSCORES
# (otherwise the returned response is a flat array in both RESPs)
proc transfrom_zset_withscores_command {argv response} {
    foreach ele $argv {
        if {[string compare -nocase $ele "WITHSCORES"] == 0} {
            return [transfrom_tuple_array_to_flat_array $argv $response]
        }
    }
    return $response
}

# With ZPOPMIN/ZPOPMAX, we only need to transform the response if the request had COUNT (3rd arg)
# (otherwise the returned response is a flat array in both RESPs)
proc transfrom_zpopmin_zpopmax {argv response} {
    if {[llength $argv] == 3} {
        return [transfrom_tuple_array_to_flat_array $argv $response]
    }
    return $response
}

set ::trasformer_funcs {
    XREAD transfrom_map_to_tupple_array
    XREADGROUP transfrom_map_to_tupple_array
    HRANDFIELD transfrom_hrandfield_command
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
