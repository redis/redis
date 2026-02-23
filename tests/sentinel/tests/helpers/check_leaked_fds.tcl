#!/usr/bin/env tclsh
#
# This script detects file descriptors that have leaked from a parent process.
#
# Our goal is to detect file descriptors that were opened by the parent and
# not cleaned up prior to exec(), but not file descriptors that were inherited
# from the grandparent which the parent knows nothing about. To do that, we
# look up every potential leak and try to match it against open files by the
# grandparent process.

# Get PID of parent process
proc get_parent_pid {_pid} {
    set fd [open "/proc/$_pid/status" "r"]
    set content [read $fd]
    close $fd

    if {[regexp {\nPPid:\s+(\d+)} $content _ ppid]} {
        return $ppid
    }

    error "failed to get parent pid"
}

# Read symlink to get info about the specified fd of the specified process.
# The result can be the file name or an arbitrary string that identifies it.
# When not able to read, an empty string is returned.
proc get_fdlink {_pid fd} {
    if { [catch {set fdlink [file readlink "/proc/$_pid/fd/$fd"]} err] } {
        return ""
    }
    return $fdlink
}

# Linux only
set os [exec uname]
if {$os != "Linux"} {
    puts "Only Linux is supported."
    exit 0
}

if {![info exists env(LEAKED_FDS_FILE)]} {
    puts "Missing LEAKED_FDS_FILE environment variable."
    exit 0
}

set outfile $::env(LEAKED_FDS_FILE)
set parent_pid [get_parent_pid [pid]]
set grandparent_pid [get_parent_pid $parent_pid]
set leaked_fds {}

# Look for fds that were directly inherited from our parent but not from
# our grandparent (tcl)
foreach fd [glob -tails -directory "/proc/self/fd" *] {
    # Ignore stdin/stdout/stderr
    if {$fd == 0 || $fd == 1 || $fd == 2} {
        continue
    }

    set fdlink [get_fdlink "self" $fd]
    if {$fdlink == ""} {
        continue
    }

    # We ignore fds that existed in the grandparent, or fds that don't exist
    # in our parent (Sentinel process).
    if {[get_fdlink $grandparent_pid $fd] == $fdlink ||
	[get_fdlink $parent_pid $fd] != $fdlink} {
        continue
    }

    lappend leaked_fds [list $fd $fdlink]
}

# Produce report only if we found leaks
if {[llength $leaked_fds] > 0} {
    set fd [open $outfile "w"]
    puts $fd [join $leaked_fds "\n"]
    close $fd
}
