# tests of corrupt ziplist payload with valid CRC

tags {"dump" "corruption"} {

set corrupt_payload_7445 "\x0E\x01\x1D\x1D\x00\x00\x00\x16\x00\x00\x00\x03\x00\x00\x04\x43\x43\x43\x43\x06\x04\x42\x42\x42\x42\x06\x3F\x41\x41\x41\x41\xFF\x09\x00\x88\xA5\xCA\xA8\xC5\x41\xF4\x35"

test {corrupt payload: #7445 - with sanitize} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r config set sanitize-dump-payload yes
        catch {
            r restore key 0 $corrupt_payload_7445
        } err
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Ziplist integrity check failed*" 0
    }
}

test {corrupt payload: #7445 - without sanitize - 1} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r config set sanitize-dump-payload no
        r config set crash-memcheck-enabled no ;# avoid valgrind issues
        r restore key 0 $corrupt_payload_7445
        catch {r lindex key 2}
        verify_log_message 0 "*ASSERTION FAILED*" 0
    }
}

test {corrupt payload: #7445 - without sanitize - 2} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r config set sanitize-dump-payload no
        r config set crash-memcheck-enabled no ;# avoid valgrind issues
        r restore key 0 $corrupt_payload_7445
        catch {r lset key 2 "BEEF"}
        verify_log_message 0 "*ASSERTION FAILED*" 0
    }
}

test {corrupt payload: hash with valid zip list header, invalid entry len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0D\x1B\x1B\x00\x00\x00\x16\x00\x00\x00\x04\x00\x00\x02\x61\x00\x04\x02\x62\x00\x04\x14\x63\x00\x04\x02\x64\x00\xFF\x09\x00\xD9\x10\x54\x92\x15\xF5\x5F\x52"
        r config set crash-memcheck-enabled no ;# avoid valgrind issues
        r config set hash-max-ziplist-entries 1
        catch {r hset key b b}
        verify_log_message 0 "*zipEntrySafe*" 0
    }
}

test {corrupt payload: invalid zlbytes header} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        catch {
            r restore key 0 "\x0D\x1B\x25\x00\x00\x00\x16\x00\x00\x00\x04\x00\x00\x02\x61\x00\x04\x02\x62\x00\x04\x02\x63\x00\x04\x02\x64\x00\xFF\x09\x00\xB7\xF7\x6E\x9F\x43\x43\x14\xC6"
        } err
        assert_match "*Bad data format*" $err
    }
}

test {corrupt payload: valid zipped hash header, dup records} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0D\x1B\x1B\x00\x00\x00\x16\x00\x00\x00\x04\x00\x00\x02\x61\x00\x04\x02\x62\x00\x04\x02\x61\x00\x04\x02\x64\x00\xFF\x09\x00\xA1\x98\x36\x78\xCC\x8E\x93\x2E"
        r config set crash-memcheck-enabled no ;# avoid valgrind issues
        r config set hash-max-ziplist-entries 1
        # cause an assertion when converting to hash table
        catch {r hset key b b}
        verify_log_message 0 "*ziplist with dup elements dump*" 0
    }
}

test {corrupt payload: quicklist big ziplist prev len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0E\x01\x13\x13\x00\x00\x00\x0E\x00\x00\x00\x02\x00\x00\x02\x61\x00\x0E\x02\x62\x00\xFF\x09\x00\x49\x97\x30\xB2\x0D\xA1\xED\xAA"
        r config set crash-memcheck-enabled no ;# avoid valgrind issues
        catch {r lindex key -2}
        verify_log_message 0 "*ASSERTION FAILED*" 0
    }
}

test {corrupt payload: quicklist small ziplist prev len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r config set sanitize-dump-payload yes
        catch {
            r restore key 0 "\x0E\x01\x13\x13\x00\x00\x00\x0E\x00\x00\x00\x02\x00\x00\x02\x61\x00\x02\x02\x62\x00\xFF\x09\x00\xC7\x71\x03\x97\x07\x75\xB0\x63"
        } err
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Ziplist integrity check failed*" 0
    }
}

test {corrupt payload: quicklist ziplist wrong count} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0E\x01\x13\x13\x00\x00\x00\x0E\x00\x00\x00\x03\x00\x00\x02\x61\x00\x04\x02\x62\x00\xFF\x09\x00\x4D\xE2\x0A\x2F\x08\x25\xDF\x91"
        r lpush key a
        # check that the server didn't crash
        r ping
    }
}

test {corrupt payload: #3080 - quicklist} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        catch {
            r RESTORE key 0 "\x0E\x01\x80\x00\x00\x00\x10\x41\x41\x41\x41\x41\x41\x41\x41\x02\x00\x00\x80\x41\x41\x41\x41\x07\x00\x03\xC7\x1D\xEF\x54\x68\xCC\xF3"
            r DUMP key
        }
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Ziplist integrity check failed*" 0
    }
}

test {corrupt payload: #3080 - ziplist} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        catch {
            r RESTORE key 0 "\x0A\x80\x00\x00\x00\x10\x41\x41\x41\x41\x41\x41\x41\x41\x02\x00\x00\x80\x41\x41\x41\x41\x07\x00\x39\x5B\x49\xE0\xC1\xC6\xDD\x76"
        }
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Ziplist integrity check failed*" 0
    }
}

test {corrupt payload: load corrupted rdb with no CRC - #3505} {
    set server_path [tmpdir "server.rdb-corruption-test"]
    exec cp tests/assets/corrupt_ziplist.rdb $server_path
    set srv [start_server [list overrides [list "dir" $server_path "dbfilename" "corrupt_ziplist.rdb" loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no]]]

    # wait for termination
    wait_for_condition 100 50 {
        ! [is_alive $srv]
    } else {
        fail "rdb loading didn't fail"
    }

    set stdout [dict get $srv stdout]
    assert_equal [count_message_lines $stdout "Terminating server after rdb file reading failure."]  1
    assert_lessthan 1 [count_message_lines $stdout "Ziplist integrity check failed"]
    kill_server $srv ;# let valgrind look for issues
}

test {corrupt payload: listpack invalid size header} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        catch {
            r restore key 0 "\x0F\x01\x10\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x02\x40\x55\x5F\x00\x00\x00\x0F\x00\x01\x01\x00\x01\x02\x01\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x00\x01\x00\x01\x00\x01\x00\x01\x02\x02\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x61\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x88\x62\x00\x00\x00\x00\x00\x00\x00\x09\x08\x01\xFF\x0A\x01\x00\x00\x09\x00\x45\x91\x0A\x87\x2F\xA5\xF9\x2E"
        } err
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Stream listpack integrity check failed*" 0
    }
}

test {corrupt payload: listpack too long entry len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0F\x01\x10\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x02\x40\x55\x55\x00\x00\x00\x0F\x00\x01\x01\x00\x01\x02\x01\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x00\x01\x00\x01\x00\x01\x00\x01\x02\x02\x89\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x61\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x88\x62\x00\x00\x00\x00\x00\x00\x00\x09\x08\x01\xFF\x0A\x01\x00\x00\x09\x00\x40\x63\xC9\x37\x03\xA2\xE5\x68"
        catch {
            r xinfo stream key full
        } err
        verify_log_message 0 "*ASSERTION FAILED*" 0
    }
}

test {corrupt payload: listpack very long entry len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r restore key 0 "\x0F\x01\x10\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x02\x40\x55\x55\x00\x00\x00\x0F\x00\x01\x01\x00\x01\x02\x01\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x00\x01\x00\x01\x00\x01\x00\x01\x02\x02\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x61\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x9C\x62\x00\x00\x00\x00\x00\x00\x00\x09\x08\x01\xFF\x0A\x01\x00\x00\x09\x00\x63\x6F\x42\x8E\x7C\xB5\xA2\x9D"
        catch {
            r xinfo stream key full
        } err
        verify_log_message 0 "*ASSERTION FAILED*" 0
    }
}

test {corrupt payload: listpack too long entry prev len} {
    start_server [list overrides [list loglevel verbose use-exit-on-panic yes crash-memcheck-enabled no] ] {
        r config set sanitize-dump-payload yes
        catch {
            r restore key 0 "\x0F\x01\x10\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x02\x40\x55\x55\x00\x00\x00\x0F\x00\x01\x01\x00\x15\x02\x01\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x00\x01\x00\x01\x00\x01\x00\x01\x02\x02\x88\x31\x00\x00\x00\x00\x00\x00\x00\x09\x88\x61\x00\x00\x00\x00\x00\x00\x00\x09\x88\x32\x00\x00\x00\x00\x00\x00\x00\x09\x88\x62\x00\x00\x00\x00\x00\x00\x00\x09\x08\x01\xFF\x0A\x01\x00\x00\x09\x00\x06\xFB\x44\x24\x0A\x8E\x75\xEA"
        } err
        assert_match "*Bad data format*" $err
        verify_log_message 0 "*Stream listpack integrity check failed*" 0
    }
}

} ;# tags

