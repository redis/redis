set testmodule [file normalize tests/modules/cmdintrospection.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module command introspection via COMMAND" {
        set reply [r command info cmdintrospection.xadd]
        assert_equal $reply {{cmdintrospection.xadd -2 {} 0 0 0 {} {summary {} complexity {} since {} group module history {{6.2 {Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option}}}}}}
    }
}
