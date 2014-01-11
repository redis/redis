start_server {
    tags {"matrix"}
} {

    test {XSET} {
        assert_equal {2 2} [r xset xsetmat1 1 1 3]
        assert_equal {100 100 3} [r xset xsetmat2 99 99 2 255]
    }

    test {XGET} {
        assert_equal {4 4} [r xset xgetmat 3 3 1]
        assert_equal {2 4 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1} [r xget xgetmat -1 -1]
    }
}
