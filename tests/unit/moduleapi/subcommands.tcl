set testmodule [file normalize tests/modules/subcommands.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module subcommands via COMMAND" {
        # Verify that module subcommands are displayed correctly in COMMAND
        set reply [r command info subcommands.bitarray]
        # create a dict for easy lookup
        unset -nocomplain mydict
        foreach {k v} [lindex [lindex $reply 0] 7] {
            dict append mydict $k $v
        }
        set subcmds [lsort [dict get $mydict subcommands]]
        assert_equal [lindex $subcmds 0] {get -2 module 1 1 1 {} {summary {} since {} group module key-specs {{flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}}}}
        assert_equal [lindex $subcmds 1] {set -2 module 1 1 1 {} {summary {} since {} group module key-specs {{flags write begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}}}}
    }

    test "Module pure-container command fails on arity error" {
        catch {r subcommands.bitarray} e
        assert_match {*wrong number of arguments*} $e

        # Subcommands can be called
        assert_equal [r subcommands.bitarray get k1] {OK}
    }
}
