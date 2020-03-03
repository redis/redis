#!/usr/bin/env tclsh

if {[llength $::argv] != 2 && [llength $::argv] != 3} {
    puts "Usage: $::argv0 <branch> <version> \[<num-commits>\]"
    exit 1
}

set branch [lindex $::argv 0]
set ver [lindex $::argv 1]
if {[llength $::argv] == 3} {
    set count [lindex ::$argv 2]
} else {
    set count 100
}

set template {
================================================================================
Redis %ver%     Released %date%
================================================================================

Upgrade urgency <URGENCY>: <DESCRIPTION>
}

set template [string trim $template]
append template "\n\n"
set date [clock format [clock seconds]]
set template [string map [list %ver% $ver %date% $date] $template]

append template [exec git log $branch~$count..$branch "--format=format:%an in commit %h:%n %s" --shortstat]

#Older, more verbose version.
#
#append template [exec git log $branch~30..$branch "--format=format:+-------------------------------------------------------------------------------%n| %s%n| By %an, %ai%n+--------------------------------------------------------------------------------%nhttps://github.com/antirez/redis/commit/%H%n%n%b" --stat]

puts $template
