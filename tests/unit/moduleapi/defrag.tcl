set testmodule [file normalize tests/modules/defragtest.so]

start_server {tags {"modules"} overrides {{save ""}}} {
    r module load $testmodule 10000
    r config set hz 100
    r config set active-defrag-ignore-bytes 1
    r config set active-defrag-threshold-lower 0
    r config set active-defrag-cycle-min 99

    # try to enable active defrag, it will fail if redis was compiled without it
    catch {r config set activedefrag yes} e
    if {[r config get activedefrag] eq "activedefrag yes"} {

        test {Module defrag: simple key defrag works} {
            r frag.create key1 1 1000 0

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_attempts] > 0}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_resumes]
        }

        test {Module defrag: late defrag with cursor works} {
            r flushdb
            r frag.resetstats

            # key can only be defragged in no less than 10 iterations
            # due to maxstep
            r frag.create key2 10000 100 1000

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_resumes] > 10}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_wrong_cursor]
        }

        test {Module defrag: global defrag works} {
            r flushdb
            r frag.resetstats

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_global_attempts] > 0}
        }
    }
}
