set testmodule [file normalize tests/modules/cmdintrospection.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module command introspection via COMMAND" {
        set reply [r command info cmdintrospection.xadd]
        assert_equal $reply {{cmdintrospection.xadd -5 {} 0 0 0 {} {summary {Appends a new entry to a stream} complexity {O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.} since 5.0.0 group module history {{6.2 {Added the NOMKSTREAM option, MINID trimming strategy and the LIMIT option}}}}}}
    }
}
