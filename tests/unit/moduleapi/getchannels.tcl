set testmodule [file normalize tests/modules/getchannels.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    # Channels are currently used to just validate ACLs, so test them here
    r ACL setuser testuser +@all resetchannels &channel &pattern*

    test "module getchannels-api with literals - ACL" {
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command subscribe literal channel subscribe literal pattern1]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command publish literal channel publish literal pattern1]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe literal channel unsubscribe literal pattern1]

        assert_equal "This user has no permissions to access the 'nopattern1' channel" [r ACL DRYRUN testuser getchannels.command subscribe literal channel subscribe literal nopattern1]
        assert_equal "This user has no permissions to access the 'nopattern1' channel" [r ACL DRYRUN testuser getchannels.command publish literal channel subscribe literal nopattern1]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe literal channel unsubscribe literal nopattern1]

        assert_equal "This user has no permissions to access the 'otherchannel' channel" [r ACL DRYRUN testuser getchannels.command subscribe literal otherchannel subscribe literal pattern1]
        assert_equal "This user has no permissions to access the 'otherchannel' channel" [r ACL DRYRUN testuser getchannels.command publish literal otherchannel subscribe literal pattern1]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe literal otherchannel unsubscribe literal pattern1]
    }

    test "module getchannels-api with patterns - ACL" {
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command subscribe pattern pattern*]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command publish pattern pattern*]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe pattern pattern*]

        assert_equal "This user has no permissions to access the 'pattern1' channel" [r ACL DRYRUN testuser getchannels.command subscribe pattern pattern1 subscribe pattern pattern*]
        assert_equal "This user has no permissions to access the 'pattern1' channel" [r ACL DRYRUN testuser getchannels.command publish pattern pattern1 subscribe pattern pattern*]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe pattern pattern1 unsubscribe pattern pattern*]

        assert_equal "This user has no permissions to access the 'otherpattern*' channel" [r ACL DRYRUN testuser getchannels.command subscribe pattern otherpattern* subscribe pattern pattern*]
        assert_equal "This user has no permissions to access the 'otherpattern*' channel" [r ACL DRYRUN testuser getchannels.command publish pattern otherpattern* subscribe pattern pattern*]
        assert_equal "OK" [r ACL DRYRUN testuser getchannels.command unsubscribe pattern otherpattern* unsubscribe pattern pattern*]
    }

    test "Unload the module - getchannels" {
        assert_equal {OK} [r module unload getchannels]
    }
}
