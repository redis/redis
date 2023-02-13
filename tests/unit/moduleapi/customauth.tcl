set testmodule [file normalize tests/modules/auth.so]
set testmoduletwo [file normalize tests/modules/customauthtwo.so]
set miscmodule [file normalize tests/modules/misc.so]

proc cmdstat {cmd} {
    return [cmdrstat $cmd r]
}

start_server {tags {"modules"}} {
    r module load $testmodule
    r module load $testmoduletwo

    set hello2_response [r HELLO 2]
    set hello3_response [r HELLO 3]

    test {test registering custom auth callbacks} {
        assert_equal {OK} [r testmoduleone.rm_register_auth_cb]
        assert_equal {OK} [r testmoduletwo.rm_register_auth_cb]
        assert_equal {OK} [r testmoduleone.rm_register_blocking_auth_cb]
    }

    test {test custom AUTH for non existing / disabled users} {
        r config resetstat
        # Validate that an error is thrown for non existing users.
        catch { r AUTH foo pwd } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
        # Validate that an error is thrown for disabled users.
        r acl setuser foo >pwd off ~* &* +@all
        catch { r AUTH foo pwd } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=2} [cmdstat auth]
    }

    test {test non blocking custom AUTH} {
        r config resetstat
        # Test for a fixed password user
        r acl setuser foo >pwd on ~* &* +@all
        assert_equal {OK} [r AUTH foo allow]
        catch { r AUTH foo deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
        catch { r AUTH foo nomatch } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=3,*,rejected_calls=0,failed_calls=2} [cmdstat auth]
        assert_equal {OK} [r AUTH foo pwd]
        # Test for No Pass user
        r acl setuser foo on ~* &* +@all nopass
        assert_equal {OK} [r AUTH foo allow]
        catch { r AUTH foo deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=6,*,rejected_calls=0,failed_calls=3} [cmdstat auth]
        assert_equal {OK} [r AUTH foo nomatch]
    }

    test {test non blocking custom HELLO AUTH} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        # Validate proto 2 and 3 incase of success
        assert_equal $hello2_response [r HELLO 2 AUTH foo pwd]
        assert_equal $hello2_response [r HELLO 2 AUTH foo allow]
        assert_equal $hello3_response [r HELLO 3 AUTH foo pwd]
        assert_equal $hello3_response [r HELLO 3 AUTH foo allow]
        # Validate denying AUTH for the HELLO cmd
        catch {r HELLO 2 AUTH foo deny} e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=5,*,rejected_calls=0,failed_calls=1} [cmdstat hello]
        catch {r HELLO 2 AUTH foo nomatch} e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=6,*,rejected_calls=0,failed_calls=2} [cmdstat hello]
        catch {r HELLO 3 AUTH foo deny} e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=7,*,rejected_calls=0,failed_calls=3} [cmdstat hello]
        catch {r HELLO 3 AUTH foo nomatch} e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=8,*,rejected_calls=0,failed_calls=4} [cmdstat hello]
    }

    test {test non blocking custom HELLO AUTH SETNAME} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        # Validate clientname is set on success
        assert_equal $hello2_response [r HELLO 2 AUTH foo pwd setname client1]
        assert {[r client getname] eq {client1}}
        assert_equal $hello2_response [r HELLO 2 AUTH foo allow setname client2]
        assert {[r client getname] eq {client2}}
        # Validate clientname is not updated on failure
        r client setname client0
        catch {r HELLO 2 AUTH foo deny setname client1} e
        assert_match {*Auth denied by Misc Module*} $e
        assert {[r client getname] eq {client0}}
        assert_match {*calls=3,*,rejected_calls=0,failed_calls=1} [cmdstat hello]
        catch {r HELLO 2 AUTH foo nomatch setname client2} e
        assert_match {*WRONGPASS*} $e
        assert {[r client getname] eq {client0}}
        assert_match {*calls=4,*,rejected_calls=0,failed_calls=2} [cmdstat hello]
    }

    test {test blocking custom AUTH} {
        r config resetstat
        # Test for a fixed password user
        r acl setuser foo >pwd on ~* &* +@all
        assert_equal {OK} [r AUTH foo block_allow]
        catch { r AUTH foo block_deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
        catch { r AUTH foo nomatch } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=3,*,rejected_calls=0,failed_calls=2} [cmdstat auth]
        assert_equal {OK} [r AUTH foo pwd]
        # Test for No Pass user
        r acl setuser foo on ~* &* +@all nopass
        assert_equal {OK} [r AUTH foo block_allow]
        catch { r AUTH foo block_deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=6,*,rejected_calls=0,failed_calls=3} [cmdstat auth]
        assert_equal {OK} [r AUTH foo nomatch]
        # Validate that every Blocking AUTH command took at least 500000 usec.
        set stats [cmdstat auth]
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 500000}
    }

    test {test blocking custom HELLO AUTH} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        # validate proto 2 and 3 incase of success
        assert_equal $hello2_response [r HELLO 2 AUTH foo pwd]
        assert_equal $hello2_response [r HELLO 2 AUTH foo block_allow]
        assert_equal $hello3_response [r HELLO 3 AUTH foo pwd]
        assert_equal $hello3_response [r HELLO 3 AUTH foo block_allow]
        # validate denying AUTH for the HELLO cmd
        catch {r HELLO 2 AUTH foo block_deny} e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=5,*,rejected_calls=0,failed_calls=1} [cmdstat hello]
        catch {r HELLO 2 AUTH foo nomatch} e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=6,*,rejected_calls=0,failed_calls=2} [cmdstat hello]
        catch {r HELLO 3 AUTH foo block_deny} e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=7,*,rejected_calls=0,failed_calls=3} [cmdstat hello]
        catch {r HELLO 3 AUTH foo nomatch} e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=8,*,rejected_calls=0,failed_calls=4} [cmdstat hello]
        # Validate that every HELLO AUTH command took at least 500000 usec.
        set stats [cmdstat hello]
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 500000}
    }

    test {test blocking custom HELLO AUTH SETNAME} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        # Validate clientname is set on success
        assert_equal $hello2_response [r HELLO 2 AUTH foo pwd setname client1]
        assert {[r client getname] eq {client1}}
        assert_equal $hello2_response [r HELLO 2 AUTH foo block_allow setname client2]
        assert {[r client getname] eq {client2}}
        # Validate clientname is not updated on failure
        r client setname client0
        catch {r HELLO 2 AUTH foo block_deny setname client1} e
        assert_match {*Auth denied by Misc Module*} $e
        assert {[r client getname] eq {client0}}
        assert_match {*calls=3,*,rejected_calls=0,failed_calls=1} [cmdstat hello]
        catch {r HELLO 2 AUTH foo nomatch setname client2} e
        assert_match {*WRONGPASS*} $e
        assert {[r client getname] eq {client0}}
        assert_match {*calls=4,*,rejected_calls=0,failed_calls=2} [cmdstat hello]
        # Validate that every HELLO AUTH SETNAME command took at least 500000 usec.
        set stats [cmdstat hello]
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 500000}
    }

    test {test AUTH after registering multiple custom auth callbacks} {
        r config resetstat

        # Register two more callbacks from the same module.
        assert_equal {OK} [r testmoduleone.rm_register_auth_cb]
        assert_equal {OK} [r testmoduleone.rm_register_blocking_auth_cb]

        # Register another custom auth callback from the second module.
        assert_equal {OK} [r testmoduletwo.rm_register_auth_cb]

        r acl setuser foo >pwd on ~* &* +@all

        # Case 1 - Non Blocking Success
        assert_equal {OK} [r AUTH foo allow]

        # Case 2 - Non Blocking Deny
        catch { r AUTH foo deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]

        r config resetstat

        # Case 3 - Blocking Success
        assert_equal {OK} [r AUTH foo block_allow]

        # Case 4 - Blocking Deny
        catch { r AUTH foo block_deny } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]

        # Validate that every Blocking AUTH command took at least 500000 usec.
        set stats [cmdstat auth]
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 500000}

        r config resetstat

        # Case 5 - Non Blocking Success via the second module.
        assert_equal {OK} [r AUTH foo allow_two]

        # Case 6 - Non Blocking Deny via the second module.
        catch { r AUTH foo deny_two } e
        assert_match {*Auth denied by Misc Module*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]

        r config resetstat

        # Case 7 - All four auth callbacks "Skip" by not explictly allowing or denying.
        catch { r AUTH foo nomatch } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
        assert_equal {OK} [r AUTH foo pwd]

        # Because we had to attempt all 4 callbacks, validate that the AUTH command took at least
        # 1000000 usec (each blocking callback takes 500000 usec).
        set stats [cmdstat auth]
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 1000000}
    }

    test {custom auth during blocking custom auth} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        set rd [redis_deferring_client]
        set rd_two [redis_deferring_client]

        # Attempt blocking custom auth. While this ongoing, attempt non blocking custom auth from
        # moduleone/moduletwo and start another blocking custom auth from another deferring client.
        $rd AUTH foo block_allow
        wait_for_blocked_clients_count 1
        assert_equal {OK} [r AUTH foo allow]
        assert_equal {OK} [r AUTH foo allow_two]
        # Validate that the non blocking custom auth cmds finished before any blocking custom auth.
        set info_clients [r info clients]
        assert_match "*blocked_clients:1*" $info_clients
        $rd_two AUTH foo block_allow

        # Validate that all of the AUTH commands succeeded.
        wait_for_blocked_clients_count 0 500 10
        $rd flush
        assert_equal [$rd read] "OK"
        $rd_two flush
        assert_equal [$rd_two read] "OK"
        assert_match {*calls=4,*,rejected_calls=0,failed_calls=0} [cmdstat auth]
    }

    test {custom auth inside MULTI EXEC} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all

        # Validate that non blocking custom auth inside MULTI succeeds.
        r multi
        r AUTH foo allow
        assert_equal {OK} [r exec]

        # Validate that blocking custom auth inside MULTI throws an err.
        r multi
        r AUTH foo block_allow
        catch {r exec} e
        assert_match {*ERR Blocking module command called from transaction*} $e
        assert_match {*calls=2,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
    }

    test {Disabling Redis User during blocking custom auth} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        set rd [redis_deferring_client]

        # Attempt blocking custom auth and disable the Redis user while custom auth is in progress.
        $rd AUTH foo pwd
        wait_for_blocked_clients_count 1
        r acl setuser foo >pwd off ~* &* +@all

        # Validate that custom auth failed.
        wait_for_blocked_clients_count 0 500 10
        $rd flush
        catch { $rd read } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
    }

    test {Killing a client in the middle of blocking custom auth} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]

        # Attempt blocking custom auth command on client `cid` and kill the client while custom auth
        # is in progress.
        $rd AUTH foo pwd
        wait_for_blocked_clients_count 1
        r client kill id $cid

        # Validate that the blocked client count goes to 0 and no AUTH command is tracked.
        wait_for_blocked_clients_count 0 500 10
        $rd flush
        catch { $rd read } e
        assert_match {I/O error reading reply} $e
        assert_match {} [cmdstat auth]
    }

    test {test RM_AbortBlock Module API during blocking custom auth} {
        r config resetstat
        r acl setuser foo >pwd on ~* &* +@all

        # Attempt custom auth. With the "block_abort" as the password, the "customauth.so" module
        # blocks the client and uses the RM_AbortBlock API. This should result in custom auth
        # failing and the client being unblocked with the default AUTH err message.
        catch { r AUTH foo block_abort } e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1} [cmdstat auth]
    }

    test {test RM_RegisterCustomAuthCallback Module API during blocking custom auth} {
        r config resetstat
        r acl setuser foo >defaultpwd on ~* &* +@all
        set rd [redis_deferring_client]

        # Start the custom auth attempt with the standard Redis auth password for the user. This
        # will result in all custom auth cbs attempted and then standard Redis auth will be tried.
        $rd AUTH foo defaultpwd
        wait_for_blocked_clients_count 1

        # Validate that we allow modules to register custom auth cbs while custom auth is already
        # in progress.
        assert_equal {OK} [r testmoduletwo.rm_register_auth_cb]
        assert_equal {OK} [r testmoduleone.rm_register_blocking_auth_cb]

        # Validate that blocking custom auth succeeds.
        wait_for_blocked_clients_count 0 500 10
        $rd flush
        assert_equal [$rd read] "OK"
        set stats [cmdstat auth]
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} $stats

        # Validate that even the new blocking custom auth cb which was registered in the middle of
        # blocking custom auth is attempted - making it take twice the duration (2x 500000 us).
        regexp "usec_per_call=(\[0-9]{1,})\.*," $stats all usec_per_call
        assert {$usec_per_call >= 1000000}
    }

    test {Module unload during blocking custom auth} {
        r config resetstat
        r module load $miscmodule
        set rd [redis_deferring_client]
        r acl setuser foo >pwd on ~* &* +@all

        # Start a blocking custom auth attempt.
        $rd AUTH foo block_allow
        wait_for_blocked_clients_count 1

        # moduleone and moduletwo have custom auth cbs registered. Because blocking custom auth is
        # ongoing, they cannot be unloaded.
        catch {r module unload testacl} e
        assert_match {*the module has blocked clients*} $e
        catch {r module unload customauthtwo} e
        assert_match {*A client is blocked on module based custom auth*} $e

        # The misc module does not have custom auth cbs registered, so it can be unloaded even when
        # blocking custom auth is ongoing.
        assert_equal "OK" [r module unload misc]

        # Validate that blocking custom auth succeeds.
        wait_for_blocked_clients_count 0 500 10
        $rd flush
        assert_equal [$rd read] "OK"
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=0} [cmdstat auth]

        # Validate that modules can be unloaded since blocking custom auth is done.
        r module unload testacl
        # Validate that unloading the first module does not unregister custom auth cbs of the second
        # module - custom auth should succeed.
        assert_equal {OK} [r AUTH foo allow_two]
        r module unload customauthtwo

        # Validate that since custom auth cbs are unregistered, custom auth attempts fail.
        catch {r AUTH foo block_allow} e
        assert_match {*WRONGPASS*} $e
        catch {r AUTH foo allow_two} e
        assert_match {*WRONGPASS*} $e
        assert_match {*calls=4,*,rejected_calls=0,failed_calls=2} [cmdstat auth]
    }
}
