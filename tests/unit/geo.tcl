# Helper functins to simulate search-in-radius in the Tcl side in order to
# verify the Redis implementation with a fuzzy test.
proc geo_degrad deg {expr {$deg*atan(1)*8/360}}

proc geo_distance {lat1d lon1d lat2d lon2d} {
    set lat1r [geo_degrad $lat1d]
    set lon1r [geo_degrad $lon1d]
    set lat2r [geo_degrad $lat2d]
    set lon2r [geo_degrad $lon2d]
    set u [expr {sin(($lat2r - $lat1r) / 2)}]
    set v [expr {sin(($lon2r - $lon1r) / 2)}]
    expr {2.0 * 6372797.560856 * \
            asin(sqrt($u * $u + cos($lat1r) * cos($lat2r) * $v * $v))}
}

proc geo_random_point {latvar lonvar} {
    upvar 1 $latvar lat
    upvar 1 $lonvar lon
    # Note that the actual latitude limit should be -85 to +85, we restrict
    # the test to -70 to +70 since in this range the algorithm is more precise
    # while outside this range occasionally some element may be missing.
    set lat [expr {-70 + rand()*140}]
    set lon [expr {-180 + rand()*360}]
}

start_server {tags {"geo"}} {
    test {GEOADD create} {
        r geoadd nyc 40.747533 -73.9454966 "lic market"
    } {1}

    test {GEOADD update} {
        r geoadd nyc 40.747533 -73.9454966 "lic market"
    } {0}

    test {GEOADD invalid coordinates} {
        catch {
            r geoadd nyc 40.747533 -73.9454966 "lic market" \
                foo bar "luck market"
        } err
        set err
    } {*valid*}

    test {GEOADD multi add} {
        r geoadd nyc 40.7648057 -73.9733487 "central park n/q/r" 40.7362513 -73.9903085 "union square" 40.7126674 -74.0131604 "wtc one" 40.6428986 -73.7858139 "jfk" 40.7498929 -73.9375699 "q4" 40.7480973 -73.9564142 4545
    } {6}

    test {Check geoset values} {
        r zrange nyc 0 -1 withscores
    } {{wtc one} 1791873972053020 {union square} 1791875485187452 {central park n/q/r} 1791875761332224 4545 1791875796750882 {lic market} 1791875804419201 q4 1791875830079666 jfk 1791895905559723}

    test {GEORADIUS simple (sorted)} {
        r georadius nyc 40.7598464 -73.9798091 3 km ascending
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS withdistance (sorted)} {
        r georadius nyc 40.7598464 -73.9798091 3 km withdistance ascending
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697}}

    test {GEORADIUSBYMEMBER simple (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market}}

    test {GEORADIUSBYMEMBER withdistance (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km withdist
    } {{{wtc one} 0.0000} {{union square} 3.2544} {{central park n/q/r} 6.7000} {4545 6.1975} {{lic market} 6.8969}}

    test {GEOENCODE simple} {
        r geoencode 41.2358883 1.8063239
    } {3471579339700058 {41.235888125243704 1.8063229322433472}\
                        {41.235890659964866 1.806328296661377}\
                        {41.235889392604285 1.8063256144523621}}

    test {GEODECODE simple} {
        r geodecode 3471579339700058
    } {{41.235888125243704 1.8063229322433472}\
       {41.235890659964866 1.806328296661377}\
       {41.235889392604285 1.8063256144523621}}

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
            geo_random_point search_lat search_lon
            lappend debuginfo "Search area: $search_lat,$search_lon $radius_km km"
            set tcl_result {}
            set argv {}
            for {set j 0} {$j < 20000} {incr j} {
                geo_random_point lat lon
                lappend argv $lat $lon "place:$j"
                if {[geo_distance $lat $lon $search_lat $search_lon] < $radius_m} {
                    lappend tcl_result "place:$j"
                    lappend debuginfo "place:$j $lat $lon [expr {[geo_distance $lat $lon $search_lat $search_lon]/1000}] km"
                }
            }
            r geoadd mypoints {*}$argv
            set res [lsort [r georadius mypoints $search_lat $search_lon $radius_km km]]
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
