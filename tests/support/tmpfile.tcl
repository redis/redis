set ::tmpcounter 0
set ::tmproot "./tests/tmp"
file mkdir $::tmproot

# returns a dirname unique to this process to write to
proc tmpdir {basename} {
    if {$::diskstore} {
        # For diskstore we want to use the same dir again and again
        # otherwise everything is too slow.
        set dir [file join $::tmproot $basename.diskstore]
    } else {
        set dir [file join $::tmproot $basename.[pid].[incr ::tmpcounter]]
    }
    file mkdir $dir
    set _ $dir
}

# return a filename unique to this process to write to
proc tmpfile {basename} {
    file join $::tmproot $basename.[pid].[incr ::tmpcounter]
}
