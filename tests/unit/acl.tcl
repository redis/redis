start_server {tags {"acl"}} {
    test {Connections start with the default user} {
        r ACL WHOAMI
    } {default}

    test {It is possible to create new users} {
        r ACL setuser newuser
    }

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

    test {By default users are not able to access any command} {
        catch {r SET foo bar} e
        set e
    } {*NOPERM*}

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

    test {Users can be configured to authenticate with any password} {
        r ACL setuser newuser nopass
        r AUTH newuser zipzapblabla
    } {OK}

    test {ACLs can exclude single commands} {
        r ACL setuser newuser -ping
        r INCR mycounter ; # Should not raise an error
        catch {r PING} e
        set e
    } {*NOPERM*}

    test {ACLs can include or excluse whole classes of commands} {
        r ACL setuser newuser -@all +@set +acl
        r SADD myset a b c; # Should not raise an error
        r ACL setuser newuser +@all -@string
        r SADD myset a b c; # Again should not raise an error
        # String commands instead should raise an error
        catch {r SET foo bar} e
        r ACL setuser newuser allcommands; # Undo commands ACL
        set e
    } {*NOPERM*}
}
