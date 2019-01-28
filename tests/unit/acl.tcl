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
}
