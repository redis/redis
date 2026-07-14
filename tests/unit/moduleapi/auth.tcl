set testmodule [file normalize tests/modules/auth.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {Modules can create a user that can be authenticated} {
        # Make sure we start authenticated with default user
        r auth default ""
        assert_equal [r acl whoami] "default"
        r auth.createmoduleuser

        set id [r auth.authmoduleuser]
        assert_equal [r client id] $id

        # Verify returned id is the same as our current id and
        # we are authenticated with the specified user
        assert_equal [r acl whoami] "global"
    }

    test {De-authenticating clients is tracked and kills clients} {
        assert_equal [r auth.changecount] 0
        r auth.createmoduleuser

        # Catch the I/O exception that was thrown when Redis
        # disconnected with us. 
        catch { [r ping] } e
        assert_match {*I/O*} $e

        # Check that a user change was registered
        assert_equal [r auth.changecount] 1
    }

    test {Modules can't authenticate with ACLs users that dont exist} {
        catch { [r auth.authrealuser auth-module-test-fake] } e
        assert_match {*Invalid user*} $e
    }

    test {Modules can authenticate with ACL users} {
        assert_equal [r acl whoami] "default"

        # Create user to auth into
        r acl setuser auth-module-test on allkeys allcommands

        set id [r auth.authrealuser auth-module-test]

        # Verify returned id is the same as our current id and
        # we are authenticated with the specified user
        assert_equal [r client id] $id
        assert_equal [r acl whoami] "auth-module-test"
    }

    test {Client callback is called on user switch} {
        assert_equal [r auth.changecount] 0

        # Auth again and validate change count
        r auth.authrealuser auth-module-test
        assert_equal [r auth.changecount] 1

        # Re-auth with the default user
        r auth default ""
        assert_equal [r auth.changecount] 1
        assert_equal [r acl whoami] "default"

        # Re-auth with the default user again, to
        # verify the callback isn't fired again
        r auth default ""
        assert_equal [r auth.changecount] 0
        assert_equal [r acl whoami] "default"
    }

    test {modules can redact arguments} {
        r config set slowlog-log-slower-than 0
        r slowlog reset
        r auth.redact 1 2 3 4
        r auth.redact 1 2 3
        r config set slowlog-log-slower-than -1
        set slowlog_resp [r slowlog get]

        # There will be 3 records, slowlog reset and the
        # two auth redact calls.
        assert_equal 3 [llength $slowlog_resp]
        assert_equal {slowlog reset} [lindex [lindex $slowlog_resp 2] 3]
        assert_equal {auth.redact 1 (redacted) 3 (redacted)} [lindex [lindex $slowlog_resp 1] 3]
        assert_equal {auth.redact (redacted) 2 (redacted)} [lindex [lindex $slowlog_resp 0] 3]
    }

    test "Unload the module - testacl" {
        assert_equal {OK} [r module unload testacl]
    }
}

# Pub/Sub provenance of module-created users across ACL LOAD.
#
# A subscription created while authenticated as a module user is stamped with
# that module user* once the client switches identity. Module users are unlinked
# from the ACL registry, so ACL LOAD must resolve provenance by pointer identity,
# not by name: it must neither kill the client (mistaking the module user for a
# vanished ACL user) nor rekey the stamp onto a same-named registry user. The
# stamp must survive untouched so RM_FreeModuleUser() can still reach and
# disconnect the subscriber. Needs an aclfile (for ACL LOAD) and allchannels as
# the pubsub default (so the module user may hold subscriptions).
set server_path [tmpdir "auth-module-user-provenance"]
exec cp -f tests/assets/user.acl $server_path

start_server [list overrides [list "dir" $server_path "acl-pubsub-default" "allchannels" "aclfile" "user.acl"] tags [list "modules external:skip"]] {
    r module load $testmodule

    # Subscribe as the module user "global", then switch to the default user so
    # the subscription is stamped with the module user*. Returns the deferring
    # client, subscribed to $channel and now authenticated as default.
    proc subscribe_as_module_user {channel cname} {
        r auth.createmoduleuser
        set rd [redis_deferring_client]
        $rd hello 3 ; # RESP3 so the client can re-auth while subscribed
        $rd read
        $rd auth.authmoduleuser
        $rd read
        $rd subscribe $channel
        $rd read
        $rd auth default "" ; # stamps $channel's provenance with the module user
        $rd read
        $rd client setname $cname
        $rd read
        return $rd
    }

    test {ACL LOAD does not kill a subscriber whose provenance is a module user} {
        set rd [subscribe_as_module_user modchan modprov]

        # "global" is not present in user.acl. Name-based resolution would treat
        # it as a vanished ACL user and kill the subscriber; pointer-identity
        # resolution recognizes the module user and leaves it alone.
        r acl load
        assert_match {*modprov*} [r client list]
        r publish modchan hello
        assert_match {*hello*} [$rd read]

        # The stamp still points at the module user (it was not rekeyed): freeing
        # that user (a fresh auth.createmoduleuser frees the previous one) must
        # disconnect the subscriber.
        r auth.createmoduleuser
        wait_for_condition 50 100 {
            ![string match {*modprov*} [r client list]]
        } else {
            fail "subscriber holding module-user provenance was not disconnected when the module user was freed"
        }
        $rd close
    }

    test {ACL LOAD does not rekey module-user provenance onto a same-named ACL user} {
        # A registry user that shares the module user's name ("global") and that
        # survives the reload. Name-based resolution would rekey the stamp onto
        # this registry user, after which RM_FreeModuleUser() could no longer find
        # the subscription. Pointer identity keeps them distinct.
        set aclfile [file join $server_path user.acl]
        set fd [open $aclfile w]
        puts $fd "user default on nopass ~* &* +@all"
        puts $fd "user global on nopass ~* &* +@all"
        close $fd

        set rd [subscribe_as_module_user modchan2 modprov2]

        r acl load ; # registry "global" survives; module "global" is distinct
        assert_match {*modprov2*} [r client list]
        r publish modchan2 hello
        assert_match {*hello*} [$rd read]

        # Revoking the registry user's channels must NOT touch this subscription,
        # which is owned by the module user, not the registry user.
        r acl setuser global resetchannels
        assert_match {*modprov2*} [r client list]

        # Freeing the module user, however, must disconnect it — proving the
        # stamp still references the module user and was never rekeyed.
        r auth.createmoduleuser
        wait_for_condition 50 100 {
            ![string match {*modprov2*} [r client list]]
        } else {
            fail "subscriber was not disconnected when its module user was freed (provenance was rekeyed to the registry user)"
        }
        $rd close
        exec cp -f tests/assets/user.acl $server_path
    }
}
