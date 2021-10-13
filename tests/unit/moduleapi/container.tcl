set testmodule [file normalize tests/modules/container.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module subcommands via COMMAND" {
        set reply [r command info container.config]
        set subcmds [lindex [lindex $reply 0] 8]
        assert_equal [lsort $subcmds] {{get -2 {} 0 0 0 {} {} {}} {set -2 {} 0 0 0 {} {} {}}}
    }

    test "Module pure-container command fails on arity error" {
        catch {r container.config} e
        set e
    } {*wrong number of arguments *}
}
