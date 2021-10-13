set testmodule [file normalize tests/modules/container.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module subcommands via COMMAND" {
        set reply [r command info container.bitarray]
        set subcmds [lindex [lindex $reply 0] 8]
        assert_equal [lsort $subcmds] {{get -2 {} 1 1 1 {} {{flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}} {}} {set -2 {} 1 1 1 {} {{flags write begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}} {}}}
    }

    test "Module pure-container command fails on arity error" {
        catch {r container.bitarray} e
        set e
    } {*wrong number of arguments *}
}
