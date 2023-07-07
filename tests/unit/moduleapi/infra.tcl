set testmodule [file normalize tests/modules/infotest.so]

test {modules config rewrite} {

    start_server {tags {"modules"}} {
        r module load $testmodule

        set modules [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $modules infotest] -1

        r config rewrite
        restart_server 0 true false

        set modules [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $modules infotest] -1

        assert_equal {OK} [r module unload infotest]

        r config rewrite
        restart_server 0 true false

        set modules [lmap x [r module list] {dict get $x name}]
        assert_equal [lsearch $modules infotest] -1
    }
}
