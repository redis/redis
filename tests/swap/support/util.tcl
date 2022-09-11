proc press_enter_to_continue {{message "Hit Enter to continue ==> "}} {
    puts -nonewline $message
    flush stdout
    gets stdin
}

proc get_info_property {r section line property} {
    set str [$r info $section]
    if {[regexp ".*${line}:\[^\r\n\]*${property}=(\[^,\r\n\]*).*" $str match submatch]} {
        set _ $submatch
    }
}

proc swap_object_property {str section property} {
    if {[regexp ".*${section}:\[^\r\n]*${property}=(\[^,\r\n\]*).*" $str match submatch]} {
        set _ $submatch
    } else {
        set _ ""
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
    if {[swap_object_property $str value dirty] == 1} {
        set _ 1
    } else {
        set _ 0
    }
}

proc object_is_cold {r key} {
    set str [$r swap object $key]
    # puts "str: $str"
    # puts "value.at: [swap_object_property $str value at], cold_meta.object_type: [swap_object_property $str cold_meta object_type]"
    if { [swap_object_property $str value at] == "" && [swap_object_property $str cold_meta object_type] != "" } {
        set _ 1
    } else {
        set _ 0
    }
}

proc object_is_warm {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str value at] != "" && [swap_object_property $str hot_meta object_type] != ""} {
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

proc object_meta_len {r key} {
    set str [$r swap object $key]
    set meta_len [swap_object_property $str hot_meta len]
    if {$meta_len == ""} {
        set meta_len [swap_object_property $str cold_meta len]
    }
    if {$meta_len != ""} {
        set _ $meta_len
    } else {
        set _ -1
    }
}

proc rio_get_meta {r key} {
    lindex [$r swap rio-get meta [$r swap encode-meta-key $key ]] 0
}

proc rio_get_data {r key subkey} {
    lindex [$r swap rio-get data [$r swap encode-data-key $key $subkey]] 0
}

