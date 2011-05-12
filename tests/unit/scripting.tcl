start_server {
    tags {"scripting"}
} {
    test "EVAL - basic scripting environment" {
        reconnect
        assert_match "redis_version:*" [r eval "return redis('INFO')" 0]
    }
    
    test "EVAL - return types" {
        reconnect
        assert_equal 23 [r eval "return 23" 0]
        assert_equal "lua string" [r eval "return 'lua string'" 0]
        assert_equal "1 2 3 4 5" [r eval "return {1,2,3,4,5}" 0]
        assert_error "ERR Error*lua error*" {r eval "return error('lua error')" 0}
    }
    
    test "EVAL - sandbox allowed libraries" {
        reconnect
        assert_match "function:*" [r eval "return tostring(ipairs)" 0]
        assert_match "table:*" [r eval "return tostring(package)" 0]
        assert_match "table:*" [r eval "return tostring(table)" 0]
        assert_match "table:*" [r eval "return tostring(os)" 0]
        assert_match "table:*" [r eval "return tostring(string)" 0]
        assert_match "table:*" [r eval "return tostring(math)" 0]
        assert_match "table:*" [r eval "return tostring(debug)" 0]
        assert_match "table:*" [r eval "return tostring(bit)" 0]
        assert_match "table:*" [r eval "return tostring(cjson)" 0]
    }
    
    test "EVAL - sandbox disallowed libraries" {
        reconnect
        assert_error "*attempt to index global 'io' (a nil value)*" {r eval "return io.flush()"  0}
    }
    
    test "EVAL - sandbox killed functions" {
        reconnect
        assert_error "*attempt to call global 'collectgarbage' (a nil value)*" {r eval "return collectgarbage()"  0}
        reconnect
        assert_error "*attempt to call global 'dofile' (a nil value)*" {r eval "return dofile('anyfile')"  0}
        reconnect
        assert_error "*attempt to call global 'load' (a nil value)*" {r eval "return load('anyfile')"  0}
        reconnect
        assert_error "*attempt to call global 'loadfile' (a nil value)*" {r eval "return loadfile('anyfile')"  0}
        reconnect
        assert_error "*attempt to call global 'loadstring' (a nil value)*" {r eval "return loadstring('i = i * 2')"  0}
        reconnect
        assert_error "*attempt to call global 'require' (a nil value)*" {r eval "return require('anyfile')"  0}
        reconnect
        assert_error "*attempt to call field 'exit' (a nil value)*" {r eval "return os.exit()"  0}
        reconnect
        assert_error "*attempt to call field 'execute' (a nil value)*" {r eval "return os.execute()"  0}
        reconnect
        assert_error "*attempt to call field 'remove' (a nil value)*" {r eval "return os.remove()"  0}
        reconnect
        assert_error "*attempt to call field 'rename' (a nil value)*" {r eval "return os.rename()"  0}
    }
}
