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

    # Read symlink to get info about the file descriptor. This can be a real
    # file name or an arbitrary string that identifies the fd.
    if { [catch {set fdlink [file readlink "/proc/self/fd/$fd"]} err] } {
        continue
    }

    # See if grandparent process has the same fd and info and skip if it does.
    if { ! [catch {set gp_fdlink [file readlink "/proc/$grandparent_pid/fd/$fd"]} err] } {
        if {$gp_fdlink == $fdlink} {
            continue
        }
    }

    lappend leaked_fds [list $fd $fdlink]
}

# Produce report only if we found leaks
if {[llength $leaked_fds] > 0} {
    set fd [open $outfile "w"]
    puts $fd [join $leaked_fds "\n"]
    close $fd
}
