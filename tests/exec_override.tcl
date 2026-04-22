rename exec old_exec
proc exec {args} {
    set cmd [lindex $args 0]
    if {$cmd eq "cp"} {
        set src ""
        set dst ""
        foreach arg [lrange $args 1 end] {
            if {$arg ne "-f"} {
                if {$src eq ""} { set src $arg } else { set dst $arg }
            }
        }
        catch {file copy -force $src $dst}
        return ""
    } elseif {$cmd eq "rm"} {
        foreach arg [lrange $args 1 end] {
            if {$arg ne "-rf" && $arg ne "-f" && $arg ne "-r"} {
                catch {file delete -force $arg}
            }
        }
        return ""
    } elseif {$cmd eq "cat"} {
        set filename [lindex $args end]
        if {[catch {
            set fp [open $filename r]
            set data [read $fp]
            close $fp
        }]} { set data "" }
        return $data
    } elseif {$cmd eq "head"} {
        set filename [lindex $args end]
        if {[catch {
            set fp [open $filename r]
            set data [gets $fp]
            close $fp
        }]} { set data "" }
        return $data
    } elseif {$cmd eq "tail"} {
        set filename ""
        for {set i 0} {$i < [llength $args]} {incr i} {
            if {[lindex $args $i] eq "<"} {
                set filename [lindex $args [expr {$i + 1}]]
            }
        }
        if {$filename eq ""} { set filename [lindex $args end] }
        if {[catch {
            set fp [open $filename r]
            set data [read $fp]
            close $fp
            set lines [split [string trimright $data "\n"] "\n"]
            set data [join [lrange $lines end-20 end] "\n"]
        }]} { set data "" }
        return $data
    } elseif {$cmd eq "uname"} {
        return "Windows"
    } elseif {$cmd eq "kill"} {
        set pid [lindex $args end]
        catch {old_exec taskkill /F /PID $pid}
        return ""
    } else {
        return [old_exec {*}$args]
    }
}