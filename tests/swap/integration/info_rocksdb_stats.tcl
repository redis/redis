start_server {tags {"swap string"}} {
    after 2000
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats]] 1
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats1]] 0
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats.]] 1
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats.a]] 1
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats.data]] 1
    assert_equal [string match "*meta rocksdb.stats*" [r info rocksdb.stats.meta]] 1
    assert_equal [string match "*meta rocksdb.stats*" [r info rocksdb.stats.meta1]] 0
    assert_equal [string match "*score rocksdb.stats*" [r info rocksdb.stats.score]] 1
    assert_equal [string match "*meta rocksdb.stats*" [r info rocksdb.stats.score1]] 0

    assert_equal [string match "*meta rocksdb.stats*" [r info rocksdb.stats.meta.score]] 1
    assert_equal [string match "*score rocksdb.stats*" [r info rocksdb.stats.meta.score]] 1
    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats.meta.score]] 0

    assert_equal [string match "*default rocksdb.stats*" [r info rocksdb.stats.meta.score.data]] 1
    assert_equal [string match "*meta rocksdb.stats*" [r info rocksdb.stats.meta.score.data]] 1
    assert_equal [string match "*score rocksdb.stats*" [r info rocksdb.stats.meta.score.data]] 1
}