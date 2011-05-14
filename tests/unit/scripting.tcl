start_server {
    tags {"scripting"}
} {
    test "EVAL - basic scripting environment" {
        reconnect
        assert_match "# Server*" [r eval "return redis('INFO')" 0]
    }
    
    test "EVAL - return types" {
        reconnect
        assert_equal 23 [r eval "return 23" 0]
        assert_equal "lua string" [r eval "return 'lua string'" 0]
        assert_equal "1 2 3 4 5" [r eval "return {1,2,3,4,5}" 0]
        assert_error "ERR Error*lua error*" {r eval "return error('lua error')" 0}
    }

}
