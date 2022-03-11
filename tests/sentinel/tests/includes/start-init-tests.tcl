test "(start-init) Flush config and compare rewrite config file lines" {
    foreach_sentinel_id id {
        assert_match "OK" [S $id SENTINEL FLUSHCONFIG]
        set file1 ../tests/includes/sentinel.conf
        set file2 [file join "sentinel_${id}" "sentinel.conf"] 
        set fh1 [open $file1 r]
        set fh2 [open $file2 r]
        while {[gets $fh1 line1]} {
            if {[gets $fh2 line2]} {
                assert [string equal $line1 $line2]
            } else {
                fail "sentinel config file rewrite sequence changed"
            }
        }
        close $fh1
        close $fh2  
    }
}