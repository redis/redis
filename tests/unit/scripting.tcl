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
    
    test "EVAL - sandbox killed functions - global 'collectgarbage'" {
        reconnect
        assert_error "*attempt to call global 'collectgarbage' (a nil value)*" {r eval "return collectgarbage()"  0}
    }
    
    test "EVAL - sandbox killed functions - global 'dofile'" {
        reconnect
        assert_error "*attempt to call global 'dofile' (a nil value)*" {r eval "return dofile('anyfile')"  0}
    }
    test "EVAL - sandbox killed functions - global 'load'" {
        reconnect
        assert_error "*attempt to call global 'load' (a nil value)*" {r eval "return load('anyfile')"  0}
    }
    
    test "EVAL - sandbox killed functions - global 'loadfile'" {
        reconnect
        assert_error "*attempt to call global 'loadfile' (a nil value)*" {r eval "return loadfile('anyfile')"  0}
    }
    
    test "EVAL - sandbox killed functions - global 'require'" {
        reconnect
        assert_error "*attempt to call global 'require' (a nil value)*" {r eval "return require('anyfile')"  0}
    }
    
    test "EVAL - sandbox killed functions - os 'exit'" {
        reconnect
        assert_error "*attempt to call field 'exit' (a nil value)*" {r eval "return os.exit()"  0}
    }
    
    test "EVAL - sandbox killed functions - os 'execute'" {
        reconnect
        assert_error "*attempt to call field 'execute' (a nil value)*" {r eval "return os.execute()"  0}
    }
    
    test "EVAL - sandbox killed functions - os 'remove'" {
        reconnect
        assert_error "*attempt to call field 'remove' (a nil value)*" {r eval "return os.remove()"  0}
    }
    
    test "EVAL - sandbox killed functions os 'rename'" {
        reconnect
        assert_error "*attempt to call field 'rename' (a nil value)*" {r eval "return os.rename()"  0}
    }
    
    test "EVAL - basic bitop functionalities" {
        reconnect
        assert_equal "-1" [r eval "return bit.tobit(0xffffffff)" 0]
        assert_equal "0xffffffff" [r eval "return '0x'..bit.tohex(-1)" 0]
        assert_equal "-1" [r eval "return bit.bnot(0)" 0]
        assert_equal "15" [r eval "return bit.bor(1, 2, 4, 8)" 0]
        assert_equal "120" [r eval "return bit.band(0x12345678, 0xff)" 0]
        assert_equal "267390960" [r eval "return bit.bxor(0xa5a5f0f0, 0xaa55ff00)" 0]
        assert_equal "256" [r eval "return bit.lshift(1, 8)" 0]
        assert_equal "16777215" [r eval "return bit.rshift(-256, 8)" 0]
        assert_equal "-1" [r eval "return bit.arshift(-256, 8)" 0]
        assert_equal "1164411171" [r eval "return bit.rol(0x12345678, 12)" 0]
        assert_equal "1736516421" [r eval "return bit.ror(0x12345678, 12)" 0]
        assert_equal "2018915346" [r eval "return bit.bswap(0x12345678)" 0]
    }

}
