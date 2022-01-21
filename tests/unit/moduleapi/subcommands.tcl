set testmodule [file normalize tests/modules/subcommands.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module subcommands via COMMAND" {
        # Verify that module subcommands are displayed correctly in COMMAND
        set command_reply [r command info subcommands.bitarray]
        set first_cmd [lindex $command_reply 0]
        set subcmds_in_command [lsort [lindex $first_cmd 9]]
        assert_equal [lindex $subcmds_in_command 0] {subcommands.bitarray|get -2 module 1 1 1 {} {} {{flags {RO access} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}} {}}
        assert_equal [lindex $subcmds_in_command 1] {subcommands.bitarray|set -2 module 1 1 1 {} {} {{flags {RW update} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}} {}}

        # Verify that module subcommands are displayed correctly in COMMAND DOCS
        set docs_reply [r command docs subcommands.bitarray]
        set docs [dict create {*}[lindex $docs_reply 1]]
        set subcmds_in_cmd_docs [dict create {*}[dict get $docs subcommands]]
        assert_equal [dict get $subcmds_in_cmd_docs "subcommands.bitarray|get"] {summary {} since {} group module}
        assert_equal [dict get $subcmds_in_cmd_docs "subcommands.bitarray|set"] {summary {} since {} group module}
    }

    test "Module pure-container command fails on arity error" {
        catch {r subcommands.bitarray} e
        assert_match {*wrong number of arguments*} $e

        # Subcommands can be called
        assert_equal [r subcommands.bitarray get k1] {OK}
    }
}
