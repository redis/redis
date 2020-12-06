start_server {tags {"currdb"}} {
    test "CURRDB" {
        assert_equal "0" r CURRDB
        r SELECT 1
        assert_equal "1" r CURRDB
        r SELECT 2
        assert_equal "2" r CURRDB
    }
}