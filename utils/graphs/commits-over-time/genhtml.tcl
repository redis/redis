#!/usr/bin/env tclsh

# Load commits history as "sha1 unixtime".
set commits [exec git log unstable {--pretty="%H %at"}]
set raw_tags [exec git tag]

# Load all the tags that are about stable releases.
foreach tag $raw_tags {
    if {[string match v*-stable $tag]} {
        set tag [string range $tag 1 end-7]
        puts $tag
    }
    if {[regexp {^[0-9]+.[0-9]+.[0-9]+$} $tag]} {
        lappend tags $tag
    }
}

# For each tag, create a list of "name unixtime"
foreach tag $tags {
    set taginfo [exec git log $tag -n 1 "--pretty=\"$tag %at\""]
    set taginfo [string trim $taginfo {"}]
    lappend labels $taginfo
}

# For each commit, check the amount of code changed and create an array
# mapping the commit to the number of lines affected.
foreach c $commits {
    set stat [exec git show --oneline --numstat [lindex $c 0]]
    set linenum 0
    set affected 0
    foreach line [split $stat "\n"] {
        incr linenum
        if {$linenum == 1 || [string match *deps/* $line]} continue
        if {[catch {llength $line} numfields]} continue
        if {$numfields == 0} continue
        catch {
            incr affected [lindex $line 0]
            incr affected [lindex $line 1]
        }
    }
    set commit_to_affected([lindex $c 0]) $affected
}

set base_time [lindex [lindex $commits end] 1]
puts [clock format $base_time]

# Generate a graph made of HTML DIVs.
puts {<html>
<style>
.box {
    position:absolute;
    width:10px;
    height:5px;
    border:1px black solid;
    background-color:#44aa33;
    opacity: 0.04;
}
.label {
    position:absolute;
    background-color:#dddddd;
    font-family:helvetica;
    font-size:12px;
    padding:2px;
    color:#666;
    border:1px #aaa solid;
    border-radius: 5px;
}
#outer {
    position:relative;
    width:1500;
    height:500;
    border:1px #aaa solid;
}
</style>
<div id="outer">
}
foreach c $commits {
    set sha [lindex $c 0]
    set t [expr {([lindex $c 1]-$base_time)/(3600*24*2)}]
    set affected [expr $commit_to_affected($sha)]
    set left $t
    set height [expr {log($affected)*20}]
    puts "<div class=\"box\" style=\"left:$left; bottom:0; height:$height\"></div>"
}

set bottom -30
foreach l $labels {
    set name [lindex $l 0]
    set t [expr {([lindex $l 1]-$base_time)/(3600*24*2)}]
    set left $t
    if {$left < 0} continue
    incr bottom -20
    if  {$bottom == -210} {set bottom -30}
    puts "<div class=\"label\" style=\"left:$left; bottom:$bottom\">$name</div>"
}
puts {</div></html>}
