start_server {tags {"acl external:skip"}} {
    set r2 [redis_client]
    test {Test basic multiple selectors} {
        r ACL SETUSER selector-1 on -@all resetkeys nopass
        $r2 auth selector-1 password
        catch {$r2 ping} err
        assert_match "*NOPERM*command*" $err
        catch {$r2 set write::foo bar} err
        assert_match "*NOPERM*command*" $err
        catch {$r2 get read::foo} err
        assert_match "*NOPERM*command*" $err

        r ACL SETUSER selector-1 (+@write ~write::*) (+@read ~read::*)
        catch {$r2 ping} err
        assert_equal "OK" [$r2 set write::foo bar]
        assert_equal "" [$r2 get read::foo]
        catch {$r2 get write::foo} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 set read::foo bar} err
        assert_match "*NOPERM*keys*" $err
    }

    test {Test ACL selectors by default have no permissions (except channels)} {
        r ACL SETUSER selector-default reset ()
        set user [r ACL GETUSER "selector-default"]
        assert_equal 2 [llength [dict get $user selectors]]
        assert_equal 0 [llength [dict get [lindex [dict get $user selectors] 0] keys]]
        assert_equal "*" [lindex [dict get [lindex [dict get $user selectors] 0] channels] 0]
        assert_equal  1 [llength [dict get [lindex [dict get $user selectors] 0] channels]]
        assert_equal "-@all" [dict get [lindex [dict get $user selectors] 0] commands]
    }

    test {Test deleting selectors} {
        r ACL SETUSER selector-del on ~root-selector "(~added-selector)"
        set user [r ACL GETUSER "selector-del"]
        assert_equal "~added-selector" [dict get [lindex [dict get $user selectors] 1] keys]
        assert_equal "~root-selector" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal [llength [dict get $user selectors]] 2

        r ACL SETUSER selector-del clearselectors
        set user [r ACL GETUSER "selector-del"]
        assert_equal [llength [dict get $user selectors]] 1
        assert_equal "~root-selector" [dict get [lindex [dict get $user selectors] 0] keys]
    }

    test {Test selector syntax error reports the error in the selector context} {
        catch {r ACL SETUSER selector-syntax on (this-is-invalid)} e
        assert_match "*ERR Error in ACL SETUSER modifier '(*)*Syntax*" $e

        catch {r ACL SETUSER selector-syntax on (&fail)} e
        assert_match "*ERR Error in ACL SETUSER modifier '(*)*Adding a pattern after the*" $e

        assert_equal "" [r ACL GETUSER selector-syntax]
    }

    test {Test flexible selector definition} {
        # Test valid selectors
        r ACL SETUSER selector-2 "(~key1 +get )" "( ~key2 +get )" "( ~key3 +get)" "(~key4 +get)"
        r ACL SETUSER selector-2 (~key5 +get ) ( ~key6 +get ) ( ~key7 +get) (~key8 +get)
        set user [r ACL GETUSER "selector-2"]
        assert_equal "" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal "~key1" [dict get [lindex [dict get $user selectors] 1] keys]
        assert_equal "~key2" [dict get [lindex [dict get $user selectors] 2] keys]
        assert_equal "~key3" [dict get [lindex [dict get $user selectors] 3] keys]
        assert_equal "~key4" [dict get [lindex [dict get $user selectors] 4] keys]
        assert_equal "~key5" [dict get [lindex [dict get $user selectors] 5] keys]
        assert_equal "~key6" [dict get [lindex [dict get $user selectors] 6] keys]
        assert_equal "~key7" [dict get [lindex [dict get $user selectors] 7] keys]
        assert_equal "~key8" [dict get [lindex [dict get $user selectors] 8] keys]

        # Test invalid selector syntax
        catch {r ACL SETUSER invalid-selector " () "} err
        assert_match "*ERR*Syntax error*" $err
        catch {r ACL SETUSER invalid-selector (} err
        assert_match "*Unmatched parenthesis*" $err
        catch {r ACL SETUSER invalid-selector )} err
        assert_match "*ERR*Syntax error" $err
    }

    test {Test separate read permission} {
        r ACL SETUSER key-permission-R on nopass %R~read* +@all
        $r2 auth key-permission-R password
        assert_equal PONG [$r2 PING]
        r set readstr bar
        assert_equal bar [$r2 get readstr]
        catch {$r2 set readstr bar} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 get notread} err
        assert_match "*NOPERM*keys*" $err
    }

    test {Test separate write permission} {
        r ACL SETUSER key-permission-W on nopass %W~write* +@all
        $r2 auth key-permission-W password
        assert_equal PONG [$r2 PING]
        # Note, SET is a RW command, so it's not used for testing
        $r2 LPUSH writelist 10
        catch {$r2 GET writestr} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 LPUSH notwrite 10} err
        assert_match "*NOPERM*keys*" $err
    }

    test {Test separate read and write permissions} {
        r ACL SETUSER key-permission-RW on nopass %R~read* %W~write* +@all
        $r2 auth key-permission-RW password
        assert_equal PONG [$r2 PING]
        r set read bar
        $r2 copy read write
        catch {$r2 copy write read} err
        assert_match "*NOPERM*keys*" $err        
    }

    test {Test separate read and write permissions on different selectors are not additive} {
        r ACL SETUSER key-permission-RW-selector on nopass "(%R~read* +@all)" "(%W~write* +@all)"
        $r2 auth key-permission-RW-selector password
        assert_equal PONG [$r2 PING]

        # Verify write selector
        $r2 LPUSH writelist 10
        catch {$r2 GET writestr} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 LPUSH notwrite 10} err
        assert_match "*NOPERM*keys*" $err

        # Verify read selector
        r set readstr bar
        assert_equal bar [$r2 get readstr]
        catch {$r2 set readstr bar} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 get notread} err
        assert_match "*NOPERM*keys*" $err

        # Verify they don't combine
        catch {$r2 copy read write} err
        assert_match "*NOPERM*keys*" $err
        catch {$r2 copy write read} err
        assert_match "*NOPERM*keys*" $err   
    }

    test {Test ACL log correctly identifies the relevant item when selectors are used} {
        r ACL SETUSER acl-log-test-selector on nopass 
        r ACL SETUSER acl-log-test-selector +mget ~key (+mget ~key ~otherkey)
        $r2 auth acl-log-test-selector password

        # Test that command is shown only if none of the selectors match
        r ACL LOG RESET
        catch {$r2 GET key} err
        assert_match "*NOPERM*command*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "command"
        assert_equal [dict get $entry object] "get"

        # Test two cases where the first selector matches less than the
        # second selector. We should still show the logically first unmatched key.
        r ACL LOG RESET
        catch {$r2 MGET otherkey someotherkey} err
        assert_match "*NOPERM*keys*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "key"
        assert_equal [dict get $entry object] "someotherkey"

        r ACL LOG RESET
        catch {$r2 MGET key otherkey someotherkey} err
        assert_match "*NOPERM*keys*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "key"
        assert_equal [dict get $entry object] "someotherkey"
    }

    test {Test ACL GETUSER response information} {
        r ACL setuser selector-info -@all +get resetchannels &channel1 %R~foo1 %W~bar1 ~baz1 
        r ACL setuser selector-info (-@all +set resetchannels &channel2 %R~foo2 %W~bar2 ~baz2)
        set user [r ACL GETUSER "selector-info"]
    
        # Root selector
        assert_equal "foo1" [lindex [dict get $user keys] 0]
        assert_equal "bar1" [lindex [dict get $user keys] 1]
        assert_equal "baz1" [lindex [dict get $user keys] 2]
        assert_equal "channel1" [lindex [dict get $user channels] 0]
        assert_equal "-@all +get" [dict get $user commands]

        set root_selector [lindex [dict get $user selectors] 0]
        assert_equal "%R~foo1" [lindex [dict get $root_selector keys] 0]
        assert_equal "%W~bar1" [lindex [dict get $root_selector keys] 1]
        assert_equal "~baz1" [lindex [dict get $root_selector keys] 2]
        assert_equal "&channel1" [lindex [dict get $root_selector channels] 0]
        assert_equal "-@all +get" [dict get $root_selector commands]

        # Added selector
        set secondary_selector [lindex [dict get $user selectors] 1]
        assert_equal "%R~foo2" [lindex [dict get $secondary_selector keys] 0]
        assert_equal "%W~bar2" [lindex [dict get $secondary_selector keys] 1]
        assert_equal "~baz2" [lindex [dict get $secondary_selector keys] 2]
        assert_equal "&channel2" [lindex [dict get $secondary_selector channels] 0]
        assert_equal "-@all +set" [dict get $secondary_selector commands] 
    }

    test {Test ACL list idempotency} {
        r ACL SETUSER user-idempotency off -@all +get resetchannels &channel1 %R~foo1 %W~bar1 ~baz1 (-@all +set resetchannels &channel2 %R~foo2 %W~bar2 ~baz2)
        set response [lindex [r ACL LIST] [lsearch [r ACL LIST] "user user-idempotency*"]]

        assert_match "*-@all*+get*(*)*" $response
        assert_match "*resetchannels*&channel1*(*)*" $response
        assert_match "*%R~foo1*%W~bar1*~baz1*(*)*" $response

        assert_match "*(*-@all*+set*)*" $response
        assert_match "*(*resetchannels*&channel2*)*" $response
        assert_match "*(*%R~foo2*%W~bar2*~baz2*)*" $response
    }

    test {Test R+W is the same as all permissions} {
        r ACL setuser selector-rw-info %R~foo %W~foo %RW~bar
        set user [r ACL GETUSER selector-rw-info]
        assert_equal "~foo" [lindex [dict get [lindex [dict get $user selectors] 0] keys] 0]
        assert_equal "~bar" [lindex [dict get [lindex [dict get $user selectors] 0] keys] 1]
    }

    test {Test basic dry run functionality} {
        r ACL setuser command-test +@all %R~read* %W~write* %RW~rw*
        assert_equal "OK" [r ACL DRYRUN command-test GET read]

        catch {r ACL DRYRUN not-a-user GET read} e
        assert_equal "ERR User 'not-a-user' not found" $e

        catch {r ACL DRYRUN command-test not-a-command read} e
        assert_equal "ERR Command 'not-a-command' not found" $e
    }

    test {Test various odd commands for key permissions} {
        r ACL setuser command-test +@all %R~read* %W~write* %RW~rw*

        # Test migrate, which is marked with incomplete keys
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever rw]
        assert_equal "This user has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever read]
        assert_equal "This user has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever write]
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS rw]
        assert_equal "This user has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS read]
        assert_equal "This user has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS write]

        # Test SORT, which is marked with incomplete keys
        assert_equal "OK" [r ACL DRYRUN command-test SORT read STORE write]
        assert_equal "This user has no permissions to access the 'read' key"  [r ACL DRYRUN command-test SORT read STORE read]
        assert_equal "This user has no permissions to access the 'write' key"  [r ACL DRYRUN command-test SORT write STORE write]

        # Test EVAL, which uses the numkey keyspec (Also test EVAL_RO)
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 1 rw1]
        assert_equal "This user has no permissions to access the 'read' key" [r ACL DRYRUN command-test EVAL "" 1 read]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL_RO "" 1 rw1]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL_RO "" 1 read]

        # Read is an optional argument and not a key here, make sure we don't treat it as a key
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 0 read]

        # These are syntax errors, but it's 'OK' from an ACL perspective
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" -1 read]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 3 rw rw]

        # Also a syntax error, but we do try to find keys so this will fail
        assert_equal "This user has no permissions to access the 'read' key" [r ACL DRYRUN command-test EVAL "" 2147483647 rw read]

        # Test GEORADIUS which uses the last type of keyspec, keyword
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M STOREDIST write]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M]
        assert_equal "This user has no permissions to access the 'read2' key" [r ACL DRYRUN command-test GEORADIUS read1 longitude latitude radius M STOREDIST read2]
        assert_equal "This user has no permissions to access the 'write1' key" [r ACL DRYRUN command-test GEORADIUS write1 longitude latitude radius M STOREDIST write2]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M STORE write]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M]
        assert_equal "This user has no permissions to access the 'read2' key" [r ACL DRYRUN command-test GEORADIUS read1 longitude latitude radius M STORE read2]
        assert_equal "This user has no permissions to access the 'write1' key" [r ACL DRYRUN command-test GEORADIUS write1 longitude latitude radius M STORE write2]
    }

    $r2 close
}

set server_path [tmpdir "selectors.acl"]
exec cp -f tests/assets/userwithselectors.acl $server_path
exec cp -f tests/assets/default.conf $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "userwithselectors.acl"] tags [list "external:skip"]] {

    test {Only default user has access to all channels irrespective of flag} {
        set selectors [dict get [r ACL getuser alice] selectors]
        assert_equal [llength $selectors] 2

        set selectors [dict get [r ACL getuser bob] selectors]
        assert_equal [llength $selectors] 3
    }
}
