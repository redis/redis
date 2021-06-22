set testmodule [file normalize tests/modules/basics.so]


# TEST.CTXFLAGS requires RDB to be disabled, so override save file
start_server {tags {"modules"} overrides {save ""}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    r module unload test
}
