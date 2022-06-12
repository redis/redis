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

proc object_is_dirty {r key} {
    set str [$r swap object $key]
    if { [swap_object_property $str value dirty] == 1 || [swap_object_property $str evict dirty] == 1 } {
        set _ 1
    } else {
        set _ 0
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

proc rocks_get_wholekey {r type key} {
    lindex [$r swap rio-get [$r swap encode-key $type $key]] 0
}

