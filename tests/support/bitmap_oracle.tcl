# Helpers for comparing legacy string bitmap behavior with future native bitmap
# behavior. PR1 only registers the legacy-string mode because OBJ_BITMAP does
# not exist yet.

namespace eval bitmap_oracle {
    variable modes {legacy-string}
}

proc bitmap_oracle::modes {} {
    variable modes
    return $modes
}

proc bitmap_oracle::set_modes {new_modes} {
    variable modes
    set modes $new_modes
}

proc bitmap_oracle::mode_setup {client mode} {
    switch -- $mode {
        legacy-string {
            return
        }
        default {
            error "unknown bitmap oracle mode '$mode'"
        }
    }
}

proc bitmap_oracle::keyspace_name {scenario mode} {
    set safe_scenario [string map {" " "_" ":" "_" "/" "_"} $scenario]
    set safe_mode [string map {" " "_" ":" "_" "/" "_"} $mode]
    return "bitmap-oracle:$safe_scenario:$safe_mode"
}

proc bitmap_oracle::expand_step {step keyspace} {
    set expanded {}
    foreach arg $step {
        lappend expanded [string map [list %NS% $keyspace] $arg]
    }
    return $expanded
}

proc bitmap_oracle::call {client command} {
    set invocation [linsert $command 0 $client]
    set status [catch {uplevel #0 $invocation} reply]
    if {$status == 0} {
        return [list ok $reply]
    }
    return [list err $reply]
}

proc bitmap_oracle::run_steps {client mode scenario steps} {
    bitmap_oracle::mode_setup $client $mode

    set keyspace [bitmap_oracle::keyspace_name $scenario $mode]
    set results {}
    foreach step $steps {
        set command [bitmap_oracle::expand_step $step $keyspace]
        lappend results [bitmap_oracle::call $client $command]
    }
    return $results
}

proc bitmap_oracle::assert_mode_equivalence {client scenario steps} {
    set baseline {}
    set baseline_mode {}
    foreach mode [bitmap_oracle::modes] {
        set results [bitmap_oracle::run_steps $client $mode $scenario $steps]
        if {$baseline_mode eq {}} {
            set baseline $results
            set baseline_mode $mode
        } elseif {$results ne $baseline} {
            error "bitmap oracle mismatch between '$baseline_mode' and '$mode': expected '$baseline', got '$results'"
        }
    }
    return $baseline
}
