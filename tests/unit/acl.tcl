start_server {tags {"acl external:skip"}} {
    test {Connections start with the default user} {
        r ACL WHOAMI
    } {default}

    test {It is possible to create new users} {
        r ACL setuser newuser
    }

    test {Coverage: ACL USERS} {
        r ACL USERS
    } {default newuser}

    test {Usernames can not contain spaces or null characters} {
        catch {r ACL setuser "a a"} err
        set err
    } {*Usernames can't contain spaces or null characters*}

    test {New users start disabled} {
        r ACL setuser newuser >passwd1
        catch {r AUTH newuser passwd1} err
        set err
    } {*WRONGPASS*}

    test {Enabling the user allows the login} {
        r ACL setuser newuser on +acl
        r AUTH newuser passwd1
        r ACL WHOAMI
    } {newuser}

    test {Only the set of correct passwords work} {
        r ACL setuser newuser >passwd2
        catch {r AUTH newuser passwd1} e
        assert {$e eq "OK"}
        catch {r AUTH newuser passwd2} e
        assert {$e eq "OK"}
        catch {r AUTH newuser passwd3} e
        set e
    } {*WRONGPASS*}

    test {It is possible to remove passwords from the set of valid ones} {
        r ACL setuser newuser <passwd1
        catch {r AUTH newuser passwd1} e
        set e
    } {*WRONGPASS*}

    test {Test password hashes can be added} {
        r ACL setuser newuser #34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4e6
        catch {r AUTH newuser passwd4} e
        assert {$e eq "OK"}
    }

    test {Test password hashes validate input} {
        # Validate Length
        catch {r ACL setuser newuser #34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4e} e
        # Validate character outside set
        catch {r ACL setuser newuser #34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4eq} e
        set e
    } {*Error in ACL SETUSER modifier*}

    test {ACL GETUSER returns the password hash instead of the actual password} {
        set passstr [dict get [r ACL getuser newuser] passwords]
        assert_match {*34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4e6*} $passstr
        assert_no_match {*passwd4*} $passstr
    }

    test {Test hashed passwords removal} {
        r ACL setuser newuser !34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4e6
        set passstr [dict get [r ACL getuser newuser] passwords]
        assert_no_match {*34344e4d60c2b6d639b7bd22e18f2b0b91bc34bf0ac5f9952744435093cfb4e6*} $passstr
    }

    test {By default users are not able to access any command} {
        catch {r SET foo bar} e
        set e
    } {*NOPERM*set*}

    test {By default users are not able to access any key} {
        r ACL setuser newuser +set
        catch {r SET foo bar} e
        set e
    } {*NOPERM*key*}

    test {It's possible to allow the access of a subset of keys} {
        r ACL setuser newuser allcommands ~foo:* ~bar:*
        r SET foo:1 a
        r SET bar:2 b
        catch {r SET zap:3 c} e
        r ACL setuser newuser allkeys; # Undo keys ACL
        set e
    } {*NOPERM*key*}

    test {By default, only default user is able to publish to any channel} {
        r AUTH default pwd
        r PUBLISH foo bar
        r ACL setuser psuser on >pspass +acl +client +@pubsub
        r AUTH psuser pspass
        catch {r PUBLISH foo bar} e
        set e
    } {*NOPERM*channel*}

    test {By default, only default user is not able to publish to any shard channel} {
        r AUTH default pwd
        r SPUBLISH foo bar
        r AUTH psuser pspass
        catch {r SPUBLISH foo bar} e
        set e
    } {*NOPERM*channel*}

    test {By default, only default user is able to subscribe to any channel} {
        set rd [redis_deferring_client]
        $rd AUTH default pwd
        $rd read
        $rd SUBSCRIBE foo
        assert_match {subscribe foo 1} [$rd read]
        $rd UNSUBSCRIBE
        $rd read
        $rd AUTH psuser pspass
        $rd read
        $rd SUBSCRIBE foo
        catch {$rd read} e
        $rd close
        set e
    } {*NOPERM*channel*}

    test {By default, only default user is able to subscribe to any shard channel} {
        set rd [redis_deferring_client]
        $rd AUTH default pwd
        $rd read
        $rd SSUBSCRIBE foo
        assert_match {ssubscribe foo 1} [$rd read]
        $rd SUNSUBSCRIBE
        $rd read
        $rd AUTH psuser pspass
        $rd read
        $rd SSUBSCRIBE foo
        catch {$rd read} e
        $rd close
        set e
    } {*NOPERM*channel*}

    test {By default, only default user is able to subscribe to any pattern} {
        set rd [redis_deferring_client]
        $rd AUTH default pwd
        $rd read
        $rd PSUBSCRIBE bar*
        assert_match {psubscribe bar\* 1} [$rd read]
        $rd PUNSUBSCRIBE
        $rd read
        $rd AUTH psuser pspass
        $rd read
        $rd PSUBSCRIBE bar*
        catch {$rd read} e
        $rd close
        set e
    } {*NOPERM*channel*}

    test {It's possible to allow publishing to a subset of channels} {
        r ACL setuser psuser resetchannels &foo:1 &bar:*
        assert_equal {0} [r PUBLISH foo:1 somemessage]
        assert_equal {0} [r PUBLISH bar:2 anothermessage]
        catch {r PUBLISH zap:3 nosuchmessage} e
        set e
    } {*NOPERM*channel*}

    test {It's possible to allow publishing to a subset of shard channels} {
        r ACL setuser psuser resetchannels &foo:1 &bar:*
        assert_equal {0} [r SPUBLISH foo:1 somemessage]
        assert_equal {0} [r SPUBLISH bar:2 anothermessage]
        catch {r SPUBLISH zap:3 nosuchmessage} e
        set e
    } {*NOPERM*channel*}

    test {Validate subset of channels is prefixed with resetchannels flag} {
        r ACL setuser hpuser on nopass resetchannels &foo +@all

        # Verify resetchannels flag is prefixed before the channel name(s)
        set users [r ACL LIST]
        set curruser "hpuser"
        foreach user [lshuffle $users] {
            if {[string first $curruser $user] != -1} {
                assert_equal {user hpuser on nopass sanitize-payload resetchannels &foo +@all} $user
            }
        }

        # authenticate as hpuser
        r AUTH hpuser pass

        assert_equal {0} [r PUBLISH foo bar]
        catch {r PUBLISH bar game} e

        # Falling back to psuser for the below tests
        r AUTH psuser pspass
        r ACL deluser hpuser
        set e
    } {*NOPERM*channel*}

    test {In transaction queue publish/subscribe/psubscribe to unauthorized channel will fail} {
        r ACL setuser psuser +multi +discard
        r MULTI
        assert_error {*NOPERM*channel*} {r PUBLISH notexits helloworld}
        r DISCARD
        r MULTI
        assert_error {*NOPERM*channel*} {r SUBSCRIBE notexits foo:1}
        r DISCARD
        r MULTI
        assert_error {*NOPERM*channel*} {r PSUBSCRIBE notexits:* bar:*}
        r DISCARD
    }

    test {It's possible to allow subscribing to a subset of channels} {
        set rd [redis_deferring_client]
        $rd AUTH psuser pspass
        $rd read
        $rd SUBSCRIBE foo:1
        assert_match {subscribe foo:1 1} [$rd read]
        $rd SUBSCRIBE bar:2
        assert_match {subscribe bar:2 2} [$rd read]
        $rd SUBSCRIBE zap:3
        catch {$rd read} e
        set e
    } {*NOPERM*channel*}

    test {It's possible to allow subscribing to a subset of shard channels} {
        set rd [redis_deferring_client]
        $rd AUTH psuser pspass
        $rd read
        $rd SSUBSCRIBE foo:1
        assert_match {ssubscribe foo:1 1} [$rd read]
        $rd SSUBSCRIBE bar:2
        assert_match {ssubscribe bar:2 2} [$rd read]
        $rd SSUBSCRIBE zap:3
        catch {$rd read} e
        set e
    } {*NOPERM*channel*}

    test {It's possible to allow subscribing to a subset of channel patterns} {
        set rd [redis_deferring_client]
        $rd AUTH psuser pspass
        $rd read
        $rd PSUBSCRIBE foo:1
        assert_match {psubscribe foo:1 1} [$rd read]
        $rd PSUBSCRIBE bar:*
        assert_match {psubscribe bar:\* 2} [$rd read]
        $rd PSUBSCRIBE bar:baz
        catch {$rd read} e
        set e
    } {*NOPERM*channel*}
    
    test {Subscribers are killed when revoked of channel permission} {
        set rd [redis_deferring_client]
        r ACL setuser psuser resetchannels &foo:1
        $rd AUTH psuser pspass
        $rd read
        $rd CLIENT SETNAME deathrow
        $rd read
        $rd SUBSCRIBE foo:1
        $rd read
        r ACL setuser psuser resetchannels
        assert_no_match {*deathrow*} [r CLIENT LIST]
        $rd close
    } {0}

    test {Subscribers are killed when revoked of channel permission} {
        set rd [redis_deferring_client]
        r ACL setuser psuser resetchannels &foo:1
        $rd AUTH psuser pspass
        $rd read
        $rd CLIENT SETNAME deathrow
        $rd read
        $rd SSUBSCRIBE foo:1
        $rd read
        r ACL setuser psuser resetchannels
        assert_no_match {*deathrow*} [r CLIENT LIST]
        $rd close
    } {0}

    test {Subscribers are killed when revoked of pattern permission} {
        set rd [redis_deferring_client]
        r ACL setuser psuser resetchannels &bar:*
        $rd AUTH psuser pspass
        $rd read
        $rd CLIENT SETNAME deathrow
        $rd read
        $rd PSUBSCRIBE bar:*
        $rd read
        r ACL setuser psuser resetchannels
        assert_no_match {*deathrow*} [r CLIENT LIST]
        $rd close
    } {0}

    test {Subscribers are killed when revoked of allchannels permission} {
        set rd [redis_deferring_client]
        r ACL setuser psuser allchannels
        $rd AUTH psuser pspass
        $rd read
        $rd CLIENT SETNAME deathrow
        $rd read
        $rd PSUBSCRIBE foo
        $rd read
        r ACL setuser psuser resetchannels
        assert_no_match {*deathrow*} [r CLIENT LIST]
        $rd close
    } {0}

    test {Subscribers are pardoned if literal permissions are retained and/or gaining allchannels} {
        set rd [redis_deferring_client]
        r ACL setuser psuser resetchannels &foo:1 &bar:* &orders
        $rd AUTH psuser pspass
        $rd read
        $rd CLIENT SETNAME pardoned
        $rd read
        $rd SUBSCRIBE foo:1
        $rd read
        $rd SSUBSCRIBE orders
        $rd read
        $rd PSUBSCRIBE bar:*
        $rd read
        r ACL setuser psuser resetchannels &foo:1 &bar:* &orders &baz:qaz &zoo:*
        assert_match {*pardoned*} [r CLIENT LIST]
        r ACL setuser psuser allchannels
        assert_match {*pardoned*} [r CLIENT LIST]
        $rd close
    } {0}

    test {blocked command gets rejected when reprocessed after permission change} {
        r auth default ""
        r config resetstat
        set rd [redis_deferring_client]
        r ACL setuser psuser reset on nopass +@all allkeys
        $rd AUTH psuser pspass
        $rd read
        $rd BLPOP list1 0
        wait_for_blocked_client
        r ACL setuser psuser resetkeys
        r LPUSH list1 foo
        assert_error {*NOPERM No permissions to access a key*} {$rd read}
        $rd ping
        $rd close
        assert_match {*calls=0,usec=0,*,rejected_calls=1,failed_calls=0} [cmdrstat blpop r]
    }

    test {Users can be configured to authenticate with any password} {
        r ACL setuser newuser nopass
        r AUTH newuser zipzapblabla
    } {OK}

    test {ACLs can exclude single commands} {
        r ACL setuser newuser -ping
        r INCR mycounter ; # Should not raise an error
        catch {r PING} e
        set e
    } {*NOPERM*ping*}

    test {ACLs can include or exclude whole classes of commands} {
        r ACL setuser newuser -@all +@set +acl
        r SADD myset a b c; # Should not raise an error
        r ACL setuser newuser +@all -@string
        r SADD myset a b c; # Again should not raise an error
        # String commands instead should raise an error
        catch {r SET foo bar} e
        r ACL setuser newuser allcommands; # Undo commands ACL
        set e
    } {*NOPERM*set*}

    test {ACLs can include single subcommands} {
        r ACL setuser newuser +@all -client
        r ACL setuser newuser +client|id +client|setname
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {+@all*-client*+client|id*} $cmdstr
        assert_match {+@all*-client*+client|setname*} $cmdstr
        r CLIENT ID; # Should not fail
        r CLIENT SETNAME foo ; # Should not fail
        catch {r CLIENT KILL type master} e
        set e
    } {*NOPERM*client|kill*}

    test {ACLs can exclude single subcommands, case 1} {
        r ACL setuser newuser +@all -client|kill
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_equal {+@all -client|kill} $cmdstr
        r CLIENT ID; # Should not fail
        r CLIENT SETNAME foo ; # Should not fail
        catch {r CLIENT KILL type master} e
        set e
    } {*NOPERM*client|kill*}

    test {ACLs can exclude single subcommands, case 2} {
        r ACL setuser newuser -@all +acl +config -config|set
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {*+config*} $cmdstr
        assert_match {*-config|set*} $cmdstr
        r CONFIG GET loglevel; # Should not fail
        catch {r CONFIG SET loglevel debug} e
        set e
    } {*NOPERM*config|set*}

    test {ACLs cannot include a subcommand with a specific arg} {
        r ACL setuser newuser +@all -config|get
        catch { r ACL setuser newuser +config|get|appendonly} e
        set e
    } {*Allowing first-arg of a subcommand is not supported*}

    test {ACLs cannot exclude or include a container commands with a specific arg} {
        r ACL setuser newuser +@all +config|get
        catch { r ACL setuser newuser +@all +config|asdf} e
        assert_match "*Unknown command or category name in ACL*" $e
        catch { r ACL setuser newuser +@all -config|asdf} e
        assert_match "*Unknown command or category name in ACL*" $e
    } {}

    test {ACLs cannot exclude or include a container command with two args} {
        r ACL setuser newuser +@all +config|get
        catch { r ACL setuser newuser +@all +get|key1|key2} e
        assert_match "*Unknown command or category name in ACL*" $e
        catch { r ACL setuser newuser +@all -get|key1|key2} e
        assert_match "*Unknown command or category name in ACL*" $e
    } {}

    test {ACLs including of a type includes also subcommands} {
        r ACL setuser newuser -@all +del +acl +@stream
        r DEL key
        r XADD key * field value
        r XINFO STREAM key
    }

    test {ACLs can block SELECT of all but a specific DB} {
        r ACL setuser newuser -@all +acl +select|0
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {*+select|0*} $cmdstr
        r SELECT 0
        catch {r SELECT 1} e
        set e
    } {*NOPERM*select*} {singledb:skip}

    test {ACLs can block all DEBUG subcommands except one} {
        r ACL setuser newuser -@all +acl +del +incr +debug|object
        r DEL key
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {*+debug|object*} $cmdstr
        r INCR key
        r DEBUG OBJECT key
        catch {r DEBUG SEGFAULT} e
        set e
    } {*NOPERM*debug*}

    test {ACLs set can include subcommands, if already full command exists} {
        r ACL setuser bob +memory|doctor
        set cmdstr [dict get [r ACL getuser bob] commands]
        assert_equal {-@all +memory|doctor} $cmdstr

        # Validate the commands have got engulfed to +memory.
        r ACL setuser bob +memory
        set cmdstr [dict get [r ACL getuser bob] commands]
        assert_equal {-@all +memory} $cmdstr

        # Appending to the existing access string of bob.
        r ACL setuser bob +@all +client|id
        # Although this does nothing, we retain it anyways so we can reproduce
        # the original ACL. 
        set cmdstr [dict get [r ACL getuser bob] commands]
        assert_equal {+@all +client|id} $cmdstr

        r ACL setuser bob >passwd1 on
        r AUTH bob passwd1
        r CLIENT ID; # Should not fail
        r MEMORY DOCTOR; # Should not fail
    }

    test {ACLs set can exclude subcommands, if already full command exists} {
        r ACL setuser alice +@all -memory|doctor
        set cmdstr [dict get [r ACL getuser alice] commands]
        assert_equal {+@all -memory|doctor} $cmdstr

        r ACL setuser alice >passwd1 on
        r AUTH alice passwd1

        assert_error {*NOPERM*memory|doctor*} {r MEMORY DOCTOR}
        r MEMORY STATS ;# should work

        # Validate the commands have got engulfed to -memory.
        r ACL setuser alice +@all -memory
        set cmdstr [dict get [r ACL getuser alice] commands]
        assert_equal {+@all -memory} $cmdstr

        assert_error {*NOPERM*memory|doctor*} {r MEMORY DOCTOR}
        assert_error {*NOPERM*memory|stats*} {r MEMORY STATS}

        # Appending to the existing access string of alice.
        r ACL setuser alice -@all

        # Now, alice can't do anything, we need to auth newuser to execute ACL GETUSER
        r AUTH newuser passwd1

        # Validate the new commands has got engulfed to -@all.
        set cmdstr [dict get [r ACL getuser alice] commands]
        assert_equal {-@all} $cmdstr

        r AUTH alice passwd1

        assert_error {*NOPERM*get*} {r GET key}
        assert_error {*NOPERM*memory|stats*} {r MEMORY STATS}

        # Auth newuser before the next test
        r AUTH newuser passwd1
    }

    test {ACL SETUSER RESET reverting to default newly created user} {
        set current_user "example"
        r ACL DELUSER $current_user
        r ACL SETUSER $current_user

        set users [r ACL LIST]
        foreach user [lshuffle $users] {
            if {[string first $current_user $user] != -1} {
                set current_user_output $user
            }
        }

        r ACL SETUSER $current_user reset
        set users [r ACL LIST]
        foreach user [lshuffle $users] {
            if {[string first $current_user $user] != -1} {
                assert_equal $current_user_output $user
            }
        }
    }

    # Note that the order of the generated ACL rules is not stable in Redis
    # so we need to match the different parts and not as a whole string.
    test {ACL GETUSER is able to translate back command permissions} {
        # Subtractive
        r ACL setuser newuser reset +@all ~* -@string +incr -debug +debug|digest
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {*+@all*} $cmdstr
        assert_match {*-@string*} $cmdstr
        assert_match {*+incr*} $cmdstr
        assert_match {*-debug +debug|digest**} $cmdstr

        # Additive
        r ACL setuser newuser reset +@string -incr +acl +debug|digest +debug|segfault
        set cmdstr [dict get [r ACL getuser newuser] commands]
        assert_match {*-@all*} $cmdstr
        assert_match {*+@string*} $cmdstr
        assert_match {*-incr*} $cmdstr
        assert_match {*+debug|digest*} $cmdstr
        assert_match {*+debug|segfault*} $cmdstr
        assert_match {*+acl*} $cmdstr
    }

    # A regression test make sure that as long as there is a simple
    # category defining the commands, that it will be used as is.
    test {ACL GETUSER provides reasonable results} {
        set categories [r ACL CAT]

        # Test that adding each single category will
        # result in just that category with both +@all and -@all
        foreach category $categories {
            # Test for future commands where allowed
            r ACL setuser additive reset +@all "-@$category"
            set cmdstr [dict get [r ACL getuser additive] commands]
            assert_equal "+@all -@$category" $cmdstr

            # Test for future commands where disallowed
            r ACL setuser restrictive reset -@all "+@$category"
            set cmdstr [dict get [r ACL getuser restrictive] commands]
            assert_equal "-@all +@$category" $cmdstr
        }
    }

    # Test that only lossless compaction of ACLs occur.
    test {ACL GETUSER provides correct results} {
        r ACL SETUSER adv-test
        r ACL SETUSER adv-test +@all -@hash -@slow +hget
        assert_equal "+@all -@hash -@slow +hget" [dict get [r ACL getuser adv-test] commands]

        # Categories are re-ordered if re-added
        r ACL SETUSER adv-test -@hash
        assert_equal "+@all -@slow +hget -@hash" [dict get [r ACL getuser adv-test] commands]

        # Inverting categories removes existing categories
        r ACL SETUSER adv-test +@hash
        assert_equal "+@all -@slow +hget +@hash" [dict get [r ACL getuser adv-test] commands]

        # Inverting the all category compacts everything
        r ACL SETUSER adv-test -@all
        assert_equal "-@all" [dict get [r ACL getuser adv-test] commands]
        r ACL SETUSER adv-test -@string -@slow +@all
        assert_equal "+@all" [dict get [r ACL getuser adv-test] commands]

        # Make sure categories are case insensitive
        r ACL SETUSER adv-test -@all +@HASH +@hash +@HaSh
        assert_equal "-@all +@hash" [dict get [r ACL getuser adv-test] commands]

        # Make sure commands are case insensitive
        r ACL SETUSER adv-test -@all +HGET +hget +hGeT
        assert_equal "-@all +hget" [dict get [r ACL getuser adv-test] commands]

        # Arbitrary category additions and removals are handled
        r ACL SETUSER adv-test -@all +@hash +@slow +@set +@set +@slow +@hash
        assert_equal "-@all +@set +@slow +@hash" [dict get [r ACL getuser adv-test] commands]

        # Arbitrary command additions and removals are handled
        r ACL SETUSER adv-test -@all +hget -hset +hset -hget
        assert_equal "-@all +hset -hget" [dict get [r ACL getuser adv-test] commands]

        # Arbitrary subcommands are compacted
        r ACL SETUSER adv-test -@all +client|list +client|list +config|get +config +acl|list -acl
        assert_equal "-@all +client|list +config -acl" [dict get [r ACL getuser adv-test] commands]

        # Deprecated subcommand usage is handled
        r ACL SETUSER adv-test -@all +select|0 +select|0 +debug|segfault +debug
        assert_equal "-@all +select|0 +debug" [dict get [r ACL getuser adv-test] commands]

        # Unnecessary categories are retained for potentional future compatibility
        r ACL SETUSER adv-test -@all -@dangerous
        assert_equal "-@all -@dangerous" [dict get [r ACL getuser adv-test] commands]

        # Duplicate categories are compressed, regression test for #12470
        r ACL SETUSER adv-test -@all +config +config|get -config|set +config
        assert_equal "-@all +config" [dict get [r ACL getuser adv-test] commands]
    }

    test "ACL CAT with illegal arguments" {
        assert_error {*Unknown category 'NON_EXISTS'} {r ACL CAT NON_EXISTS}
        assert_error {*unknown subcommand or wrong number of arguments for 'CAT'*} {r ACL CAT NON_EXISTS NON_EXISTS2}
    }

    test "ACL CAT without category - list all categories" {
        set categories [r acl cat]
        assert_not_equal [lsearch $categories "keyspace"] -1
        assert_not_equal [lsearch $categories "connection"] -1
    }

    test "ACL CAT category - list all commands/subcommands that belong to category" {
        assert_not_equal [lsearch [r acl cat transaction] "multi"] -1
        assert_not_equal [lsearch [r acl cat scripting] "function|list"] -1

        # Negative check to make sure it doesn't actually return all commands.
        assert_equal [lsearch [r acl cat keyspace] "set"] -1
        assert_equal [lsearch [r acl cat stream] "get"] -1
    }

    test "ACL requires explicit permission for scripting for EVAL_RO, EVALSHA_RO and FCALL_RO" {
        r ACL SETUSER scripter on nopass +readonly
        assert_match {*has no permissions to run the 'eval_ro' command*} [r ACL DRYRUN scripter EVAL_RO "" 0]
        assert_match {*has no permissions to run the 'evalsha_ro' command*} [r ACL DRYRUN scripter EVALSHA_RO "" 0]
        assert_match {*has no permissions to run the 'fcall_ro' command*} [r ACL DRYRUN scripter FCALL_RO "" 0]
    }

    test {ACL #5998 regression: memory leaks adding / removing subcommands} {
        r AUTH default ""
        r ACL setuser newuser reset -debug +debug|a +debug|b +debug|c
        r ACL setuser newuser -debug
        # The test framework will detect a leak if any.
    }

    test {ACL LOG aggregates similar errors together and assigns unique entry-id to new errors} {
         r ACL LOG RESET
         r ACL setuser user1 >foo
         assert_error "*WRONGPASS*" {r AUTH user1 doo}
         set entry_id_initial_error [dict get [lindex [r ACL LOG] 0] entry-id]
         set timestamp_created_original [dict get [lindex [r ACL LOG] 0] timestamp-created]
         set timestamp_last_update_original [dict get [lindex [r ACL LOG] 0] timestamp-last-updated]
         after 1
         for {set j 0} {$j < 10} {incr j} {
             assert_error "*WRONGPASS*" {r AUTH user1 doo}
         }
         set entry_id_lastest_error [dict get [lindex [r ACL LOG] 0] entry-id]
         set timestamp_created_updated [dict get [lindex [r ACL LOG] 0] timestamp-created]
         set timestamp_last_updated_after_update [dict get [lindex [r ACL LOG] 0] timestamp-last-updated]
         assert {$entry_id_lastest_error eq $entry_id_initial_error}
         assert {$timestamp_last_update_original < $timestamp_last_updated_after_update}
         assert {$timestamp_created_original eq $timestamp_created_updated}
         r ACL setuser user2 >doo
         assert_error "*WRONGPASS*" {r AUTH user2 foo}
         set new_error_entry_id [dict get [lindex [r ACL LOG] 0] entry-id]
         assert {$new_error_entry_id eq $entry_id_lastest_error + 1 }
    }

    test {ACL LOG shows failed command executions at toplevel} {
        r ACL LOG RESET
        r ACL setuser antirez >foo on +set ~object:1234
        r ACL setuser antirez +eval +multi +exec
        r ACL setuser antirez resetchannels +publish
        r AUTH antirez foo
        assert_error "*NOPERM*get*" {r GET foo}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {antirez}}
        assert {[dict get $entry context] eq {toplevel}}
        assert {[dict get $entry reason] eq {command}}
        assert {[dict get $entry object] eq {get}}
        assert_match {*cmd=get*} [dict get $entry client-info]
    }

    test "ACL LOG shows failed subcommand executions at toplevel" {
        r ACL LOG RESET
        r ACL DELUSER demo
        r ACL SETUSER demo on nopass
        r AUTH demo ""
        assert_error "*NOPERM*script|help*" {r SCRIPT HELP}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {demo}
        assert_equal [dict get $entry context] {toplevel}
        assert_equal [dict get $entry reason] {command}
        assert_equal [dict get $entry object] {script|help}
    }

    test {ACL LOG is able to test similar events} {
        r ACL LOG RESET
        r AUTH antirez foo
        catch {r GET foo}
        catch {r GET foo}
        catch {r GET foo}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry count] == 3}
    }

    test {ACL LOG is able to log keys access violations and key name} {
        r AUTH antirez foo
        catch {r SET somekeynotallowed 1234}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry reason] eq {key}}
        assert {[dict get $entry object] eq {somekeynotallowed}}
    }

    test {ACL LOG is able to log channel access violations and channel name} {
        r AUTH antirez foo
        catch {r PUBLISH somechannelnotallowed nullmsg}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry reason] eq {channel}}
        assert {[dict get $entry object] eq {somechannelnotallowed}}
    }

    test {ACL LOG RESET is able to flush the entries in the log} {
        r ACL LOG RESET
        assert {[llength [r ACL LOG]] == 0}
    }

    test {ACL LOG can distinguish the transaction context (1)} {
        r AUTH antirez foo
        r MULTI
        catch {r INCR foo}
        catch {r EXEC}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry context] eq {multi}}
        assert {[dict get $entry object] eq {incr}}
    }

    test {ACL LOG can distinguish the transaction context (2)} {
        set rd1 [redis_deferring_client]
        r ACL SETUSER antirez +incr

        r AUTH antirez foo
        r MULTI
        r INCR object:1234
        $rd1 ACL SETUSER antirez -incr
        $rd1 read
        catch {r EXEC}
        $rd1 close
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry context] eq {multi}}
        assert {[dict get $entry object] eq {incr}}
        assert_match {*cmd=exec*} [dict get $entry client-info]
        r ACL SETUSER antirez -incr
    }

    test {ACL can log errors in the context of Lua scripting} {
        r AUTH antirez foo
        catch {r EVAL {redis.call('incr','foo')} 0}
        r AUTH default ""
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry context] eq {lua}}
        assert {[dict get $entry object] eq {incr}}
        assert_match {*cmd=eval*} [dict get $entry client-info]
    }

    test {ACL LOG can accept a numerical argument to show less entries} {
        r AUTH antirez foo
        catch {r INCR foo}
        catch {r INCR foo}
        catch {r INCR foo}
        catch {r INCR foo}
        r AUTH default ""
        assert {[llength [r ACL LOG]] > 1}
        assert {[llength [r ACL LOG 2]] == 2}
    }

    test {ACL LOG can log failed auth attempts} {
        catch {r AUTH antirez wrong-password}
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry context] eq {toplevel}}
        assert {[dict get $entry reason] eq {auth}}
        assert {[dict get $entry object] eq {AUTH}}
        assert {[dict get $entry username] eq {antirez}}
    }

    test {ACLLOG - zero max length is correctly handled} {
        r ACL LOG RESET
        r CONFIG SET acllog-max-len 0
        for {set j 0} {$j < 10} {incr j} {
            catch {r SET obj:$j 123}
        }
        r AUTH default ""
        assert {[llength [r ACL LOG]] == 0}
    }

    test {ACL LOG entries are limited to a maximum amount} {
        r ACL LOG RESET
        r CONFIG SET acllog-max-len 5
        r AUTH antirez foo
        for {set j 0} {$j < 10} {incr j} {
            catch {r SET obj:$j 123}
        }
        r AUTH default ""
        assert {[llength [r ACL LOG]] == 5}
    }

    test {ACL LOG entries are still present on update of max len config} {
        r CONFIG SET acllog-max-len 0
        assert {[llength [r ACL LOG]] == 5}
    }

    test {When default user is off, new connections are not authenticated} {
        r ACL setuser default off
        catch {set rd1 [redis_deferring_client]} e
        r ACL setuser default on
        set e
    } {*NOAUTH*}

    test {When default user has no command permission, hello command still works for other users} {
        r ACL setuser secure-user >supass on +@all
        r ACL setuser default -@all
        r HELLO 2 AUTH secure-user supass
        r ACL setuser default nopass +@all
        r AUTH default ""
    }

    test {When an authentication chain is used in the HELLO cmd, the last auth cmd has precedence} {
        r ACL setuser secure-user1 >supass on +@all
        r ACL setuser secure-user2 >supass on +@all
        r HELLO 2 AUTH secure-user pass AUTH secure-user2 supass AUTH secure-user1 supass
        assert {[r ACL whoami] eq {secure-user1}}
        catch {r HELLO 2 AUTH secure-user supass AUTH secure-user2 supass AUTH secure-user pass} e
        assert_match "WRONGPASS invalid username-password pair or user is disabled." $e
        assert {[r ACL whoami] eq {secure-user1}}
    }

    test {When a setname chain is used in the HELLO cmd, the last setname cmd has precedence} {
        r HELLO 2 setname client1 setname client2 setname client3 setname client4
        assert {[r client getname] eq {client4}}
        catch {r HELLO 2 setname client5 setname client6 setname "client name"} e
        assert_match "ERR Client names cannot contain spaces, newlines or special characters." $e
        assert {[r client getname] eq {client4}}
    }

    test {When authentication fails in the HELLO cmd, the client setname should not be applied} {
        r client setname client0
        catch {r HELLO 2 AUTH user pass setname client1} e
        assert_match "WRONGPASS invalid username-password pair or user is disabled." $e
        assert {[r client getname] eq {client0}}
    }

    test {ACL HELP should not have unexpected options} {
        catch {r ACL help xxx} e
        assert_match "*wrong number of arguments for 'acl|help' command" $e
    }

    test {Delete a user that the client doesn't use} {
        r ACL setuser not_used on >passwd
        assert {[r ACL deluser not_used] == 1}
        # The client is not closed
        assert {[r ping] eq {PONG}}
    }

    test {Delete a user that the client is using} {
        r ACL setuser using on +acl >passwd
        r AUTH using passwd
        # The client will receive reply normally
        assert {[r ACL deluser using] == 1}
        # The client is closed
        catch {[r ping]} e
        assert_match "*I/O error*" $e
    }

    test {ACL GENPASS command failed test} {
       catch {r ACL genpass -236} err1
       catch {r ACL genpass 5000} err2
       assert_match "*ACL GENPASS argument must be the number*" $err1
       assert_match "*ACL GENPASS argument must be the number*" $err2
    }

    test {Default user can not be removed} {
       catch {r ACL deluser default} err
       set err
    } {ERR The 'default' user cannot be removed}

    test {ACL load non-existing configured ACL file} {
       catch {r ACL load} err
       set err
    } {*Redis instance is not configured to use an ACL file*}

    # If there is an AUTH failure the metric increases
    test {ACL-Metrics user AUTH failure} {
        set current_auth_failures [s acl_access_denied_auth]
        set current_invalid_cmd_accesses [s acl_access_denied_cmd]
        set current_invalid_key_accesses [s acl_access_denied_key]
        set current_invalid_channel_accesses [s acl_access_denied_channel]
        assert_error "*WRONGPASS*" {r AUTH notrealuser 1233456} 
        assert {[s acl_access_denied_auth] eq [expr $current_auth_failures + 1]}
        assert_error "*WRONGPASS*" {r HELLO 3 AUTH notrealuser 1233456}
        assert {[s acl_access_denied_auth] eq [expr $current_auth_failures + 2]}
        assert_error "*WRONGPASS*" {r HELLO 2 AUTH notrealuser 1233456}
        assert {[s acl_access_denied_auth] eq [expr $current_auth_failures + 3]}
        assert {[s acl_access_denied_cmd] eq $current_invalid_cmd_accesses}
        assert {[s acl_access_denied_key] eq $current_invalid_key_accesses}
        assert {[s acl_access_denied_channel] eq $current_invalid_channel_accesses}
    }

    # If a user try to access an unauthorized command the metric increases
    test {ACL-Metrics invalid command accesses} {
        set current_auth_failures [s acl_access_denied_auth]
        set current_invalid_cmd_accesses [s acl_access_denied_cmd]
        set current_invalid_key_accesses [s acl_access_denied_key]
        set current_invalid_channel_accesses [s acl_access_denied_channel]
        r ACL setuser invalidcmduser on >passwd nocommands
        r AUTH invalidcmduser passwd
        assert_error "*no permissions to run the * command*" {r acl list}
        r AUTH default ""
        assert {[s acl_access_denied_auth] eq $current_auth_failures}
        assert {[s acl_access_denied_cmd] eq [expr $current_invalid_cmd_accesses + 1]}
        assert {[s acl_access_denied_key] eq $current_invalid_key_accesses}
        assert {[s acl_access_denied_channel] eq $current_invalid_channel_accesses}
    }

    # If a user try to access an unauthorized key the metric increases
    test {ACL-Metrics invalid key accesses} {
        set current_auth_failures [s acl_access_denied_auth]
        set current_invalid_cmd_accesses [s acl_access_denied_cmd]
        set current_invalid_key_accesses [s acl_access_denied_key]
        set current_invalid_channel_accesses [s acl_access_denied_channel]
        r ACL setuser invalidkeyuser on >passwd resetkeys allcommands
        r AUTH invalidkeyuser passwd
        assert_error "*NOPERM*key*" {r get x}
        r AUTH default ""
        assert {[s acl_access_denied_auth] eq $current_auth_failures}
        assert {[s acl_access_denied_cmd] eq $current_invalid_cmd_accesses}
        assert {[s acl_access_denied_key] eq [expr $current_invalid_key_accesses + 1]}
        assert {[s acl_access_denied_channel] eq $current_invalid_channel_accesses}
    }   

    # If a user try to access an unauthorized channel the metric increases
    test {ACL-Metrics invalid channels accesses} {
        set current_auth_failures [s acl_access_denied_auth]
        set current_invalid_cmd_accesses [s acl_access_denied_cmd]
        set current_invalid_key_accesses [s acl_access_denied_key]
        set current_invalid_channel_accesses [s acl_access_denied_channel]
        r ACL setuser invalidchanneluser on >passwd resetchannels allcommands
        r AUTH invalidkeyuser passwd
        assert_error "*NOPERM*channel*" {r subscribe x}
        r AUTH default ""
        assert {[s acl_access_denied_auth] eq $current_auth_failures}
        assert {[s acl_access_denied_cmd] eq $current_invalid_cmd_accesses}
        assert {[s acl_access_denied_key] eq $current_invalid_key_accesses}
        assert {[s acl_access_denied_channel] eq [expr $current_invalid_channel_accesses + 1]}
    }
}

set server_path [tmpdir "server.acl"]
exec cp -f tests/assets/user.acl $server_path
start_server [list overrides [list "dir" $server_path "acl-pubsub-default" "allchannels" "aclfile" "user.acl"] tags [list "external:skip"]] {
    # user alice on allcommands allkeys &* >alice
    # user bob on -@all +@set +acl ~set* &* >bob
    # user default on nopass ~* &* +@all

    test {default: load from include file, can access any channels} {
        r SUBSCRIBE foo
        r PSUBSCRIBE bar*
        r UNSUBSCRIBE
        r PUNSUBSCRIBE
        r PUBLISH hello world
    }

    test {default: with config acl-pubsub-default allchannels after reset, can access any channels} {
        r ACL setuser default reset on nopass ~* +@all
        r SUBSCRIBE foo
        r PSUBSCRIBE bar*
        r UNSUBSCRIBE
        r PUNSUBSCRIBE
        r PUBLISH hello world
    }

    test {default: with config acl-pubsub-default resetchannels after reset, can not access any channels} {
        r CONFIG SET acl-pubsub-default resetchannels
        r ACL setuser default reset on nopass ~* +@all
        assert_error {*NOPERM*channel*} {r SUBSCRIBE foo}
        assert_error {*NOPERM*channel*} {r PSUBSCRIBE bar*}
        assert_error {*NOPERM*channel*} {r PUBLISH hello world}
        r CONFIG SET acl-pubsub-default resetchannels
    }

    test {Alice: can execute all command} {
        r AUTH alice alice
        assert_equal "alice" [r acl whoami]
        r SET key value
    }

    test {Bob: just execute @set and acl command} {
        r AUTH bob bob
        assert_equal "bob" [r acl whoami]
        assert_equal "3" [r sadd set 1 2 3]
        catch {r SET key value} e
        set e
    } {*NOPERM*set*}

    test {ACL LOAD only disconnects affected clients} {
        reconnect
        r ACL SETUSER doug on nopass resetchannels &test* +@all ~*

        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 AUTH alice alice
        $rd1 read
        $rd1 SUBSCRIBE test1
        $rd1 read

        $rd2 AUTH doug doug
        $rd2 read
        $rd2 SUBSCRIBE test1
        $rd2 read

        r ACL LOAD
        r PUBLISH test1 test-message

        # Permissions for 'alice' haven't changed, so they should still be connected
        assert_match {*test-message*} [$rd1 read]

        # 'doug' no longer has access to "test1" channel, so they should get disconnected
        catch {$rd2 read} e
        assert_match {*I/O error*} $e

        $rd1 close
        $rd2 close
    }

    test {ACL LOAD disconnects clients of deleted users} {
        reconnect
        r ACL SETUSER mortimer on >mortimer ~* &* +@all

        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 AUTH alice alice
        $rd1 read
        $rd1 SUBSCRIBE test
        $rd1 read

        $rd2 AUTH mortimer mortimer
        $rd2 read
        $rd2 SUBSCRIBE test
        $rd2 read

        r ACL LOAD
        r PUBLISH test test-message

        # Permissions for 'alice' haven't changed, so they should still be connected
        assert_match {*test-message*} [$rd1 read]

        # 'mortimer' has been deleted, so their client should get disconnected
        catch {$rd2 read} e
        assert_match {*I/O error*} $e

        $rd1 close
        $rd2 close
    }

    test {ACL load and save} {
        r ACL setuser eve +get allkeys >eve on
        r ACL save

        r ACL load

        # Clients should not be disconnected since permissions haven't changed

        r AUTH alice alice
        r SET key value
        r AUTH eve eve
        r GET key
        catch {r SET key value} e
        set e
    } {*NOPERM*set*}

    test {ACL load and save with restricted channels} {
        r AUTH alice alice
        r ACL setuser harry on nopass resetchannels &test +@all ~*
        r ACL save

        r ACL load

        # Clients should not be disconnected since permissions haven't changed

        r AUTH harry anything
        r publish test bar
        catch {r publish test1 bar} e
        r ACL deluser harry
        set e
    } {*NOPERM*channel*}
}

set server_path [tmpdir "resetchannels.acl"]
exec cp -f tests/assets/nodefaultuser.acl $server_path
exec cp -f tests/assets/default.conf $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "nodefaultuser.acl"] tags [list "external:skip"]] {

    test {Default user has access to all channels irrespective of flag} {
        set channelinfo [dict get [r ACL getuser default] channels]
        assert_equal "&*" $channelinfo
        set channelinfo [dict get [r ACL getuser alice] channels]
        assert_equal "" $channelinfo
    }

    test {Update acl-pubsub-default, existing users shouldn't get affected} {
        set channelinfo [dict get [r ACL getuser default] channels]
        assert_equal "&*" $channelinfo
        r CONFIG set acl-pubsub-default allchannels
        r ACL setuser mydefault
        set channelinfo [dict get [r ACL getuser mydefault] channels]
        assert_equal "&*" $channelinfo
        r CONFIG set acl-pubsub-default resetchannels
        set channelinfo [dict get [r ACL getuser mydefault] channels]
        assert_equal "&*" $channelinfo
    }

    test {Single channel is valid} {
        r ACL setuser onechannel &test
        set channelinfo [dict get [r ACL getuser onechannel] channels]
        assert_equal "&test" $channelinfo
        r ACL deluser onechannel
    }

    test {Single channel is not valid with allchannels} {
        r CONFIG set acl-pubsub-default allchannels
        catch {r ACL setuser onechannel &test} err
        r CONFIG set acl-pubsub-default resetchannels
        set err
    } {*start with an empty list of channels*}
}

set server_path [tmpdir "resetchannels.acl"]
exec cp -f tests/assets/nodefaultuser.acl $server_path
exec cp -f tests/assets/default.conf $server_path
start_server [list overrides [list "dir" $server_path "acl-pubsub-default" "resetchannels" "aclfile" "nodefaultuser.acl"] tags [list "external:skip"]] {

    test {Only default user has access to all channels irrespective of flag} {
        set channelinfo [dict get [r ACL getuser default] channels]
        assert_equal "&*" $channelinfo
        set channelinfo [dict get [r ACL getuser alice] channels]
        assert_equal "" $channelinfo
    }
}


start_server {overrides {user "default on nopass ~* +@all"} tags {"external:skip"}} {
    test {default: load from config file, without channel permission default user can't access any channels} {
        catch {r SUBSCRIBE foo} e
        set e
    } {*NOPERM*channel*}
}

start_server {overrides {user "default on nopass ~* &* +@all"} tags {"external:skip"}} {
    test {default: load from config file with all channels permissions} {
        r SUBSCRIBE foo
        r PSUBSCRIBE bar*
        r UNSUBSCRIBE
        r PUNSUBSCRIBE
        r PUBLISH hello world
    }
}

set server_path [tmpdir "duplicate.acl"]
exec cp -f tests/assets/user.acl $server_path
exec cp -f tests/assets/default.conf $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "user.acl"] tags [list "external:skip"]] {

    test {Test loading an ACL file with duplicate users} {
        exec cp -f tests/assets/user.acl $server_path

        # Corrupt the ACL file
        set corruption "\nuser alice on nopass ~* -@all"
        exec echo $corruption >> $server_path/user.acl
        catch {r ACL LOAD} err
        assert_match {*Duplicate user 'alice' found*} $err 

        # Verify the previous users still exist
        # NOTE: A missing user evaluates to an empty
        # string. 
        assert {[r ACL GETUSER alice] != ""}
        assert_equal [dict get [r ACL GETUSER alice] commands] "+@all"
        assert {[r ACL GETUSER bob] != ""}
        assert {[r ACL GETUSER default] != ""}
    }

    test {Test loading an ACL file with duplicate default user} {
        exec cp -f tests/assets/user.acl $server_path

        # Corrupt the ACL file
        set corruption "\nuser default on nopass ~* -@all"
        exec echo $corruption >> $server_path/user.acl
        catch {r ACL LOAD} err
        assert_match {*Duplicate user 'default' found*} $err 

        # Verify the previous users still exist
        # NOTE: A missing user evaluates to an empty
        # string. 
        assert {[r ACL GETUSER alice] != ""}
        assert_equal [dict get [r ACL GETUSER alice] commands] "+@all"
        assert {[r ACL GETUSER bob] != ""}
        assert {[r ACL GETUSER default] != ""}
    }
    
    test {Test loading duplicate users in config on startup} {
        catch {exec src/redis-server --user foo --user foo} err
        assert_match {*Duplicate user*} $err

        catch {exec src/redis-server --user default --user default} err
        assert_match {*Duplicate user*} $err
    } {} {external:skip}
}

start_server {overrides {user "default on nopass ~* +@all -flushdb"} tags {acl external:skip}} {
    test {ACL from config file and config rewrite} {
        assert_error {NOPERM *} {r flushdb}
        r config rewrite
        restart_server 0 true false
        assert_error {NOPERM *} {r flushdb}
    }
}

