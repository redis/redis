set testmodule [file normalize tests/modules/infotest.so]

# Return value for INFO property
proc field {info property} {
    if {[regexp "\r\n$property:(.*?)\r\n" $info _ value]} {
        set _ $value
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule log-key 0

    test {module reading info} {
        # check string, integer and float fields
        assert_equal [r info.gets replication role] "master"
        assert_equal [r info.getc replication role] "master"
        assert_equal [r info.geti stats expired_keys] 0
        assert_equal [r info.getd stats expired_stale_perc] 0

        # check signed and unsigned
        assert_equal [r info.geti infotest infotest_global] -2
        assert_equal [r info.getu infotest infotest_uglobal] -2

        # the above are always 0, try module info that is non-zero
        assert_equal [r info.geti infotest_italian infotest_due] 2
        set tre [r info.getd infotest_italian infotest_tre]
        assert {$tre > 3.2 && $tre < 3.4 }

        # search using the wrong section
        catch { [r info.gets badname redis_version] } e
        assert_match {*not found*} $e

        # check that section filter works
        assert { [string match "*usec_per_call*" [r info.gets all cmdstat_info.gets] ] }
        catch { [r info.gets default cmdstat_info.gets] ] } e
        assert_match {*not found*} $e
    }

    test {module info all} {
        set info [r info all]
        # info all does not contain modules
        assert { ![string match "*Spanish*" $info] }
        assert { ![string match "*infotest_*" $info] }
        assert { [string match "*used_memory*" $info] }
    }

    test {module info all infotest} {
        set info [r info all infotest]
        # info all infotest should contain both ALL and the module information
        assert { [string match "*Spanish*" $info] }
        assert { [string match "*infotest_*" $info] }
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
        set info [r info INFOtest] ;# test case insensitive compare
        # info all does not contain modules
        assert { [string match "*Spanish*" $info] }
        assert { ![string match "*used_memory*" $info] }
        field $info infotest_global
    } {-2}

    test {module info one section} {
        set info [r info INFOtest_SpanisH] ;# test case insensitive compare
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

    test {module info unsafe fields} {
        set info [r info infotest_unsafe]
        assert_match {*infotest_unsafe_field:value=1*} $info
    }

    test {module info multiply sections without all, everything, default keywords} {
        set info [r info replication INFOTEST]
        assert { [string match "*Spanish*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { [string match "*repl_offset*" $info] }
    }

    test {module info multiply sections with all keyword and modules} {
        set info [r info all modules]
        assert { [string match "*cluster*" $info] }
        assert { [string match "*cmdstat_info*" $info] }
        assert { [string match "*infotest_global*" $info] }
    }

    test {module info multiply sections with everything keyword} {
        set info [r info replication everything cpu]
        assert { [string match "*client_recent*" $info] }
        assert { [string match "*cmdstat_info*" $info] }
        assert { [string match "*Italian*" $info] }
        # check that we didn't get the same info twice
        assert { ![string match "*used_cpu_user_children*used_cpu_user_children*" $info] }
        assert { ![string match "*Italian*Italian*" $info] }
        field $info infotest_dos
    } {2}

    test "Unload the module - infotest" {
        assert_equal {OK} [r module unload infotest]
    }

    # TODO: test crash report.
} 
