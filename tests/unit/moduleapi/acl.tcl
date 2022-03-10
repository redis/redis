set testmodule [file normalize tests/modules/acl.so]

start_server {tags {"modules acl"}} {
    r module load $testmodule

    test {test module dump/reload acls} {
        # create user
        assert_equal [r acl setuser test on allkeys allcommands >test ] OK
        # verify user
        assert_not_equal [r acl getuser test] {_}
        # dump acls
        set acls [r acl.dump]
        # delete user
        assert_equal [r acl deluser test ] 1
        # verify user deleted
        assert_equal [r acl getuser test] ""
        # reload acls
        r acl.load acls
        # verify user exists
        assert_equal [r acl getuser test] "hello"
    }
}