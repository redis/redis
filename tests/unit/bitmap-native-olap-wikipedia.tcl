proc seed_string_bitmap_wikipedia_olap {key bits} {
    r del $key
    r set $key ""
    foreach bit $bits {
        r setbit $key $bit 1
    }
}

proc seed_native_bitmap_wikipedia_olap {key bits} {
    set old [lindex [r config get bitmap-default-roaring] 1]
    r config set bitmap-default-roaring yes
    r del $key
    if {[llength $bits] == 0} {
        r setbit $key 0 0
    } else {
        foreach bit $bits {
            r setbit $key $bit 1
        }
    }
    r config set bitmap-default-roaring $old
}

proc assert_wikipedia_olap_bitmap_has_exact_bits {key bits} {
    set unique [lsort -integer -unique $bits]
    assert_equal [llength $unique] [r bitcount $key]
    foreach bit $unique {
        assert_equal 1 [r getbit $key $bit]
    }
}

start_server {tags {"bitmap" "bitmap-native" "cluster:skip"}} {
    test {native bitmap BITOP models Druid Wikipedia query tutorial filters} {
        # Implements the Apache Druid query tutorial's Wikipedia-style OLAP
        # story as Redis bitmaps over row IDs:
        # https://druid.apache.org/docs/latest/tutorials/tutorial-query/
        #
        # 0 {page Copa America countryName United States channel en isRobot false}
        # 1 {page Copa America countryName Brazil        channel es isRobot false}
        # 2 {page Lionel Messi countryName Argentina    channel es isRobot false}
        # 3 {page Apache Druid countryName null         channel en isRobot true}
        # 4 {page Apache Druid countryName United States channel en isRobot false}
        # 5 {page Wind countryName null                 channel de isRobot false}
        # 6 {page Copa America countryName United States channel en isRobot true}
        foreach {index bits} {
            page:copa-america {0 1 6}
            page:apache-druid {3 4}
            page:lionel-messi {2}
            page:wind {5}
            country:united-states {0 4 6}
            country:brazil {1}
            country:argentina {2}
            country:null {3 5}
            channel:en {0 3 4 6}
            channel:es {1 2}
            channel:de {5}
            isrobot:true {3 6}
            isrobot:false {0 1 2 4 5}
            universe {0 1 2 3 4 5 6}
        } {
            seed_native_bitmap_wikipedia_olap "bitmap:wikipedia:$index" $bits
            assert_equal bitmap [r type "bitmap:wikipedia:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:wikipedia:$index"]
        }

        # Tutorial query pattern: exclude rows without a countryName value.
        r bitop not bitmap:wikipedia:q:country-not-null:raw bitmap:wikipedia:country:null
        assert_equal 1 [r getbit bitmap:wikipedia:q:country-not-null:raw 7]

        r bitop and bitmap:wikipedia:q:country-not-null \
            bitmap:wikipedia:universe bitmap:wikipedia:q:country-not-null:raw
        assert_wikipedia_olap_bitmap_has_exact_bits \
            bitmap:wikipedia:q:country-not-null {0 1 2 4 6}
        assert_equal 0 [r getbit bitmap:wikipedia:q:country-not-null 7]

        # Tutorial query pattern: group by page and countryName, then count rows.
        r bitop and bitmap:wikipedia:q:copa-country-edits \
            bitmap:wikipedia:page:copa-america bitmap:wikipedia:q:country-not-null
        assert_wikipedia_olap_bitmap_has_exact_bits \
            bitmap:wikipedia:q:copa-country-edits {0 1 6}
        assert_equal 3 [r bitcount bitmap:wikipedia:q:copa-country-edits]

        r bitop and bitmap:wikipedia:q:copa-us-edits \
            bitmap:wikipedia:page:copa-america bitmap:wikipedia:country:united-states
        assert_wikipedia_olap_bitmap_has_exact_bits \
            bitmap:wikipedia:q:copa-us-edits {0 6}
        assert_equal 2 [r bitcount bitmap:wikipedia:q:copa-us-edits]

        # Query: English edits with a countryName value, excluding robot edits.
        r bitop not bitmap:wikipedia:q:not-robot:raw bitmap:wikipedia:isrobot:true
        r bitop and bitmap:wikipedia:q:not-robot \
            bitmap:wikipedia:universe bitmap:wikipedia:q:not-robot:raw
        r bitop and bitmap:wikipedia:q:en-country-not-robot \
            bitmap:wikipedia:channel:en \
            bitmap:wikipedia:q:country-not-null \
            bitmap:wikipedia:q:not-robot
        assert_wikipedia_olap_bitmap_has_exact_bits \
            bitmap:wikipedia:q:en-country-not-robot {0 4}

        # Query: edits from either Brazil or Argentina.
        r bitop or bitmap:wikipedia:q:brazil-or-argentina \
            bitmap:wikipedia:country:brazil bitmap:wikipedia:country:argentina
        assert_wikipedia_olap_bitmap_has_exact_bits \
            bitmap:wikipedia:q:brazil-or-argentina {1 2}
    }
}
