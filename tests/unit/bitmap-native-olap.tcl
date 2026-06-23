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
        # Inspired by Apache Druid's columnar segment and logical filter docs:
        # https://druid.apache.org/docs/latest/design/segments/
        # https://druid.apache.org/docs/latest/querying/filters/#logical-expression-filters
        #
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

    test {native bitmap BITOP models Pinot inverted index examples} {
        # Implements the Apache Pinot Star-Tree Index example table and inverted
        # index story as Redis bitmaps over document IDs:
        # https://docs.pinot.apache.org/build-with-pinot/indexing/star-tree-index
        #
        # 0 {Country CA  Browser Chrome  Locale en  Impressions 400}
        # 1 {Country CA  Browser Firefox Locale fr  Impressions 200}
        # 2 {Country MX  Browser Safari  Locale es  Impressions 300}
        # 3 {Country MX  Browser Safari  Locale en  Impressions 100}
        # 4 {Country USA Browser Chrome  Locale en  Impressions 600}
        # 5 {Country USA Browser Firefox Locale es  Impressions 200}
        # 6 {Country USA Browser Firefox Locale en  Impressions 400}
        foreach {index bits} {
            country:ca {0 1}
            country:mx {2 3}
            country:usa {4 5 6}
            browser:chrome {0 4}
            browser:firefox {1 5 6}
            browser:safari {2 3}
            locale:en {0 3 4 6}
            locale:fr {1}
            locale:es {2 5}
            metric:impressions-at-least-400 {0 4 6}
            universe {0 1 2 3 4 5 6}
        } {
            seed_native_bitmap_olap "bitmap:pinot:$index" $bits
            assert_equal bitmap [r type "bitmap:pinot:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:pinot:$index"]
        }

        # Source story: an inverted index maps a value such as Browser=Firefox
        # to the matching document IDs.
        assert_olap_bitmap_has_exact_bits bitmap:pinot:browser:firefox {1 5 6}
        assert_olap_bitmap_has_exact_bits bitmap:pinot:locale:en {0 3 4 6}

        # Query: which Firefox documents are in the English locale?
        r bitop and bitmap:pinot:q:firefox-en \
            bitmap:pinot:browser:firefox bitmap:pinot:locale:en
        assert_olap_bitmap_has_exact_bits bitmap:pinot:q:firefox-en {6}

        # Query: which USA documents used Chrome or Spanish locale?
        r bitop or bitmap:pinot:q:chrome-or-es \
            bitmap:pinot:browser:chrome bitmap:pinot:locale:es
        assert_olap_bitmap_has_exact_bits bitmap:pinot:q:chrome-or-es {0 2 4 5}

        r bitop and bitmap:pinot:q:usa-chrome-or-es \
            bitmap:pinot:country:usa bitmap:pinot:q:chrome-or-es
        assert_olap_bitmap_has_exact_bits bitmap:pinot:q:usa-chrome-or-es {4 5}

        # Query: which USA documents have at least 400 impressions?
        r bitop and bitmap:pinot:q:usa-high-impressions \
            bitmap:pinot:country:usa bitmap:pinot:metric:impressions-at-least-400
        assert_olap_bitmap_has_exact_bits bitmap:pinot:q:usa-high-impressions {4 6}

        # Query: which CA or MX documents are not in the French locale?
        r bitop or bitmap:pinot:q:ca-or-mx \
            bitmap:pinot:country:ca bitmap:pinot:country:mx
        r bitop not bitmap:pinot:q:not-fr:raw bitmap:pinot:locale:fr
        assert_equal 1 [r getbit bitmap:pinot:q:not-fr:raw 7]

        r bitop and bitmap:pinot:q:not-fr \
            bitmap:pinot:universe bitmap:pinot:q:not-fr:raw
        r bitop and bitmap:pinot:q:ca-or-mx-not-fr \
            bitmap:pinot:q:ca-or-mx bitmap:pinot:q:not-fr
        assert_olap_bitmap_has_exact_bits bitmap:pinot:q:ca-or-mx-not-fr {0 2 3}
    }
}
