proc press_enter_to_continue {{message "Hit Enter to continue ==> "}} {
    puts -nonewline $message
    flush stdout
    gets stdin
}

proc start_run_load {host port seconds counter code} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/swap/helpers/gen_run_load.tcl $host $port $seconds $counter $::tls $::target_db $code &
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

proc keyspace_is_cold {r} {
    if {[get_info_property r keyspace db0 keys] == "0"} {
        set _ 1
    } else {
        set _ 0
    }
}

proc wait_keyspace_cold {r} {
    wait_for_condition 50 40 {
        [keyspace_is_cold $r]
    } else {
        fail "wait keyspace cold failed."
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
        set _ 0
    }
}

proc object_meta_version {r key} {
    if { [catch {$r swap object $key} e] } {
        set _ 0
    } else {
        set str [$r swap object $key]
        set meta_version [swap_object_property $str hot_meta version]
        if {$meta_version == ""} {
            set meta_version [swap_object_property $str cold_meta version]
        }
        if {$meta_version != ""} {
            set _ $meta_version
        } else {
            set _ 0
        }
    }
}

proc rio_get_meta {r key} {
    lindex [$r swap rio-get meta [$r swap encode-meta-key $key ]] 0
}

proc rio_get_data {r key version subkey} {
    lindex [$r swap rio-get data [$r swap encode-data-key $key $version $subkey]] 0
}

proc get_info {r section line} {
    set str [$r info $section]
    if {[regexp ".*${line}:(\[^\r\n\]*)\r\n" $str match submatch]} {
        set _ $submatch
    }
}

proc scan_all_keys {r} {
    set keys {}
    set cursor 0
    while {1} {
        set res [$r scan $cursor]
        set cursor [lindex $res 0]
        lappend keys {*}[split [lindex $res 1] " "]
        if {$cursor == 0} {
            break
        }
    }
    set _ $keys
}

proc data_conflict {type key subkey v1 v2} {
    if {$subkey ne ""} {
        assert_failed "\[data conflict\]\[$type\]\[$key\]\[$subkey\] '$v1' - '$v2'" ""
    } else {
        assert_failed "\[data conflict\]\[$type\]\[$key\] '$v1' - '$v2'" ""
    }
}

proc swap_data_comp {r1 r2} {
    assert_equal [$r1 dbsize] [$r2 dbsize]
    set keys [scan_all_keys $r1]
    foreach key $keys {
        set t [$r1 type {*}$key]
        set t2 [$r2 type {*}$key]
        if {$t != $t2} {
            assert_failed "key '$key' type mismatch '$t' - '$t2'"
        }
        switch $t {
            {string} {
                set v1 [$r1 get {*}$key]
                set v2 [$r2 get {*}$key]
                if {$v1 != $v2} {
                    data_conflict $t $key '' $v1 $v2
                }
            }
            {list} {
                set len [$r1 llen {*}$key]
                set len2 [$r2 llen {*}$key]
                if {$len != $len2} {
                    data_conflict $t $key '' 'LLEN:$len' 'LLEN:$len2'
                }
                for {set i 0} {$i < $len} {incr i} {
                    set v1 [$r1 lindex {*}$key $i]
                    set v2 [$r2 lindex {*}$key $i]
                    if {$v1 != $v2} {
                        data_conflict $t $key $i $v1 $v2
                    }
                }
            }
            {set} {
                set len [$r1 scard {*}$key]
                set len2 [$r2 scard {*}$key]
                if {$len != $len2} {
                    data_conflict $t $key '' 'SLEN:$len' 'SLEN:$len2'
                }
                set skeys [r smembers k1]
                foreach skey $skeys {
                    if {0 == [$r2 sismember $skey]} {
                        data_conflict $t $key $skey "1" "0"
                    }
                }
            }
            {zset} {
                set len [$r1 zcard {*}$key]
                set len2 [$r2 zcard {*}$key]
                if {$len != $len2} {
                    data_conflict $t $key '' 'SLEN:$len' 'SLEN:$len2'
                }
                set zcursor 0
                while 1 {
                    set res [$r1 zscan {*}$key $zcursor]
                    set zcursor [lindex $res 0]
                    set zdata [lindex $res 1]
                    set dlen [llength $zdata]
                    for {set i 0} {$i < $dlen} {incr i 2} {
                        set zkey [lindex $zdata $i]
                        set zscore [lindex $zdata [expr $i+1]]
                        set zscore2 [$r2 zscore {*}$key $zkey]
                        if {$zscore != $zscore2} {
                            data_conflict $t $key $zkey $zscore $zscore2
                        }
                    }
                    if {$zcursor == 0} {
                        break
                    }
                }
            }
            {hash} {
                set len [$r1 hlen {*}$key]
                set len2 [$r2 hlen {*}$key]
                if {$len != $len2} {
                    data_conflict $t $key '' 'HLEN:$len' 'HLEN:$len2'
                }
                set hkeys [$r1 hkeys {*}$key]
                foreach hkey $hkeys {
                    set v1 [$r1 hget {*}$key $hkey]
                    set v2 [$r2 hget {*}$key $hkey]
                    if {$v1 != $v2} {
                        data_conflict $t $key $hkey $v1 $v2
                    }
                }
            }
        }
    }
}
