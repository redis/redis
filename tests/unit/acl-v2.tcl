# Returns a parsed GETUSER response using the provided
# client and username
proc get_user {client username} {
    set response [$client ACL GETUSER $username]

    # Verify structure of the GETUSER response
    assert_equal [llength $response] 12
    assert_equal [lindex $response 0] "flags"
    assert_equal [lindex $response 2] "passwords"
    assert_equal [lindex $response 4] "commands"
    assert_equal [lindex $response 6] "keys"
    assert_equal [lindex $response 8] "channels"
    assert_equal [lindex $response 10] "selectors" 

    set selectors {}
    foreach sl [lindex $response 11]  {
        # Verify structure of the single selector response
        assert_equal [lindex $sl 0] "commands"
        assert_equal [lindex $sl 2] "keys"
        assert_equal [lindex $sl 4] "channels"

        # Append the selector
        set selector [dict create \
            "commands" [lindex $sl 1] \
            "keys" [lindex $sl 3] \
            "channels" [lindex $sl 5] \
        ]
        lappend selectors $selector 
    }


    set user [dict create \
        "flags" [lindex $response 1] \
        "passwords" [lindex $response 3] \
        "commands" [lindex $response 5] \
        "keys" [lindex $response 7] \
        "channels" [lindex $response 9] \
        "selectors" $selectors \
    ]
    return $user
}


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

    test {Test flexible selector definition} {
        # Test valid selectors
        r ACL SETUSER selector-2 "(~key1 +get )" "( ~key2 +get )" "( ~key3 +get)" "(~key4 +get)"
        r ACL SETUSER selector-2 (~key5 +get ) ( ~key6 +get ) ( ~key7 +get) (~key8 +get)
        set user [get_user r "selector-2"]
        assert_equal "" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal "key1" [dict get [lindex [dict get $user selectors] 1] keys]
        assert_equal "key2" [dict get [lindex [dict get $user selectors] 2] keys]
        assert_equal "key3" [dict get [lindex [dict get $user selectors] 3] keys]
        assert_equal "key4" [dict get [lindex [dict get $user selectors] 4] keys]
        assert_equal "key5" [dict get [lindex [dict get $user selectors] 5] keys]
        assert_equal "key6" [dict get [lindex [dict get $user selectors] 6] keys]
        assert_equal "key7" [dict get [lindex [dict get $user selectors] 7] keys]
        assert_equal "key8" [dict get [lindex [dict get $user selectors] 8] keys]

        # Test invalid selector
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

    test {Test ACL log correctly identifies the failed item when selectors are used} {
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

    $r2 close
}
