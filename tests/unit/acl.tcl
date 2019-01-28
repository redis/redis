start_server {tags {"acl"}} {
    test {Connections start with the default user} {
        r ACL WHOAMI
    } {default}
}
