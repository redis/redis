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
    
    test "EVAL - sandbox allowed libraries - base" {
        reconnect
        assert_match "function:*" [r eval "return tostring(ipairs)" 0]
    }
    
    test "EVAL - sandbox allowed libraries - package" {
        reconnect
        assert_match "table:*" [r eval "return tostring(package)" 0]
    }
    
    test "EVAL - sandbox allowed libraries - table" {
        reconnect
        assert_match "table:*" [r eval "return tostring(table)" 0]
    }
    
    test "EVAL - sandbox allowed libraries - os" {
        reconnect
        assert_match "table:*" [r eval "return tostring(os)" 0]
    }
    
    test "EVAL - sandbox allowed libraries - string" {
        reconnect
        assert_match "table:*" [r eval "return tostring(string)" 0]
    }
    
    test "EVAL - sandbox allowed libraries - math" {
        reconnect
        assert_match "table:*" [r eval "return tostring(math)" 0]
    }
    
    test "EVAL - sandbox allowed libraries debug" {
        reconnect
        assert_match "table:*" [r eval "return tostring(debug)" 0]
    }
    
    test "EVAL - sandbox disallowed libraries - io" {
        reconnect
        assert_error "*attempt to index global 'io' (a nil value)*" {r eval "return io.flush()"  0}
    }

}
