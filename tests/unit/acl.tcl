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
        r ACL setuser newuser on
        r AUTH newuser passwd1
    } {OK}
}
