set ::aof_manifest_name "appendonly.aof.manifest"
set ::aof_meta_temp_filename "temp_appendonly.aof.manifest"
set ::default_aof_basename "appendonly.aof"
set ::base_aof_sufix ".base"
set ::incr_aof_sufix ".incr"
set ::aof_format_suffix ".aof"
set ::rdb_format_suffix ".rdb"

proc get_full_path {dir filename} {
    set _ [format "%s/appendonly.aof/%s" $dir $filename]
}

proc get_redis_dir {} {
    set config [srv config]
    set _ [dict get $config "dir"]
}

proc check_file_exist {dir filename} {
    set file_path [get_full_path $dir $filename]
    return [file exists $file_path]
}

proc del_file {dir filename} {
    set file_path [get_full_path $dir $filename]
    catch {exec rm -rf $file_path}
}

proc get_cur_base_aof_name {dir} {
    set meta_path [get_full_path $dir $::aof_manifest_name]
    set fp [open $meta_path r+]
    set lines {}
    while {1} {
        set line [gets $fp]
        if {[eof $fp]} {
           close $fp
           break;
        }

        lappend lines $line
    }

    set first_line [lindex $lines 0]
    set aofname [lindex [split $first_line " "] 1]
    set aoftype [lindex [split $first_line " "] 5]
    if { $aoftype eq "b" } {
        return $aofname
    }

    return ""
}

proc get_last_incr_aof_name {dir} {
    set meta_path [get_full_path $dir $::aof_manifest_name]
    set fp [open $meta_path r+]
    set lines {}
    while {1} {
        set line [gets $fp]
        if {[eof $fp]} {
           close $fp
           break;
        }

        lappend lines $line
    }

    set len [llength $lines]
    set last_line [lindex $lines [expr $len - 1]]
    set aofname [lindex [split $last_line " "] 1]
    set aoftype [lindex [split $last_line " "] 5]
    if { $aoftype eq "i" } {
        return $aofname
    }

    return ""
}

proc assert_aof_manifest_content {dir content} {
    set meta_path [get_full_path $dir $::aof_manifest_name]
    set fp [open $meta_path r+]
    set lines {}
    while {1} {
        set line [gets $fp]
        if {[eof $fp]} {
           close $fp
           break;
        }

        lappend lines $line
    }

    assert_equal [llength $lines] [llength $content]

    for { set i 0 } { $i < [llength $lines] } {incr i} {
        assert_equal [lindex $lines $i] [lindex $content $i]
    }
}

proc clean_aof_persistence {dir basename} {
    set path [format "%s/%s" $dir $basename]
    catch {eval exec rm -rf [glob $path]}
}

proc append_to_manifest {str} {
    upvar fp fp
    puts -nonewline $fp $str
}

proc create_aof_manifest {aof_manifest_path code} {
    upvar fp fp 
    set fp [open $aof_manifest_path w+]
    uplevel 1 $code
    close $fp
}

proc append_to_aof {str} {
    upvar fp fp
    puts -nonewline $fp $str
}

proc create_aof {aof_path code} {
    upvar fp fp 
    set fp [open $aof_path w+]
    uplevel 1 $code
    close $fp
}

proc create_aof_dir {dir_path} {
    file mkdir $dir_path
}

proc start_server_aof {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    set srv [start_server [list overrides $config]]
    uplevel 1 $code
    kill_server $srv
}
