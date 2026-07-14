set testmodule [file normalize tests/modules/croaring_collision.so]

if {$::tcl_platform(os) eq "Linux"} {
    start_server {tags {"modules external:skip"}} {
        r module load $testmodule

        test {vendored CRoaring symbols do not interpose module symbols} {
            assert_equal 1 [r croaring_collision.resolves-locally]
        }

        test {unload the CRoaring collision module} {
            assert_equal OK [r module unload croaring_collision]
        }
    }
}
