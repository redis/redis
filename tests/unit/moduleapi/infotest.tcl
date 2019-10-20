set testmodule [file normalize tests/modules/infotest.so]

# Return value for INFO property
proc field {info property} {
    if {[regexp "\r\n$property:(.*?)\r\n" $info _ value]} {
        set _ $value
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule log-key 0

    test {module info all} {
        set info [r info all]
        # info all does not contain modules
        assert { ![string match "*Spanish*" $info] }
        assert { ![string match "*infotest_*" $info] }
        assert { [string match "*used_memory*" $info] }
    }

    test {module info everything} {
        set info [r info everything]
        # info everything contains all default sections, but not ones for crash report
        assert { [string match "*infotest_global*" $info] }
        assert { [string match "*Spanish*" $info] }
        assert { [string match "*Italian*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { ![string match "*Klingon*" $info] }
        field $info infotest_dos
    } {2}

    test {module info modules} {
        set info [r info modules]
        # info all does not contain modules
        assert { [string match "*Spanish*" $info] }
        assert { [string match "*infotest_global*" $info] }
        assert { ![string match "*used_memory*" $info] }
    }

    test {module info one module} {
        set info [r info INFOTEST]
        # info all does not contain modules
        assert { [string match "*Spanish*" $info] }
        assert { ![string match "*used_memory*" $info] }
        field $info infotest_global
    } {-2}

    test {module info one section} {
        set info [r info INFOTEST_SPANISH]
        assert { ![string match "*used_memory*" $info] }
        assert { ![string match "*Italian*" $info] }
        assert { ![string match "*infotest_global*" $info] }
        field $info infotest_uno
    } {one}

    test {module info dict} {
        set info [r info infotest_keyspace]
        set keyspace [field $info infotest_db0]
        set keys [scan [regexp -inline {keys\=([\d]*)} $keyspace] keys=%d]
    } {3}

    # TODO: test crash report.
} 
