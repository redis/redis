set testmodule [file normalize tests/modules/persistentclient.so]

start_server {tags {"modules persistentclient"}} {
    r module load $testmodule

    test {test basic persistent client exec} {
        r del x
        r mc.create
        r set x 1
        assert_equal [r mc.exec get x] 1
        r mc.delete
    }

    test {test persistent client watch} {
        r del x
        r mc.create
        r mc.exec watch x
        r set x 1
        assert_equal [r mc.getflags] 1
        r mc.delete
    }

    test {test persistent client multi/exec watch} {
        r set x 1
        r mc.create
        r mc.exec watch x
        r mc.exec multi
        r mc.exec get x
        r mc.exec get x
        assert { [r mc.exec exec] == {1 1} }
        r mc.delete
    }

    test {test persistent client multi/exec with watch failure} {
        r set x 1
        r mc.create
        r mc.exec watch x
        r mc.exec multi
        r mc.exec get x
        r mc.exec get x
        r set x 2
        assert { [r mc.exec exec] == {} }
        r mc.delete
    }
}