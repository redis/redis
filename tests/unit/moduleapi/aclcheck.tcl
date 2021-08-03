set testmodule [file normalize tests/modules/aclcheck.so]

start_server {tags {"modules acl"}} {
    r module load $testmodule

    test {test module check acl for command perm} {
        # block SET command for user
        r acl setuser default -set
        catch {r set.aclcheck.cmd x 5} e
        set e
    } {*DENIED CMD*}

    test {test module check acl for key perm} {
        # give permission for SET and block all keys but x
        r acl setuser default +set resetkeys ~x
        assert_equal [r set.aclcheck.key x 5] OK
        catch {r set.aclcheck.key y 5} e
        set e
    } {*DENIED KEY*}

    test {test module check acl for channel perm} {
        # block all channels but ch1
        r acl setuser default resetchannels &ch1
        assert_equal [r publish.aclcheck.channel ch1 msg] 0
        catch {r publish.aclcheck.channel ch2 msg} e
        set e
    } {*DENIED CHANNEL*}

    test {test module check acl in rm_call} {
        # rm call check for key permission
        catch {r rm_call.aclcheck set y 5} e
        assert_match {*NOPERM*} $e
        # rm call check for command permission
        r acl setuser default -set
        catch {r rm_call.aclcheck set x 5} e
        set e
    } {*NOPERM*}
}
