set testmodule [file normalize tests/modules/moduleconfigs.so]
set configaccessmodule [file normalize tests/modules/configaccess.so]

proc crm_read_config_file {} {
    set fd [open [srv 0 config_file] r]
    set content [read $fd]
    close $fd
    return $content
}

proc crm_count_directive {name} {
    set n 0
    foreach line [split [crm_read_config_file] "\n"] {
        set trimmed [string trim $line]
        if {$trimmed eq "" || [string match "#*" $trimmed]} continue
        if {[lindex [split $trimmed] 0] eq $name} {incr n}
    }
    return $n
}

start_server {tags {"modules config-rewrite-mode external:skip"} overrides {config-rewrite-mode runtime-modified enable-module-command yes}} {
    test {runtime-modified - REWRITE before MODULE LOAD does not emit loadmodule} {
        r config rewrite
        assert_equal 0 [crm_count_directive loadmodule]
    }

    test {runtime-modified - MODULE LOAD emits loadmodule line} {
        r module load $testmodule
        r config rewrite
        set content [crm_read_config_file]
        assert_equal 1 [crm_count_directive loadmodule]
        assert_match "*loadmodule*moduleconfigs.so*" $content
    }
}

start_server {tags {"modules config-rewrite-mode external:skip"} overrides {config-rewrite-mode runtime-modified enable-module-command yes}} {
    test {runtime-modified - module-registered config emitted when modified} {
        r module load $testmodule
        r config set moduleconfigs.mutable_bool no
        r config rewrite

        set content [crm_read_config_file]
        assert_match "*loadmodule*moduleconfigs.so*" $content
        assert_match "*moduleconfigs.mutable_bool no*" $content
        assert_equal 0 [regexp {(^|\n)moduleconfigs\.string\s} $content]
        assert_equal 0 [regexp {(^|\n)moduleconfigs\.numeric\s} $content]
        assert_equal 0 [regexp {(^|\n)moduleconfigs\.enum\s} $content]
    }
}

start_server {tags {"modules config-rewrite-mode external:skip"} overrides {config-rewrite-mode runtime-modified enable-module-command yes}} {
    test {runtime-modified - RM_ConfigSet from module is treated as runtime-modified} {
        # configaccess module exposes commands that drive RM_ConfigSet* and
        # thus exercise the moduleSetXxxConfig path in src/config.c.
        r module load $configaccessmodule
        # RM_ConfigSet on a built-in config should mark it runtime-modified.
        r configaccess.set timeout 30
        r config rewrite
        assert_match "*timeout 30*" [crm_read_config_file]
    }
}

start_server {tags {"modules config-rewrite-mode external:skip"} overrides {config-rewrite-mode runtime-modified enable-module-command yes}} {
    test {runtime-modified - MODULE LOADEX CONFIG values are persisted} {
        # LOADEX applies module configs through performModuleConfigSetFromName,
        # which must mark them runtime-modified so REWRITE emits them — else
        # on restart the module loads with defaults and the user's LOADEX
        # values are silently lost.
        r module loadex $testmodule CONFIG moduleconfigs.mutable_bool no \
                                    CONFIG moduleconfigs.string customstr
        # Sanity: values are in fact set in-memory after LOADEX.
        assert_equal {moduleconfigs.mutable_bool no} [r config get moduleconfigs.mutable_bool]
        assert_equal {moduleconfigs.string customstr} [r config get moduleconfigs.string]
        r config rewrite
        set content [crm_read_config_file]
        assert_match "*loadmodule*moduleconfigs.so*" $content
        assert_match "*moduleconfigs.mutable_bool no*" $content
        assert_match "*moduleconfigs.string*customstr*" $content
    }
}

# Module loaded via the startup config file (with module-config directives
# also in the file): the startup load path shares performModuleConfigSetFromName
# with LOADEX, but startup-loaded values must NOT be marked runtime-modified —
# otherwise a fresh server with no runtime changes triggers a spurious REWRITE.
start_server [list \
        overrides {config-rewrite-mode runtime-modified enable-module-command yes} \
        config_lines [list \
            loadmodule [file normalize tests/modules/moduleconfigs.so] \
            moduleconfigs.mutable_bool no] \
        tags {"modules config-rewrite-mode external:skip"}] {
    test {runtime-modified - startup module-config values do not mark runtime-modified} {
        # Sanity: the startup value landed in memory.
        assert_equal {moduleconfigs.mutable_bool no} [r config get moduleconfigs.mutable_bool]
        # No runtime mutations occurred. A REWRITE should be a no-op — the
        # short-circuit should fire and the file's mtime should not bump.
        after 1100
        set mtime_before [file mtime [srv 0 config_file]]
        r config rewrite
        assert_equal $mtime_before [file mtime [srv 0 config_file]]
    }
}
