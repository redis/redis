start_server {tags "debug"} {
    test {rocksdb-property-value for rocksdb.stats} {
        assert_match {*[default]*[meta]*[score]*} [r swap rocksdb-property-value rocksdb.stats]
        assert_equal {} [r swap rocksdb-property-value rocksdb.stats "wrongcf"]
        assert_equal {} [r swap rocksdb-property-value rocksdb.stats "wrongcf,meta"]
        assert_match {*[meta]*} [r swap rocksdb-property-value rocksdb.stats "meta"]
        assert_match {*[default]*} [r swap rocksdb-property-value rocksdb.stats "default"]
        assert_match {*[default]*[meta]*[score]*} [r swap rocksdb-property-value rocksdb.stats "default,meta,score,default,meta,score"]
    }

    test {rocksdb-property-value for wrong-prop-name} {
        assert_match {} [r swap rocksdb-property-value wrong-prop-name]
        assert_equal {} [r swap rocksdb-property-value wrong-prop-name "wrongcf"]
        assert_equal {} [r swap rocksdb-property-value wrong-prop-name "wrongcf,meta"]
        assert_match {} [r swap rocksdb-property-value wrong-prop-name "meta"]
        assert_match {} [r swap rocksdb-property-value wrong-prop-name "default"]
        assert_match {} [r swap rocksdb-property-value wrong-prop-name "default,meta,score,default,meta,score"]
    }

    test {rocksdb-property-int for rocksdb.block-cache-usage} {
        r swap rocksdb-property-int rocksdb.block-cache-usage
        r swap rocksdb-property-int rocksdb.block-cache-usage "wrongcf"
        r swap rocksdb-property-int rocksdb.block-cache-usage "wrongcf,meta"
        r swap rocksdb-property-int rocksdb.block-cache-usage "meta"
        r swap rocksdb-property-int rocksdb.block-cache-usage "default"
        r swap rocksdb-property-int rocksdb.block-cache-usage "default,meta,score,default,meta,score"
    }

    test {rocksdb-property-int for wrong-prop-name} {
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name]
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name "wrongcf"]
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name "wrongcf,meta"]
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name "meta"]
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name "default"]
        assert_equal 0 [r swap rocksdb-property-int wrong-prop-name "default,meta,score,default,meta,score"]
    }
}

