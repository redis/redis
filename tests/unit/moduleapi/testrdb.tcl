set testmodule [file normalize tests/modules/testrdb.so]

proc restart_and_wait {} {
    catch {
        r debug restart
    }

    # wait for the server to come back up
    set retry 50
    while {$retry} {
        if {[catch { r ping }]} {
            after 100
        } else {
            break
        }
        incr retry -1
    }
}

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {
        test {modules are able to persist types} {
            r testrdb.set.key key1 value1
            assert_equal "value1" [r testrdb.get.key key1]
            r debug reload
            assert_equal "value1" [r testrdb.get.key key1]
        }

        test {modules global are lost without aux} {
            r testrdb.set.before global1
            assert_equal "global1" [r testrdb.get.before]
            restart_and_wait
            assert_equal "" [r testrdb.get.before]
        }
    }

    start_server [list overrides [list loadmodule "$testmodule 2"]] {
        test {modules are able to persist globals before and after} {
            r testrdb.set.before global1
            r testrdb.set.after global2
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
            restart_and_wait
            assert_equal "global1" [r testrdb.get.before]
            assert_equal "global2" [r testrdb.get.after]
        }

    }

    start_server [list overrides [list loadmodule "$testmodule 1"]] {
        test {modules are able to persist globals just after} {
            r testrdb.set.after global2
            assert_equal "global2" [r testrdb.get.after]
            restart_and_wait
            assert_equal "global2" [r testrdb.get.after]
        }
    }


    # TODO: test short read handling

}
