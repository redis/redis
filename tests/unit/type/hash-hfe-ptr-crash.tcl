# Regression tests: dangling-pointer / use-after-free crashes in HFE active
# expiration caused by module post-notification jobs that mutate the victim
# hash while it is being iterated.
#
# Root cause (both encodings):
#   propagateHashFieldDeletion() calls postExecutionUnitOperations(), which
#   fires queued post-notification jobs synchronously.  If a job modifies the
#   hash being expired, raw pointers held by the caller become invalid.
#
# Five scenarios are covered:
#
# 1) LISTPACK_EX + HDEL  -- job HDELs the field being expired; listpack
#    elements shift / buffer reallocates, leaving loop's `ptr` invalid.
#
# 2) LISTPACK_EX + DEL   -- job DELetes the entire hash; lpt and lp buffer
#    are freed, subsequent lpNext() is a use-after-free.
#
# 3) HT + HSET           -- job HSETs the expiring field with a large value;
#    entryUpdate() reallocates the Entry and frees the old one, leaving
#    `field = (sds)e` in onFieldExpire() dangling.
#
# 4) HT + HDEL           -- job HDELs the expiring field; dictEntryDestructor
#    -> entryFree() frees the Entry while `field` still points to it.
#
# 5) HT + DEL            -- job DELetes the entire hash; expCtx->hashObj and
#    `field` become dangling, hashTypeDelete() dereferences freed memory.

set testmodule_lp_hdel  [file normalize tests/modules/hfe_listpackex_hdel_crash.so]
set testmodule_ht_hset  [file normalize tests/modules/hfe_ht_hset_crash.so]
set testmodule_ht_hdel  [file normalize tests/modules/hfe_ht_hdel_crash.so]

start_server {tags {"external:skip needs:debug"}} {
    r module load $testmodule_lp_hdel
    r module load $testmodule_ht_hset
    r module load $testmodule_ht_hdel

    # ------------------------------------------------------------------
    # Scenario 1: LISTPACK_EX + HDEL
    # ------------------------------------------------------------------
    test "HEXPIRE active-expire - listpackExExpire crash: LISTPACK_EX + HDEL of expired field" {
        r config set hash-max-listpack-entries 128
        r config set hash-max-listpack-value 512
        r debug set-active-expire 0
        r del hfe_hdel_lp_trigger hfe_hdel_lp_victim

        r hset hfe_hdel_lp_trigger f1 v1 f2 v2
        r hpexpire hfe_hdel_lp_trigger 1 FIELDS 2 f1 f2

        # g1 expires first (2 ms) -- the job fires during g1's
        # propagateHashFieldDeletion() and HDELs g1, invalidating ptr.
        r hset hfe_hdel_lp_victim g1 v1
        r hpexpire hfe_hdel_lp_victim 2 FIELDS 1 g1
        r hset hfe_hdel_lp_victim g2 v2
        r hpexpire hfe_hdel_lp_victim 3 FIELDS 1 g2
        r hset hfe_hdel_lp_victim g3 v3
        r hpexpire hfe_hdel_lp_victim 4 FIELDS 1 g3
        r hset hfe_hdel_lp_victim g4 v4   ;# non-expired, stays at end of listpack

        after 20
        r debug set-active-expire 1

        wait_for_condition 200 10 { [r exists hfe_hdel_lp_trigger] == 0 } else {
            fail "hfe_hdel_lp_trigger was not expired by active-expire within timeout"
        }
        wait_for_condition 200 10 { [r hexists hfe_hdel_lp_victim g2] == 0 } else {
            fail "hfe_hdel_lp_victim fields were not expired within timeout"
        }

        assert_equal 0 [r hexists hfe_hdel_lp_victim g1]
        assert_equal 0 [r hexists hfe_hdel_lp_victim g2]
        assert_equal 0 [r hexists hfe_hdel_lp_victim g3]
        assert_equal 1 [r hexists hfe_hdel_lp_victim g4]
        assert_equal 1 [r exists  hfe_hdel_lp_victim]
    }

    # ------------------------------------------------------------------
    # Scenario 2: LISTPACK_EX + DEL
    # ------------------------------------------------------------------
    test "HEXPIRE active-expire - listpackExExpire crash: LISTPACK_EX + DEL of victim hash" {
        r config set hash-max-listpack-entries 128
        r config set hash-max-listpack-value 512
        r debug set-active-expire 0
        r del hfe_del_lp_trigger hfe_del_lp_victim

        r hset hfe_del_lp_trigger f1 v1 f2 v2
        r hpexpire hfe_del_lp_trigger 1 FIELDS 2 f1 f2

        r hset hfe_del_lp_victim g1 v1 g2 v2 g3 v3
        r hpexpire hfe_del_lp_victim 2 FIELDS 3 g1 g2 g3

        after 20
        r debug set-active-expire 1

        wait_for_condition 200 10 { [r exists hfe_del_lp_trigger] == 0 } else {
            fail "hfe_del_lp_trigger was not expired by active-expire within timeout"
        }
        wait_for_condition 200 10 { [r exists hfe_del_lp_victim] == 0 } else {
            fail "hfe_del_lp_victim should have been deleted by the post-notification job"
        }

        assert_equal 0 [r exists hfe_del_lp_victim]
    }

    # ------------------------------------------------------------------
    # Scenario 3: HT + HSET
    # ------------------------------------------------------------------
    test "HEXPIRE active-expire - onFieldExpire crash: HT + HSET replaces expired field entry" {
        r config set hash-max-listpack-entries 0
        r debug set-active-expire 0
        r del hfe_hset_ht_trigger hfe_hset_ht_victim

        r hset hfe_hset_ht_trigger f1 v1 f2 v2
        r hpexpire hfe_hset_ht_trigger 1 FIELDS 2 f1 f2

        # g1 (small value, embedded entry + ExpireMeta) expires first;
        # the job HSETs it with a 1000-byte value -> entryUpdate() changes
        # layout, frees old Entry, leaving `field` dangling.
        r hset hfe_hset_ht_victim g1 v1
        r hpexpire hfe_hset_ht_victim 2 FIELDS 1 g1
        r hset hfe_hset_ht_victim g2 v2
        r hpexpire hfe_hset_ht_victim 3 FIELDS 1 g2
        r hset hfe_hset_ht_victim g3 v3
        r hpexpire hfe_hset_ht_victim 4 FIELDS 1 g3

        after 20
        r debug set-active-expire 1

        wait_for_condition 200 10 { [r exists hfe_hset_ht_trigger] == 0 } else {
            fail "hfe_hset_ht_trigger was not expired by active-expire within timeout"
        }
        wait_for_condition 200 10 { [r hexists hfe_hset_ht_victim g2] == 0 } else {
            fail "hfe_hset_ht_victim fields were not expired within timeout"
        }

        assert_equal 0 [r hexists hfe_hset_ht_victim g2]
        assert_equal 0 [r hexists hfe_hset_ht_victim g3]
        assert_equal 1 [r exists  hfe_hset_ht_victim]  ;# g1 re-set by job
    }

    # ------------------------------------------------------------------
    # Scenario 4: HT + HDEL
    # ------------------------------------------------------------------
    test "HEXPIRE active-expire - onFieldExpire crash: HT + HDEL of the expired field" {
        r config set hash-max-listpack-entries 0
        r debug set-active-expire 0
        r del hfe_hdel_ht_trigger hfe_hdel_ht_victim

        r hset hfe_hdel_ht_trigger f1 v1 f2 v2
        r hpexpire hfe_hdel_ht_trigger 1 FIELDS 2 f1 f2

        r hset hfe_hdel_ht_victim g1 v1
        r hpexpire hfe_hdel_ht_victim 2 FIELDS 1 g1
        r hset hfe_hdel_ht_victim g2 v2
        r hpexpire hfe_hdel_ht_victim 3 FIELDS 1 g2
        r hset hfe_hdel_ht_victim g3 v3
        r hpexpire hfe_hdel_ht_victim 4 FIELDS 1 g3
        r hset hfe_hdel_ht_victim g4 v4   ;# non-expired, should survive

        after 20
        r debug set-active-expire 1

        wait_for_condition 200 10 { [r exists hfe_hdel_ht_trigger] == 0 } else {
            fail "hfe_hdel_ht_trigger was not expired by active-expire within timeout"
        }
        wait_for_condition 200 10 { [r hexists hfe_hdel_ht_victim g2] == 0 } else {
            fail "hfe_hdel_ht_victim fields were not expired within timeout"
        }

        assert_equal 0 [r hexists hfe_hdel_ht_victim g1]
        assert_equal 0 [r hexists hfe_hdel_ht_victim g2]
        assert_equal 0 [r hexists hfe_hdel_ht_victim g3]
        assert_equal 1 [r hexists hfe_hdel_ht_victim g4]
        assert_equal 1 [r exists  hfe_hdel_ht_victim]
    }

    # ------------------------------------------------------------------
    # Scenario 5: HT + DEL
    # ------------------------------------------------------------------
    test "HEXPIRE active-expire - onFieldExpire crash: HT + DEL of victim hash" {
        r config set hash-max-listpack-entries 0
        r debug set-active-expire 0
        r del hfe_del_ht_trigger hfe_del_ht_victim

        r hset hfe_del_ht_trigger f1 v1 f2 v2
        r hpexpire hfe_del_ht_trigger 1 FIELDS 2 f1 f2

        r hset hfe_del_ht_victim g1 v1 g2 v2 g3 v3
        r hpexpire hfe_del_ht_victim 2 FIELDS 3 g1 g2 g3

        after 20
        r debug set-active-expire 1

        wait_for_condition 200 10 { [r exists hfe_del_ht_trigger] == 0 } else {
            fail "hfe_del_ht_trigger was not expired by active-expire within timeout"
        }
        wait_for_condition 200 10 { [r exists hfe_del_ht_victim] == 0 } else {
            fail "hfe_del_ht_victim should have been deleted by the post-notification job"
        }

        assert_equal 0 [r exists hfe_del_ht_victim]
    }
}

