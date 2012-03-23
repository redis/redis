set server_path [tmpdir "server.rdb-encoding-test"]

# Copy RDB with different encodings in server path
exec cp tests/assets/encodings.rdb $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "encodings.rdb"]] {
  test "RDB encoding loading test" {
    r select 0
    csvdump r
  } {"compressible","string","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
"hash","hash","a","1","aa","10","aaa","100","b","2","bb","20","bbb","200","c","3","cc","30","ccc","300","ddd","400","eee","5000000000",
"hash_zipped","hash","a","1","b","2","c","3",
"list","list","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000",
"list_zipped","list","1","2","3","a","b","c","100000","6000000000",
"number","string","10"
"set","set","1","100000","2","3","6000000000","a","b","c",
"set_zipped_1","set","1","2","3","4",
"set_zipped_2","set","100000","200000","300000","400000",
"set_zipped_3","set","1000000000","2000000000","3000000000","4000000000","5000000000","6000000000",
"string","string","Hello World"
"zset","zset","a","1","b","2","c","3","aa","10","bb","20","cc","30","aaa","100","bbb","200","ccc","300","aaaa","1000","cccc","123456789","bbbb","5000000000",
"zset_zipped","zset","a","1","b","2","c","3",
}
}

