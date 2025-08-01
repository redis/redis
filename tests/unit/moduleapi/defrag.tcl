set testmodule [file normalize tests/modules/defragtest.so]

start_server {tags {"modules"} overrides {{save ""}}} {
    r module load $testmodule
    r config set hz 100
    r config set active-defrag-ignore-bytes 1
    r config set active-defrag-threshold-lower 0
    r config set active-defrag-cycle-min 99

    # try to enable active defrag, it will fail if redis was compiled without it
    catch {r config set activedefrag yes} e
    if {[r config get activedefrag] eq "activedefrag yes"} {

        test {Module defrag: simple key defrag works} {
            r config set activedefrag no
            wait_for_condition 100 50 {
                [s active_defrag_running] eq 0
            } else {
                fail "Unable to wait for active defrag to stop"
            }

            r flushdb
            r frag.resetstats
            r frag.create key1 1 1000 0

            r config set activedefrag yes
            wait_for_condition 200 50 {
                [getInfoProperty [r info defragtest_stats] defragtest_defrag_ended] > 0
            } else {
                fail "Unable to wait for a complete defragmentation cycle to finish"
            }

            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_attempts] > 0}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_resumes]
            assert_morethan [getInfoProperty $info defragtest_datatype_raw_defragged] 0
            assert_morethan [getInfoProperty $info defragtest_defrag_started] 0
            assert_morethan [getInfoProperty $info defragtest_defrag_ended] 0
        } {} {tsan:skip}

        test {Module defrag: late defrag with cursor works} {
            r config set activedefrag no
            wait_for_condition 100 50 {
                [s active_defrag_running] eq 0
            } else {
                fail "Unable to wait for active defrag to stop"
            }

            r flushdb
            r frag.resetstats

            # key can only be defragged in no less than 10 iterations
            # due to maxstep
            r frag.create key2 10000 100 1000

            r config set activedefrag yes
            wait_for_condition 200 50 {
                [getInfoProperty [r info defragtest_stats] defragtest_defrag_ended] > 0
            } else {
                fail "Unable to wait for a complete defragmentation cycle to finish"
            }

            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_resumes] > 10}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_wrong_cursor]
            assert_morethan [getInfoProperty $info defragtest_datatype_raw_defragged] 0
            assert_morethan [getInfoProperty $info defragtest_defrag_started] 0
            assert_morethan [getInfoProperty $info defragtest_defrag_ended] 0
        } {} {tsan:skip}

        test {Module defrag: global defrag works} {
            r config set activedefrag no
            wait_for_condition 100 50 {
                [s active_defrag_running] eq 0
            } else {
                fail "Unable to wait for active defrag to stop"
            }

            r flushdb
            r frag.resetstats
            r frag.create_frag_global 50000
            r config set activedefrag yes

            wait_for_condition 200 50 {
                [getInfoProperty [r info defragtest_stats] defragtest_defrag_ended] > 0
            } else {
                fail "Unable to wait for a complete defragmentation cycle to finish"
            }

            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_global_strings_attempts] > 0}
            assert {[getInfoProperty $info defragtest_global_dicts_attempts] > 0}
            assert {[getInfoProperty $info defragtest_global_dicts_defragged] > 0}
            assert {[getInfoProperty $info defragtest_global_dicts_items_defragged] > 0}
            assert_morethan [getInfoProperty $info defragtest_defrag_started] 0
            assert_morethan [getInfoProperty $info defragtest_defrag_ended] 0
            assert_morethan [getInfoProperty $info defragtest_global_dicts_resumes] [getInfoProperty $info defragtest_defrag_ended]
            assert_morethan [getInfoProperty $info defragtest_global_subdicts_resumes] [getInfoProperty $info defragtest_defrag_ended]
        } {} {tsan:skip}
    }
}
