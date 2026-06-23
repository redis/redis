proc seed_string_bitmap_olap {key bits} {
    r del $key
    r set $key ""
    foreach bit $bits {
        r setbit $key $bit 1
    }
}

proc seed_native_bitmap_olap {key bits} {
    seed_string_bitmap_olap $key $bits
    r bitmap convert $key
}

proc assert_olap_bitmap_has_exact_bits {key bits} {
    set unique [lsort -integer -unique $bits]
    assert_equal [llength $unique] [r bitcount $key]
    foreach bit $unique {
        assert_equal 1 [r getbit $key $bit]
    }
}

start_server {tags {"bitmap" "bitmap-native" "cluster:skip"}} {
    test {native bitmap BITOP supports OLAP columnar index user stories} {
        # Rows model ad-tech events in a Druid-style columnar segment:
        # 0 {country Brazil clicks 1 gender male}
        # 1 {country Brazil clicks 1 gender female}
        # 2 {country United States impressions 1 gender female}
        # 3 {country United States clicks 1 gender male}
        # 4 {country United States installs 1 gender female}
        # 5 {country United States impressions 1 gender female}
        # 6 {country United States installs 1 gender female}
        #
        # Each dimension or metric value is indexed by the row IDs that match it.
        foreach {index bits} {
            country:brazil {0 1}
            country:united-states {2 3 4 5 6}
            gender:male {0 3}
            gender:female {1 2 4 5 6}
            metric:clicks {0 1 3}
            metric:impressions {2 5}
            metric:installs {4 6}
            universe {0 1 2 3 4 5 6}
        } {
            seed_native_bitmap_olap "bitmap:olap:$index" $bits
            assert_equal bitmap [r type "bitmap:olap:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:olap:$index"]
        }

        # Query: how many Brazil users installed the app?
        r bitop and bitmap:olap:q:brazil-installs \
            bitmap:olap:country:brazil bitmap:olap:metric:installs
        assert_olap_bitmap_has_exact_bits bitmap:olap:q:brazil-installs {}

        # Query: how many female users clicked but did not install?
        #
        # The NOT predicate must be bounded by the segment universe. Otherwise
        # complementing a bitmap may include bits outside the ingested rows.
        r bitop not bitmap:olap:q:not-installs:raw bitmap:olap:metric:installs
        assert_equal 1 [r getbit bitmap:olap:q:not-installs:raw 7]

        r bitop and bitmap:olap:q:not-installs \
            bitmap:olap:universe bitmap:olap:q:not-installs:raw
        assert_olap_bitmap_has_exact_bits bitmap:olap:q:not-installs {0 1 2 3 5}
        assert_equal 0 [r getbit bitmap:olap:q:not-installs 7]

        r bitop and bitmap:olap:q:female-click-no-install \
            bitmap:olap:gender:female bitmap:olap:metric:clicks bitmap:olap:q:not-installs
        assert_olap_bitmap_has_exact_bits bitmap:olap:q:female-click-no-install {1}
        assert_equal bitmap [r type bitmap:olap:q:female-click-no-install]

        # Query: how many United States users clicked or saw an impression?
        r bitop or bitmap:olap:q:engaged \
            bitmap:olap:metric:clicks bitmap:olap:metric:impressions
        assert_olap_bitmap_has_exact_bits bitmap:olap:q:engaged {0 1 2 3 5}

        r bitop and bitmap:olap:q:us-engaged \
            bitmap:olap:country:united-states bitmap:olap:q:engaged
        assert_olap_bitmap_has_exact_bits bitmap:olap:q:us-engaged {2 3 5}
        assert_equal bitmap [r type bitmap:olap:q:us-engaged]
    }
}
