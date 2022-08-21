set testmodule [file normalize tests/modules/dryrun.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {test Dry Run - OK OOM} {
        set val [r get x]
        assert_equal [r dryrun set x 5] OK
        assert_equal [r get x] $val
    }

    test {test Dry Run - OK ACL} {
        set val [r get x]
        assert_equal [r dryrun set x 5] OK
        assert_equal [r get x] $val
    }

    test {test Dry Run - Fail OOM} {
        set val [r get x]
        r config set maxmemory 1
        catch {r dryrun set x 5} e
        assert_match {*OOM*} $e
        r config set maxmemory 100000000000
        assert_equal [r get x] $val
    }

    test {test Dry Run - Fail ACL} {
        set val [r get x]
        # deny all permissions besides the dryrun command
        r acl setuser default +get -set +dryrun resetkeys ~x

        catch {r dryrun set x 5} e
        assert_match {*ERR acl verification failed, can't run this command or subcommand*} $e
        assert_equal [r get x] $val
    }
}
