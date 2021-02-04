set testmodule [file normalize tests/modules/hash.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r del k
        assert_equal 0 [r hash.set k "" squirrel yes]
        assert_equal 2 [r hash.set k "i" banana no sushi whynot]
        assert_equal 0 [r hash.set k "n" squirrel hoho what nothing]
        assert_equal 1 [r hash.set k "ni" squirrel hoho something nice]
        assert_equal 0 [r hash.set k "xi" new stuff not inserted]
        assert_equal 1 [r hash.set k "x" squirrel ofcourse]
        assert_equal 1 [r hash.set k "" sushi :delete: none :delete:]
        r hgetall k
    } {squirrel ofcourse banana no what nothing something nice}
}
