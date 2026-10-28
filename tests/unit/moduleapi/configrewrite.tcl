set rewrite_module [file normalize tests/modules/moduleconfigs.so]
set rewrite_module_two [file normalize tests/modules/moduleconfigstwo.so]

set rewrite_dir [file normalize [tmpdir config-rewrite-sources]]
set rewrite_leaf $rewrite_dir/leaf.conf
set rewrite_include $rewrite_dir/include.conf
set fd [open $rewrite_leaf w]
puts $fd "loadmodule \"$rewrite_module\""
puts $fd {moduleconfigs.string included-value}
close $fd
set fd [open $rewrite_include w]
puts $fd "include \"$rewrite_dir/leaf*.conf\""
close $fd

start_server [list tags {modules external:skip} config_lines [list include "\"$rewrite_include\""] \
    args [list --loadmodule $rewrite_module_two]] {
    test {CONFIG REWRITE preserves legacy module serialization by default} {
        assert_equal no [lindex [r config get experimental-config-rewrite] 1]
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        # Legacy mode serializes modules from both includes and CLI arguments.
        assert_equal 2 [regexp -all -line {^loadmodule } $contents]
        assert {[string first "loadmodule $rewrite_module\n" $contents] >= 0}
        assert {[string first "loadmodule $rewrite_module_two\n" $contents] >= 0}
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list include "\"$rewrite_include\""]] {
    test {CONFIG REWRITE does not copy nested include modules, even on repeated rewrites} {
        # These values belong to nested include files and must not be copied
        # into the main file, even when rewrite is repeated.
        set before [read_file $rewrite_leaf]
        r config rewrite
        assert_equal 0 [regexp -line {^loadmodule } [read_file [srv 0 config_file]]]
        assert_equal $before [read_file $rewrite_leaf]
        assert_equal 0 [regexp -line {^moduleconfigs.string } [read_file [srv 0 config_file]]]
        assert_equal included-value [lindex [r config get moduleconfigs.string] 1]
        restart_server 0 true false
        assert {[lsearch -exact [lmap m [r module list] {dict get $m name}] moduleconfigs] >= 0}
    }

    test {CONFIG REWRITE persists a runtime module alongside an included module} {
        # Persist only the module loaded at runtime; keep the included module
        # owned by its original file across rewrites and restarts.
        # The included module is already active before this runtime load.
        r module load $rewrite_module_two arg1 arg2
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        # The main file should contain only the runtime module's load directive.
        assert_equal 1 [regexp -all -line {^loadmodule } $contents]
        assert_equal -1 [string first $rewrite_module $contents]
        assert {[string first $rewrite_module_two $contents] >= 0}

        # After restart, one module comes from the include and the other from
        # the rewritten main file.
        restart_server 0 true false
        assert {[lsearch -exact [lmap m [r module list] {dict get $m name}] configs] >= 0}

        # Unloading the runtime module and rewriting should remove its line;
        # the included module must still load after another restart.
        r module unload configs
        r config rewrite
        restart_server 0 true false
        assert {[lsearch -exact [lmap m [r module list] {dict get $m name}] moduleconfigs] >= 0}
    }

    test {CONFIG REWRITE rereads included files to determine current values} {
        # A missing file in the include tree makes the rewrite baseline
        # unavailable; restore it and verify rewrite succeeds afterward.
        file rename $rewrite_include $rewrite_include.saved
        assert_error {*} {r config rewrite}
        file delete $rewrite_include
        file rename $rewrite_include.saved $rewrite_include
        assert_equal OK [r config rewrite]
        assert_equal 0 [regexp -line {^loadmodule } [read_file [srv 0 config_file]]]
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} args [list --loadmodule $rewrite_module]] {
    test {CONFIG REWRITE excludes a CLI module but persists a runtime module} {
        # The original CLI module remains on the launch command; only the
        # module loaded at runtime should be persisted in main.conf.
        r module load $rewrite_module_two
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        assert_equal -1 [string first $rewrite_module $contents]
        assert {[string first $rewrite_module_two $contents] >= 0}
        # Start with the exact generated module directive and the original CLI.
        regexp -line {^loadmodule (.*)$} $contents -> module_args
        start_server [list overrides {experimental-config-rewrite yes} config_lines [list loadmodule $module_args] args [list --loadmodule $rewrite_module]] {
            set names [lmap m [r module list] {dict get $m name}]
            assert {[lsearch -exact $names moduleconfigs] >= 0}
            assert {[lsearch -exact $names configs] >= 0}
        }
        # Restart helper omits CLI arguments: only the persisted module should load.
        restart_server 0 true false
        set names [lmap m [r module list] {dict get $m name}]
        assert {[lsearch -exact $names configs] >= 0}
        assert_equal -1 [lsearch -exact $names moduleconfigs]
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list loadmodule "  '$rewrite_module'"]] {
    test {CONFIG REWRITE keeps existing main-file module serialization and unload cleanup} {
        r config rewrite
        assert {[string first "loadmodule $rewrite_module\n" [read_file [srv 0 config_file]]] >= 0}
        r module unload moduleconfigs
        r config rewrite
        assert_equal 0 [regexp -line {^loadmodule } [read_file [srv 0 config_file]]]
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list dir $rewrite_dir include leaf.conf]] {
    test {CONFIG REWRITE skips modules loaded from relative includes} {
        # Relative include resolution must use the startup dir as loading does.
        r config rewrite
        assert_equal 0 [regexp -line {^loadmodule } [read_file [srv 0 config_file]]]
        restart_server 0 true false
        # The module should still load from the relative include after restart,
        # even though CONFIG REWRITE did not copy its directive to main.conf.
        assert {[lsearch -exact [lmap m [r module list] {dict get $m name}] moduleconfigs] >= 0}
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list loadmodule "\"$rewrite_module\""] args {--unprefix.string-alias cli-value}] {
    test {CONFIG REWRITE tracks CLI source for late-registered module configs and aliases} {
        # The CLI option is parsed before the module registers it, so its value
        # and CLI source must be queued until the module is loaded.
        # Reading by the canonical name confirms the queued alias was applied.
        assert_equal cli-value [lindex [r config get unprefix-string] 1]

        # Rewrite uses the canonical option name, but must not persist a value
        # that originated from the CLI.
        r config rewrite
        assert_equal 0 [regexp -line {^unprefix-string } [read_file [srv 0 config_file]]]

        # Changing the CLI-sourced option through its alias must not make it
        # eligible for CONFIG REWRITE persistence.
        r config set unprefix.string-alias changed-cli-value
        r config rewrite
        assert_equal 0 [regexp -line {^unprefix-string } [read_file [srv 0 config_file]]]

        # The restart helper omits the CLI argument, so the option returns to
        # the module's default instead of either CLI-provided value.
        restart_server 0 true false
        assert_equal "secret unprefix" [lindex [r config get unprefix-string] 1]
    }
}

set module_value_include [file normalize [tmpfile module-config-values.conf]]
set fd [open $module_value_include w]
puts $fd {moduleconfigs.memory_numeric 2kb}
close $fd
start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list loadmodule "\"$rewrite_module\"" moduleconfigs.memory_numeric 2048]] {
    test {CONFIG REWRITE compares main-file module config values before rewriting} {
        set config_file [srv 0 config_file]
        # The main file stores this memory value as a byte count. Keep its line
        # so we can verify an equivalent getter value leaves it untouched.
        set initial_line [regexp -line -inline {^moduleconfigs\.memory_numeric .*$} [read_file $config_file]]
        r config rewrite
        assert_equal $initial_line [regexp -line -inline {^moduleconfigs\.memory_numeric .*$} [read_file $config_file]]

        # A runtime change must be serialized and survive a restart.
        r config set moduleconfigs.memory_numeric 4096
        r config rewrite
        assert_match {*moduleconfigs.memory_numeric 4kb*} [read_file $config_file]
        restart_server 0 true false
        assert_equal 4096 [lindex [r config get moduleconfigs.memory_numeric] 1]
    }
}

start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list loadmodule "\"$rewrite_module\"" include "\"$module_value_include\""]] {
    test {CONFIG REWRITE compares unchanged included module config values} {
        # An unchanged module option from an include must not be copied to the
        # main file.
        assert_equal 2048 [lindex [r config get moduleconfigs.memory_numeric] 1]
        r config rewrite
        assert_equal 0 [regexp -line {^moduleconfigs.memory_numeric } [read_file [srv 0 config_file]]]
    }

    test {CONFIG REWRITE appends changed included module config values after includes} {
        # A changed include option is appended after includes so it wins on
        # restart, while the original include remains unchanged.
        r config set moduleconfigs.memory_numeric 4096
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        assert_equal 1 [regexp -all -line {^moduleconfigs.memory_numeric 4kb$} $contents]
        assert {[string last "include \"$module_value_include\"" $contents] < [string last {moduleconfigs.memory_numeric 4kb} $contents]}
        restart_server 0 true false
        assert_equal 4096 [lindex [r config get moduleconfigs.memory_numeric] 1]
    }
}

set queued_source_include [file normalize [tmpfile module-config-source.conf]]
set fd [open $queued_source_include w]
puts $fd {moduleconfigs.string include-value}
close $fd
start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} config_lines [list loadmodule "\"$rewrite_module\"" moduleconfigs.string main-value include "\"$queued_source_include\""] args {--moduleconfigs.string cli-value}] {
    test {Queued module config replacement updates value and source together} {
        # The module config values are queued until module registration. In
        # load order, MAIN sets main-value, INCLUDE overrides it, then CLI wins.
        assert_equal cli-value [lindex [r config get moduleconfigs.string] 1]

        # The final queued value and source must both be CLI: keep the original
        # MAIN line, but do not persist the CLI or INCLUDE value into main.conf.
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        assert_equal 1 [regexp -line {^moduleconfigs\.string main-value$} $contents]
        assert_equal 0 [regexp -line {^moduleconfigs\.string (include-value|cli-value)$} $contents]

        # The included source file remains untouched by CONFIG REWRITE.
        assert_equal "moduleconfigs.string include-value\n" [read_file $queued_source_include]
    }
}

set cli_include [file normalize [tmpfile cli-include.conf]]
set fd [open $cli_include w]
puts $fd "include \"$rewrite_leaf\""
close $fd
start_server [list tags {modules external:skip} overrides {experimental-config-rewrite yes} args [list --include $cli_include]] {
    test {CONFIG REWRITE skips module configs from CLI includes} {
        # CLI supplies only the include path. Directives read from the included
        # leaf file inherit CLI provenance, including its module load.
        assert_equal included-value [lindex [r config get moduleconfigs.string] 1]

        # Neither the module load nor its configuration is copied from the
        # CLI-controlled include into main.conf.
        r config rewrite
        set contents [read_file [srv 0 config_file]]
        assert_equal 0 [regexp -line {^loadmodule } $contents]
        assert_equal 0 [regexp -line {^moduleconfigs\.string } $contents]
        assert_equal "include \"$rewrite_leaf\"\n" [read_file $cli_include]
    }
}
