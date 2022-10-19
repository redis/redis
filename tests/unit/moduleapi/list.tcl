set testmodule [file normalize tests/modules/list.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module list set, get, insert, delete} {
        r del k
        r rpush k x
        # insert, set, get
        r list.insert k 0 foo
        r list.insert k -1 bar
        r list.set k 1 xyz
        assert_equal {foo xyz bar} [r list.getall k]
        assert_equal {foo} [r list.get k 0]
        assert_equal {xyz} [r list.get k 1]
        assert_equal {bar} [r list.get k 2]
        assert_equal {bar} [r list.get k -1]
        assert_equal {foo} [r list.get k -3]
        assert_error {ERR index out*} {r list.get k -4}
        assert_error {ERR index out*} {r list.get k 3}
        # remove
        assert_error {ERR index out*} {r list.delete k -4}
        assert_error {ERR index out*} {r list.delete k 3}
        r list.delete k 0
        r list.delete k -1
        assert_equal {xyz} [r list.getall k]
        # removing the last element deletes the list
        r list.delete k 0
        assert_equal 0 [r exists k]
    }

    test {Module list iteration} {
        r del k
        r rpush k x y z
        assert_equal {x y z} [r list.getall k]
        assert_equal {z y x} [r list.getall k REVERSE]
    }

    test {Module list insert & delete} {
        r del k
        r rpush k x y z
        r list.edit k ikikdi foo bar baz
        r list.getall k
    } {foo x bar y baz}

    test {Module list insert & delete, neg index} {
        r del k
        r rpush k x y z
        r list.edit k REVERSE ikikdi foo bar baz
        r list.getall k
    } {baz y bar z foo}

    test {Module list set while iterating} {
        r del k
        r rpush k x y z
        r list.edit k rkr foo bar
        r list.getall k
    } {foo y bar}

    test {Module list set while iterating, neg index} {
        r del k
        r rpush k x y z
        r list.edit k reverse rkr foo bar
        r list.getall k
    } {bar y foo}

    test {Module list - list entry and index should be updated when deletion} {
        set original_config [config_get_set list-max-listpack-size 1]

        # delete from start of list (index 0)
        r del l
        r rpush l x y z
        r list.edit l dd
        assert_equal [r list.getall l] {z}

        # delete from tail of list (index -3)
        r del l
        r rpush l x y z
        r list.edit l reverse kkdi foo
        assert_equal [r list.getall l] {foo y z}

        # delete from tail (index 2)
        r del l
        r rpush l x y z
        r list.edit l kkd
        assert_equal [r list.getall l] {x y}

        # delete from tail (index -1)
        r del l
        r rpush l x y z
        r list.edit l reverse dd
        assert_equal [r list.getall l] {x}

        # delete from middle (index 1)
        r del l
        r rpush l x y z
        r list.edit l kdd
        assert_equal [r list.getall l] {x}

        # delete from middle (index -2)
        r del l
        r rpush l x y z
        r list.edit l reverse kdd
        assert_equal [r list.getall l] {z}

        config_set list-max-listpack-size $original_config
    }

    test "Unload the module - list" {
        assert_equal {OK} [r module unload list]
    }
}
