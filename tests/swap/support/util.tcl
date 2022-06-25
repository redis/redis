proc press_enter_to_continue {{message "Hit Enter to continue ==> "}} {
    puts -nonewline $message
    flush stdout
    gets stdin
}

proc wait_keys_evicted r {
    wait_for_condition 50 100 {
        [string match {*keys=0,evicts=*} [$r info keyspace]]
    } else {
        fail "wait keys evict failed."
    }
}

proc swap_object_property {str section property} {
    if {[regexp ".*${section}:.*${property}=(\[^,\r\n\]*).*" $str match submatch]} {
        set _ $submatch
    } else {
        set _ ""
    }
}

proc object_is_big {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str value big] == 1 || [swap_object_property $str evict big] == 1 } {
        set _ 1
    } else {
        set _ 0
    }
}

proc keyspace_is_empty {r} {
    if {[regexp ".*db0.*" [$r info keyspace] match]} {
        set _ 0
    } else {
        set _ 1
    }
}

proc object_is_dirty {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str value dirty] == 1} {
        set _ 1
    } else {
        set _ 0
    }
}

proc object_is_cold {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str evict at] != "" } {
        set _ 1
    } else {
        set _ 0
    }
}

proc object_is_warm {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str meta len] > 0 } {
        set _ 1
    } else {
        set _ 0
    }
}

proc wait_key_cold {r key} {
    wait_for_condition 50 40 {
        [object_is_cold $r $key]
    } else {
        fail "wait $key cold failed."
    }
}

proc wait_key_warm {r key} {
    wait_for_condition 50 40 {
        [object_is_warm $r $key]
    } else {
        fail "wait $key warm failed."
    }
}

proc object_meta_version {r key} {
    set str [$r swap object $key]
    set meta_version [swap_object_property $str meta version]
    if {$meta_version != ""} {
        set _ $meta_version
    } else {
        set _ 0
    }
}

proc object_meta_len {r key} {
    set str [$r swap object $key]
    set meta_len [swap_object_property $str meta len]
    if {$meta_len != ""} {
        set _ $meta_len
    } else {
        set _ 0
    }
}

proc rocks_get_wholekey {r type key} {
    lindex [$r swap rio-get [$r swap encode-key $type $key]] 0
}

proc rocks_get_bighash {r version key subkey} {
    lindex [$r swap rio-get [$r swap encode-key h $version $key $subkey]] 0
}

proc get_info_property {r section line property} {
    set str [$r info $section]
    if {[regexp ".*${line}:\[^\r\n\]*${property}=(\[^,\r\n\]*).*" $str match submatch]} {
        set _ $submatch
    }
}

