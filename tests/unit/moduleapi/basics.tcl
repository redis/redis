set testmodule [file normalize tests/modules/basics.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    r module unload test
}
