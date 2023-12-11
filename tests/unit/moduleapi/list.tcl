set testmodule [file normalize tests/modules/list.so]

# The following arguments can be passed to args:
#   i -- the number of inserts
#   d -- the number of deletes
#   r -- the number of replaces
#   index -- the last index
#   entry -- The entry pointed to by index
proc verify_list_edit_reply {reply argv} {
    foreach {k v} $argv {
        assert_equal [dict get $reply $k] $v
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module list set, get, insert, delete} {
        r del k
        assert_error {WRONGTYPE Operation against a key holding the wrong kind of value*} {r list.set k 1 xyz}
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
        verify_list_edit_reply [r list.edit k ikikdi foo bar baz] {i 3 index 5}
        r list.getall k
    } {foo x bar y baz}

    test {Module list insert & delete, neg index} {
        r del k
        r rpush k x y z
        verify_list_edit_reply [r list.edit k REVERSE ikikdi foo bar baz] {i 3 index -6}
        r list.getall k
    } {baz y bar z foo}

    test {Module list set while iterating} {
        r del k
        r rpush k x y z
        verify_list_edit_reply [r list.edit k rkr foo bar] {r 2 index 3}
        r list.getall k
    } {foo y bar}

    test {Module list set while iterating, neg index} {
        r del k
        r rpush k x y z
        verify_list_edit_reply [r list.edit k reverse rkr foo bar] {r 2 index -4}
        r list.getall k
    } {bar y foo}

    test {Module list - encoding conversion while inserting} {
        r config set list-max-listpack-size 4
        r del k
        r rpush k a b c d
        assert_encoding listpack k

        # Converts to quicklist after inserting.
        r list.edit k dii foo bar
        assert_encoding quicklist k
        assert_equal [r list.getall k] {foo bar b c d}

        # Converts to listpack after deleting three entries.
        r list.edit k ddd e
        assert_encoding listpack k
        assert_equal [r list.getall k] {c d}
    }

    test {Module list - encoding conversion while replacing} {
        r config set list-max-listpack-size -1
        r del k
        r rpush k x y z
        assert_encoding listpack k

        # Converts to quicklist after replacing.
        set big [string repeat "x" 4096]
        r list.edit k r $big
        assert_encoding quicklist k
        assert_equal [r list.getall k] "$big y z"

        # Converts to listpack after deleting the big entry.
        r list.edit k d
        assert_encoding listpack k
        assert_equal [r list.getall k] {y z}
    }

    test {Module list - list entry and index should be updated when deletion} {
        set original_config [config_get_set list-max-listpack-size 1]

        # delete from start (index 0)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l dd] {d 2 index 0 entry z}
        assert_equal [r list.getall l] {z}

        # delete from start (index -3)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l reverse kkd] {d 1 index -3}
        assert_equal [r list.getall l] {y z}

        # # delete from tail (index 2)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l kkd] {d 1 index 2}
        assert_equal [r list.getall l] {x y}

        # # delete from tail (index -1)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l reverse dd] {d 2 index -1 entry x}
        assert_equal [r list.getall l] {x}

        # # delete from middle (index 1)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l kdd] {d 2 index 1}
        assert_equal [r list.getall l] {x}

        # # delete from middle (index -2)
        r del l
        r rpush l x y z
        verify_list_edit_reply [r list.edit l reverse kdd] {d 2 index -2}
        assert_equal [r list.getall l] {z}

        config_set list-max-listpack-size $original_config
    }

    test "Unload the module - list" {
        assert_equal {OK} [r module unload list]
    }
}
