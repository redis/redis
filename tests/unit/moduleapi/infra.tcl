set testmodule [file normalize tests/modules/infotest.so]

test {modules config rewrite} {

    start_server {tags {"modules"}} {
        r module load $testmodule

        assert_equal [lindex [lindex [r module list] 0] 1] infotest

        r config rewrite
        restart_server 0 true false

        assert_equal [lindex [lindex [r module list] 0] 1] infotest

        assert_equal {OK} [r module unload infotest]

        r config rewrite
        restart_server 0 true false

        assert_equal [llength [r module list]] 0
    }
}
