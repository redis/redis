start_server {tags {"gopher protocol"}} {
   r set "/" rootval
   r set "/subkey" subkeyval 
   assert_equal [r config set gopher-enabled yes] OK


    test "Get root explicit" {
        reconnect
        r write "/\r\n"
        r flush
        r read_all
    } {rootval}

    test "Get root implicit" {
        reconnect
        r write "\r\n"
        r flush
        r read_all
    } {rootval}

    test "Get subkey" {
        reconnect
        r write "/subkey\r\n"
        r flush
        r read_all
    } {subkeyval}

    test "Get invalid key" {
        reconnect
        r write "/invalidkey\r\n"
        r flush
        assert_match "iError:*" [r read_all]
    }
}


