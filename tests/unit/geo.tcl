# Helper functins to simulate search-in-radius in the Tcl side in order to
# verify the Redis implementation with a fuzzy test.
proc geo_degrad deg {expr {$deg*atan(1)*8/360}}

proc geo_distance {lon1d lat1d lon2d lat2d} {
    set lon1r [geo_degrad $lon1d]
    set lat1r [geo_degrad $lat1d]
    set lon2r [geo_degrad $lon2d]
    set lat2r [geo_degrad $lat2d]
    set v [expr {sin(($lon2r - $lon1r) / 2)}]
    set u [expr {sin(($lat2r - $lat1r) / 2)}]
    expr {2.0 * 6372797.560856 * \
            asin(sqrt($u * $u + cos($lat1r) * cos($lat2r) * $v * $v))}
}

proc geo_random_point {lonvar latvar} {
    upvar 1 $lonvar lon
    upvar 1 $latvar lat
    # Note that the actual latitude limit should be -85 to +85, we restrict
    # the test to -70 to +70 since in this range the algorithm is more precise
    # while outside this range occasionally some element may be missing.
    set lon [expr {-180 + rand()*360}]
    set lat [expr {-70 + rand()*140}]
}

start_server {tags {"geo"}} {
    test {GEOADD create} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {1}

    test {GEOADD update} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {0}

    test {GEOADD invalid coordinates} {
        catch {
            r geoadd nyc -73.9454966 40.747533 "lic market" \
                foo bar "luck market"
        } err
        set err
    } {*valid*}

    test {GEOADD multi add} {
        r geoadd nyc -73.9733487 40.7648057 "central park n/q/r" -73.9903085 40.7362513 "union square" -74.0131604 40.7126674 "wtc one" -73.7858139 40.6428986 "jfk" -73.9375699 40.7498929 "q4" -73.9564142 40.7480973 4545
    } {6}

    test {Check geoset values} {
        r zrange nyc 0 -1 withscores
    } {{wtc one} 1791873972053020 {union square} 1791875485187452 {central park n/q/r} 1791875761332224 4545 1791875796750882 {lic market} 1791875804419201 q4 1791875830079666 jfk 1791895905559723}

    test {GEORADIUS simple (sorted)} {
        r georadius nyc -73.9798091 40.7598464 3 km asc
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS withdist (sorted)} {
        r georadius nyc -73.9798091 40.7598464 3 km withdist asc
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697}}

    test {GEORADIUS with COUNT} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS with COUNT but missing integer argument} {
        catch {r georadius nyc -73.9798091 40.7598464 10 km COUNT} e
        set e
    } {ERR*syntax*}

    test {GEORADIUS with COUNT DESC} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 2 DESC
    } {{wtc one} q4}

    test {GEORADIUS HUGE, issue #2767} {
        r geoadd users -47.271613776683807 -54.534504198047678 user_000000
        llength [r GEORADIUS users 0 0 50000 km WITHCOORD]
    } {1}

    test {GEORADIUSBYMEMBER simple (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market}}

    test {GEORADIUSBYMEMBER withdist (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km withdist
    } {{{wtc one} 0.0000} {{union square} 3.2544} {{central park n/q/r} 6.7000} {4545 6.1975} {{lic market} 6.8969}}

    test {GEOHASH is able to return geohash strings} {
        # Example from Wikipedia.
        r del points
        r geoadd points -5.6 42.6 test
        lindex [r geohash points test] 0
    } {ezs42e44yx0}

    test {GEOPOS simple} {
        r del points
        r geoadd points 10 20 a 30 40 b
        lassign [lindex [r geopos points a b] 0] x1 y1
        lassign [lindex [r geopos points a b] 1] x2 y2
        assert {abs($x1 - 10) < 0.001}
        assert {abs($y1 - 20) < 0.001}
        assert {abs($x2 - 30) < 0.001}
        assert {abs($y2 - 40) < 0.001}
    }

    test {GEOPOS missing element} {
        r del points
        r geoadd points 10 20 a 30 40 b
        lindex [r geopos points a x b] 1
    } {}

    test {GEODIST simple & unit} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        set m [r geodist points Palermo Catania]
        assert {$m > 166274 && $m < 166275}
        set km [r geodist points Palermo Catania km]
        assert {$km > 166.2 && $km < 166.3}
    }

    test {GEODIST missing elements} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        set m [r geodist points Palermo Agrigento]
        assert {$m eq {}}
        set m [r geodist points Ragusa Agrigento]
        assert {$m eq {}}
        set m [r geodist empty_key Palermo Catania]
        assert {$m eq {}}
    }

    test {GEORADIUS STORE option: syntax error} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        catch {r georadius points 13.361389 38.115556 50 km store} e
        set e
    } {*ERR*syntax*}

    test {GEORANGE STORE option: incompatible options} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        catch {r georadius points 13.361389 38.115556 50 km store points2 withdist} e
        assert_match {*ERR*} $e
        catch {r georadius points 13.361389 38.115556 50 km store points2 withhash} e
        assert_match {*ERR*} $e
        catch {r georadius points 13.361389 38.115556 50 km store points2 withcoords} e
        assert_match {*ERR*} $e
    }

    test {GEORANGE STORE option: plain usage} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        r georadius points 13.361389 38.115556 500 km store points2
        assert_equal [r zrange points 0 -1] [r zrange points2 0 -1]
    }

    test {GEORANGE STOREDIST option: plain usage} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        r georadius points 13.361389 38.115556 500 km storedist points2
        set res [r zrange points2 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
        assert {[lindex $res 3] < 167}
    }

    test {GEORANGE STOREDIST option: COUNT ASC and DESC} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        r georadius points 13.361389 38.115556 500 km storedist points2 asc count 1
        assert {[r zcard points2] == 1}
        set res [r zrange points2 0 -1 withscores]
        assert {[lindex $res 0] eq "Palermo"}

        r georadius points 13.361389 38.115556 500 km storedist points2 desc count 1
        assert {[r zcard points2] == 1}
        set res [r zrange points2 0 -1 withscores]
        assert {[lindex $res 0] eq "Catania"}
    }

    test {GEOADD + GEORANGE randomized test} {
        set attempt 10
        while {[incr attempt -1]} {
            unset -nocomplain debuginfo
            set srand_seed [randomInt 1000000]
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints
            set radius_km [expr {[randomInt 200]+10}]
            set radius_m [expr {$radius_km*1000}]
            geo_random_point search_lon search_lat
            lappend debuginfo "Search area: $search_lon,$search_lat $radius_km km"
            set tcl_result {}
            set argv {}
            for {set j 0} {$j < 20000} {incr j} {
                geo_random_point lon lat
                lappend argv $lon $lat "place:$j"
                if {[geo_distance $lon $lat $search_lon $search_lat] < $radius_m} {
                    lappend tcl_result "place:$j"
                    lappend debuginfo "place:$j $lon $lat [expr {[geo_distance $lon $lat $search_lon $search_lat]/1000}] km"
                }
            }
            r geoadd mypoints {*}$argv
            set res [lsort [r georadius mypoints $search_lon $search_lat $radius_km km]]
            set res2 [lsort $tcl_result]
            set test_result OK
            if {$res != $res2} {
                puts "Redis: $res"
                puts "Tcl  : $res2"
                puts [join $debuginfo "\n"]
                set test_result FAIL
            }
            unset -nocomplain debuginfo
            if {$test_result ne {OK}} break
        }
        set test_result
    } {OK}
}
