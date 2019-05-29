set testmodule [file normalize src/modules/testmodule.so]


# TEST.CTXFLAGS requires RDB to be disabled, so override save file
start_server {tags {"modules"} overrides {save ""}} {
    r module load $testmodule

    test {TEST.IT runs successfully} {
        r test.it
    } "ALL TESTS PASSED"

    r module unload test
}
