# Regression test: listpackExExpire() dangling-pointer crash (listpack.c:1691)
# A module post-notification job triggered mid-iteration moves the listpack
# buffer via lpRealloc(), leaving the local `ptr` stale.

set testmodule_hfe_crash [file normalize tests/modules/hfe_listpackex_expire_crash.so]

start_server {tags {"external:skip needs:debug"}} {
    r module load $testmodule_hfe_crash

    test "HEXPIRE active-expire - listpackExExpire crash due to module post-notification job" {
        r config set hash-max-listpack-entries 128
        r config set hash-max-listpack-value 1100 ;# keep LISTPACK_EX after the 1000-byte HSET

        r debug set-active-expire 0
        r del hfe_crash_trigger hfe_crash_victim

        # Trigger expires first; its hexpired event queues a post-notification
        # job that does HSET on the victim with a 1000-byte value.
        r hset hfe_crash_trigger f1 v1 f2 v2
        r hpexpire hfe_crash_trigger 1 FIELDS 2 f1 f2

        # Victim expires second; the job fires inside listpackExExpire(),
        # lpRealloc() moves the buffer, and the stale ptr causes the crash.
        r hset hfe_crash_victim g1 v1 g2 v2 g3 v3
        r hpexpire hfe_crash_victim 2 FIELDS 3 g1 g2 g3

        after 20
        r debug set-active-expire 1

        # A crash produces a connection error, failing the test automatically.
        wait_for_condition 200 10 {
            [r exists hfe_crash_trigger] == 0
        } else {
            fail "hfe_crash_trigger was not expired by active-expire within timeout"
        }

        wait_for_condition 200 10 {
            [r hexists hfe_crash_victim g1] == 0
        } else {
            fail "hfe_crash_victim g1 was not expired within timeout"
        }

        assert_equal 0 [r hexists hfe_crash_victim g1]
        assert_equal 0 [r hexists hfe_crash_victim g2]
        assert_equal 0 [r hexists hfe_crash_victim g3]
        assert_equal 1 [r exists hfe_crash_victim]
    }
}

