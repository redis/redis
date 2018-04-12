#!/usr/bin/env tclsh

if {[llength $::argv] != 2} {
    puts "Usage: $::argv0 <branch> <version>"
    exit 1
}

set branch [lindex $::argv 0]
set ver [lindex $::argv 1]

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

append template [exec git log $branch~30..$branch "--format=format:%an in commit %h:%n %s" --shortstat]

#Older, more verbose version.
#
#append template [exec git log $branch~30..$branch "--format=format:+-------------------------------------------------------------------------------%n| %s%n| By %an, %ai%n+--------------------------------------------------------------------------------%nhttps://github.com/antirez/redis/commit/%H%n%n%b" --stat]

puts $template
