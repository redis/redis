# Helper functions to simulate search-in-radius in the Tcl side in order to
# verify the Redis implementation with a fuzzy test.
proc geo_degrad deg {expr {$deg*(atan(1)*8/360)}}
proc geo_raddeg rad {expr {$rad/(atan(1)*8/360)}}

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

# Return elements non common to both the lists.
# This code is from http://wiki.tcl.tk/15489
proc compare_lists {List1 List2} {
   set DiffList {}
   foreach Item $List1 {
      if {[lsearch -exact $List2 $Item] == -1} {
         lappend DiffList $Item
      }
   }
   foreach Item $List2 {
      if {[lsearch -exact $List1 $Item] == -1} {
         if {[lsearch -exact $DiffList $Item] == -1} {
            lappend DiffList $Item
         }
      }
   }
   return $DiffList
}

# return true If a point in circle.
# search_lon and search_lat define the center of the circle,
# and lon, lat define the point being searched.
proc pointInCircle {radius_km lon lat search_lon search_lat} {
    set radius_m [expr {$radius_km*1000}]
    set distance [geo_distance $lon $lat $search_lon $search_lat]
    if {$distance < $radius_m} {
        return true
    }
    return false
}

# return true If a point in rectangle.
# search_lon and search_lat define the center of the rectangle,
# and lon, lat define the point being searched.
# error: can adjust the width and height of the rectangle according to the error
proc pointInRectangle {width_km height_km lon lat search_lon search_lat error} {
    set width_m [expr {$width_km*1000*$error/2}]
    set height_m [expr {$height_km*1000*$error/2}]
    set lon_distance [geo_distance $lon $lat $search_lon $lat]
    set lat_distance [geo_distance $lon $lat $lon $search_lat]

    if {$lon_distance > $width_m || $lat_distance > $height_m} {
        return false
    }
    return true
}

proc verify_geo_edge_response_bylonlat {expected_response expected_store_response} {
    catch {r georadius src{t} 1 1 1 km} response
    assert_match $expected_response $response

    catch {r georadius src{t} 1 1 1 km store dest{t}} response
    assert_match $expected_store_response $response

    catch {r geosearch src{t} fromlonlat 0 0 byradius 1 km} response
    assert_match $expected_response $response

    catch {r geosearchstore dest{t} src{t} fromlonlat 0 0 byradius 1 km} response
    assert_match $expected_store_response $response
}

proc verify_geo_edge_response_bymember {expected_response expected_store_response} {
    catch {r georadiusbymember src{t} member 1 km} response
    assert_match $expected_response $response

    catch {r georadiusbymember src{t} member 1 km store dest{t}} response
    assert_match $expected_store_response $response

    catch {r geosearch src{t} frommember member bybox 1 1 km} response
    assert_match $expected_response $response

    catch {r geosearchstore dest{t} src{t} frommember member bybox 1 1 m} response
    assert_match $expected_store_response $response
}

# The following list represents sets of random seed, search position
# and radius that caused bugs in the past. It is used by the randomized
# test later as a starting point. When the regression vectors are scanned
# the code reverts to using random data.
#
# The format is: seed km lon lat
set regression_vectors {
    {1482225976969 7083 81.634948934258375 30.561509253718668}
    {1482340074151 5416 -70.863281847379767 -46.347003465679947}
    {1499014685896 6064 -89.818768962202014 -40.463868561416803}
    {1412 156 149.29737817929004 15.95807862745508}
    {441574 143 59.235461856813856 66.269555127373678}
    {160645 187 -101.88575239939883 49.061997951502917}
    {750269 154 -90.187939661642517 66.615930412251487}
    {342880 145 163.03472387745728 64.012747720821181}
    {729955 143 137.86663517256579 63.986745399416776}
    {939895 151 59.149620271823181 65.204186651485145}
    {1412 156 149.29737817929004 15.95807862745508}
    {564862 149 84.062063109158544 -65.685403922426232}
    {1546032440391 16751 -1.8175081637769495 20.665668878082954}
}
set rv_idx 0

start_server {tags {"geo"}} {
    test {GEO with wrong type src key} {
        r set src{t} wrong_type

        verify_geo_edge_response_bylonlat "WRONGTYPE*" "WRONGTYPE*"
        verify_geo_edge_response_bymember "WRONGTYPE*" "WRONGTYPE*"
    }

    test {GEO with non existing src key} {
        r del src{t}

        verify_geo_edge_response_bylonlat {} 0
        verify_geo_edge_response_bymember {} 0
    }

    test {GEO BYLONLAT with empty search} {
        r del src{t}
        r geoadd src{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        verify_geo_edge_response_bylonlat {} 0
    }

    test {GEO BYMEMBER with non existing member} {
        r del src{t}
        r geoadd src{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        verify_geo_edge_response_bymember "ERR*" "ERR*"
    }

    test {GEOADD create} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {1}

    test {GEOADD update} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {0}

    test {GEOADD update with CH option} {
        assert_equal 1 [r geoadd nyc CH 40.747533 -73.9454966 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 40.747 < 0.001}
        assert {abs($y1) - 73.945 < 0.001}
    } {}

    test {GEOADD update with NX option} {
        assert_equal 0 [r geoadd nyc NX -73.9454966 40.747533 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 40.747 < 0.001}
        assert {abs($y1) - 73.945 < 0.001}
    } {}

    test {GEOADD update with XX option} {
        assert_equal 0 [r geoadd nyc XX -83.9454966 40.747533 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 83.945 < 0.001}
        assert {abs($y1) - 40.747 < 0.001}
    } {}

    test {GEOADD update with CH NX option} {
        r geoadd nyc CH NX -73.9454966 40.747533 "lic market"
    } {0}

    test {GEOADD update with CH XX option} {
        r geoadd nyc CH XX -73.9454966 40.747533 "lic market"
    } {1}

    test {GEOADD update with XX NX option will return syntax error} {
        catch {
            r geoadd nyc xx nx -73.9454966 40.747533 "lic market"
        } err
        set err
    } {ERR *syntax*}

    test {GEOADD update with invalid option} {
        catch {
            r geoadd nyc ch xx foo -73.9454966 40.747533 "lic market"
        } err
        set err
    } {ERR *syntax*}

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

    test {GEOSEARCH simple (sorted)} {
        r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km asc
    } {{central park n/q/r} 4545 {union square} {lic market}}

    test {GEOSEARCH FROMLONLAT and FROMMEMBER cannot exist at the same time} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 frommember xxx bybox 6 6 km asc} e
        set e
    } {ERR *syntax*}

    test {GEOSEARCH FROMLONLAT and FROMMEMBER one must exist} {
        catch {r geosearch nyc bybox 3 3 km asc desc withhash withdist withcoord} e
        set e
    } {ERR *exactly one of FROMMEMBER or FROMLONLAT*}

    test {GEOSEARCH BYRADIUS and BYBOX cannot exist at the same time} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 byradius 3 km bybox 3 3 km asc} e
        set e
    } {ERR *syntax*}

    test {GEOSEARCH BYRADIUS and BYBOX one must exist} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 asc desc withhash withdist withcoord} e
        set e
    } {ERR *exactly one of BYRADIUS and BYBOX*}

    test {GEOSEARCH with STOREDIST option} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km asc storedist} e
        set e
    } {ERR *syntax*}

    test {GEORADIUS withdist (sorted)} {
        r georadius nyc -73.9798091 40.7598464 3 km withdist asc
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697}}

    test {GEOSEARCH withdist (sorted)} {
        r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km withdist asc
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697} {{lic market} 3.1991}}

    test {GEORADIUS with COUNT} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS with ANY not sorted by default} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3 ANY
    } {{wtc one} {union square} {central park n/q/r}}

    test {GEORADIUS with ANY sorted by ASC} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3 ANY ASC
    } {{central park n/q/r} {union square} {wtc one}}

    test {GEORADIUS with ANY but no COUNT} {
        catch {r georadius nyc -73.9798091 40.7598464 10 km ANY ASC} e
        set e
    } {ERR *ANY*requires*COUNT*}

    test {GEORADIUS with COUNT but missing integer argument} {
        catch {r georadius nyc -73.9798091 40.7598464 10 km COUNT} e
        set e
    } {ERR *syntax*}

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
    
    test {GEORADIUSBYMEMBER search areas contain satisfied points in oblique direction} {
        r del k1
        
        r geoadd k1 -0.15307903289794921875 85 n1 0.3515625 85.00019260486917005437 n2
        set ret1 [r GEORADIUSBYMEMBER k1 n1 4891.94 m]
        assert_equal $ret1 {n1 n2}
        
        r zrem k1 n1 n2
        r geoadd k1 -4.95211958885192871094 85 n3 11.25 85.0511 n4
        set ret2 [r GEORADIUSBYMEMBER k1 n3 156544 m]
        assert_equal $ret2 {n3 n4}
        
        r zrem k1 n3 n4
        r geoadd k1 -45 65.50900022111811438208 n5 90 85.0511 n6
        set ret3 [r GEORADIUSBYMEMBER k1 n5 5009431 m]
        assert_equal $ret3 {n5 n6}
    }

    test {GEORADIUSBYMEMBER crossing pole search} {
        r del k1
        r geoadd k1 45 65 n1 -135 85.05 n2
        set ret [r GEORADIUSBYMEMBER k1 n1 5009431 m]
        assert_equal $ret {n1 n2}
    }

    test {GEOSEARCH FROMMEMBER simple (sorted)} {
        r geosearch nyc frommember "wtc one" bybox 14 14 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market} q4}

    test {GEOSEARCH vs GEORADIUS} {
        r del Sicily
        r geoadd Sicily 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"
        r geoadd Sicily 12.758489 38.788135 "edge1"   17.241510 38.788135 "eage2"
        set ret1 [r georadius Sicily 15 37 200 km asc]
        assert_equal $ret1 {Catania Palermo}
        set ret2 [r geosearch Sicily fromlonlat 15 37 bybox 400 400 km asc]
        assert_equal $ret2 {Catania Palermo eage2 edge1}
    }

    test {GEOSEARCH non square, long and narrow} {
        r del Sicily
        r geoadd Sicily 12.75 36.995 "test1"
        r geoadd Sicily 12.75 36.50 "test2"
        r geoadd Sicily 13.00 36.50 "test3"
        # box height=2km width=400km
        set ret1 [r geosearch Sicily fromlonlat 15 37 bybox 400 2 km]
        assert_equal $ret1 {test1}

        # Add a western Hemisphere point
        r geoadd Sicily -1 37.00 "test3"
        set ret2 [r geosearch Sicily fromlonlat 15 37 bybox 3000 2 km asc]
        assert_equal $ret2 {test1 test3}
    }

    test {GEOSEARCH corner point test} {
        r del Sicily
        r geoadd Sicily 12.758489 38.788135 edge1 17.241510 38.788135 edge2 17.250000 35.202000 edge3 12.750000 35.202000 edge4 12.748489955781654 37 edge5 15 38.798135872540925 edge6 17.251510044218346 37 edge7 15 35.201864127459075 edge8 12.692799634687903 38.798135872540925 corner1 12.692799634687903 38.798135872540925 corner2 17.200560937451133 35.201864127459075 corner3 12.799439062548865 35.201864127459075 corner4
        set ret [lsort [r geosearch Sicily fromlonlat 15 37 bybox 400 400 km asc]]
        assert_equal $ret {edge1 edge2 edge5 edge7}
    }

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
        set dist [r geodist points Palermo Palermo]
        assert {$dist eq 0.0000}
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
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        catch {r georadius points{t} 13.361389 38.115556 50 km store} e
        set e
    } {*ERR*syntax*}

    test {GEOSEARCHSTORE STORE option: syntax error} {
        catch {r geosearchstore abc{t} points{t} fromlonlat 13.361389 38.115556 byradius 50 km store abc{t}} e
        set e
    } {*ERR*syntax*}

    test {GEORANGE STORE option: incompatible options} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withdist} e
        assert_match {*ERR*} $e
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withhash} e
        assert_match {*ERR*} $e
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withcoords} e
        assert_match {*ERR*} $e
    }

    test {GEORANGE STORE option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km store points2{t}
        assert_equal [r zrange points{t} 0 -1] [r zrange points2{t} 0 -1]
    }

    test {GEORADIUSBYMEMBER STORE/STOREDIST option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        r georadiusbymember points{t} Palermo 500 km store points2{t}
        assert_equal {Palermo Catania} [r zrange points2{t} 0 -1]

        r georadiusbymember points{t} Catania 500 km storedist points2{t}
        assert_equal {Catania Palermo} [r zrange points2{t} 0 -1]

        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
    }

    test {GEOSEARCHSTORE STORE option: plain usage} {
        r geosearchstore points2{t} points{t} fromlonlat 13.361389 38.115556 byradius 500 km
        assert_equal [r zrange points{t} 0 -1] [r zrange points2{t} 0 -1]
    }

    test {GEORANGE STOREDIST option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
        assert {[lindex $res 3] < 167}
    }

    test {GEOSEARCHSTORE STOREDIST option: plain usage} {
        r geosearchstore points2{t} points{t} fromlonlat 13.361389 38.115556 byradius 500 km storedist
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
        assert {[lindex $res 3] < 167}
    }

    test {GEORANGE STOREDIST option: COUNT ASC and DESC} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t} asc count 1
        assert {[r zcard points2{t}] == 1}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 0] eq "Palermo"}

        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t} desc count 1
        assert {[r zcard points2{t}] == 1}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 0] eq "Catania"}
    }

    test {GEOSEARCH the box spans -180° or 180°} {
        r del points
        r geoadd points 179.5 36 point1
        r geoadd points -179.5 36 point2
        assert_equal {point1 point2} [r geosearch points fromlonlat 179 37 bybox 400 400 km asc]
        assert_equal {point2 point1} [r geosearch points fromlonlat -179 37 bybox 400 400 km asc]
    }

    test {GEOSEARCH with small distance} {
        r del points
        r geoadd points -122.407107 37.794300 1
        r geoadd points -122.227336 37.794300 2
        assert_equal {{1 0.0001} {2 9.8182}} [r GEORADIUS points -122.407107 37.794300 30 mi ASC WITHDIST]
    }

    foreach {type} {byradius bybox} {
    test "GEOSEARCH fuzzy test - $type" {
        if {$::accurate} { set attempt 300 } else { set attempt 30 }
        while {[incr attempt -1]} {
            set rv [lindex $regression_vectors $rv_idx]
            incr rv_idx

            set radius_km 0; set width_km 0; set height_km 0
            unset -nocomplain debuginfo
            set srand_seed [clock milliseconds]
            if {$rv ne {}} {set srand_seed [lindex $rv 0]}
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints

            if {[randomInt 10] == 0} {
                # From time to time use very big radiuses
                if {$type == "byradius"} {
                    set radius_km [expr {[randomInt 5000]+10}]
                } elseif {$type == "bybox"} {
                    set width_km [expr {[randomInt 5000]+10}]
                    set height_km [expr {[randomInt 5000]+10}]
                }
            } else {
                # Normally use a few - ~200km radiuses to stress
                # test the code the most in edge cases.
                if {$type == "byradius"} {
                    set radius_km [expr {[randomInt 200]+10}]
                } elseif {$type == "bybox"} {
                    set width_km [expr {[randomInt 200]+10}]
                    set height_km [expr {[randomInt 200]+10}]
                }
            }
            if {$rv ne {}} {
                set radius_km [lindex $rv 1]
                set width_km [lindex $rv 1]
                set height_km [lindex $rv 1]
            }
            geo_random_point search_lon search_lat
            if {$rv ne {}} {
                set search_lon [lindex $rv 2]
                set search_lat [lindex $rv 3]
            }
            lappend debuginfo "Search area: $search_lon,$search_lat $radius_km $width_km $height_km km"
            set tcl_result {}
            set argv {}
            for {set j 0} {$j < 20000} {incr j} {
                geo_random_point lon lat
                lappend argv $lon $lat "place:$j"
                if {$type == "byradius"} {
                    if {[pointInCircle $radius_km $lon $lat $search_lon $search_lat]} {
                        lappend tcl_result "place:$j"
                    }
                } elseif {$type == "bybox"} {
                    if {[pointInRectangle $width_km $height_km $lon $lat $search_lon $search_lat 1]} {
                        lappend tcl_result "place:$j"
                    }
                }
                lappend debuginfo "place:$j $lon $lat"
            }
            r geoadd mypoints {*}$argv
            if {$type == "byradius"} {
                set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat byradius $radius_km km]]
            } elseif {$type == "bybox"} {
                set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_km $height_km km]]
            }
            set res2 [lsort $tcl_result]
            set test_result OK

            if {$res != $res2} {
                set rounding_errors 0
                set diff [compare_lists $res $res2]
                foreach place $diff {
                    lassign [lindex [r geopos mypoints $place] 0] lon lat
                    set mydist [geo_distance $lon $lat $search_lon $search_lat]
                    set mydist [expr $mydist/1000]
                    if {$type == "byradius"} {
                        if {($mydist / $radius_km) > 0.999} {
                            incr rounding_errors
                            continue
                        }
                        if {$mydist < [expr {$radius_km*1000}]} {
                            # This is a false positive for redis since given the
                            # same points the higher precision calculation provided
                            # by TCL shows the point within range
                            incr rounding_errors
                            continue
                        }
                    } elseif {$type == "bybox"} {
                        # we add 0.1% error for floating point calculation error
                        if {[pointInRectangle $width_km $height_km $lon $lat $search_lon $search_lat 1.001]} {
                            incr rounding_errors
                            continue
                        }
                    }
                }

                # Make sure this is a real error and not a rounidng issue.
                if {[llength $diff] == $rounding_errors} {
                    set res $res2; # Error silenced
                }
            }

            if {$res != $res2} {
                set diff [compare_lists $res $res2]
                puts "*** Possible problem in GEO radius query ***"
                puts "Redis: $res"
                puts "Tcl  : $res2"
                puts "Diff : $diff"
                puts [join $debuginfo "\n"]
                foreach place $diff {
                    if {[lsearch -exact $res2 $place] != -1} {
                        set where "(only in Tcl)"
                    } else {
                        set where "(only in Redis)"
                    }
                    lassign [lindex [r geopos mypoints $place] 0] lon lat
                    set mydist [geo_distance $lon $lat $search_lon $search_lat]
                    set mydist [expr $mydist/1000]
                    puts "$place -> [r geopos mypoints $place] $mydist $where"
                }
                set test_result FAIL
            }
            unset -nocomplain debuginfo
            if {$test_result ne {OK}} break
        }
        set test_result
    } {OK}
    }

    test {GEOSEARCH box edges fuzzy test} {
        if {$::accurate} { set attempt 300 } else { set attempt 30 }
        while {[incr attempt -1]} {
            unset -nocomplain debuginfo
            set srand_seed [clock milliseconds]
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints

            geo_random_point search_lon search_lat
            set width_m [expr {[randomInt 10000]+10}]
            set height_m [expr {[randomInt 10000]+10}]
            set lat_delta [geo_raddeg [expr {$height_m/2/6372797.560856}]]
            set long_delta_top [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad [expr {$search_lat+$lat_delta}]])}]]
            set long_delta_middle [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad $search_lat])}]]
            set long_delta_bottom [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad [expr {$search_lat-$lat_delta}]])}]]

            # Total of 8 points are generated, which are located at each vertex and the center of each side
            set points(north) [list $search_lon [expr {$search_lat+$lat_delta}]]
            set points(south) [list $search_lon [expr {$search_lat-$lat_delta}]]
            set points(east) [list [expr {$search_lon+$long_delta_middle}] $search_lat]
            set points(west) [list [expr {$search_lon-$long_delta_middle}] $search_lat]
            set points(north_east) [list [expr {$search_lon+$long_delta_top}] [expr {$search_lat+$lat_delta}]]
            set points(north_west) [list [expr {$search_lon-$long_delta_top}] [expr {$search_lat+$lat_delta}]]
            set points(south_east) [list [expr {$search_lon+$long_delta_bottom}] [expr {$search_lat-$lat_delta}]]
            set points(south_west) [list [expr {$search_lon-$long_delta_bottom}] [expr {$search_lat-$lat_delta}]]

            lappend debuginfo "Search area: geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_m $height_m m"
            set tcl_result {}
            foreach name [array names points] {
                set x [lindex $points($name) 0]
                set y [lindex $points($name) 1]
                # If longitude crosses -180° or 180°, we need to convert it.
                # latitude doesn't have this problem, because it's scope is -70~70, see geo_random_point
                if {$x > 180} {
                    set x [expr {$x-360}]
                } elseif {$x < -180} {
                    set x [expr {$x+360}]
                }
                r geoadd mypoints $x $y place:$name
                lappend tcl_result "place:$name"
                lappend debuginfo "geoadd mypoints $x $y place:$name"
            }

            set res2 [lsort $tcl_result]

            # make the box larger by two meter in each direction to put the coordinate slightly inside the box.
            set height_new [expr {$height_m+4}]
            set width_new [expr {$width_m+4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != $res2} {
                set diff [compare_lists $res $res2]
                lappend debuginfo "res: $res, res2: $res2, diff: $diff"
                fail "place should be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # The width decreases and the height increases. Only north and south are found
            set width_new [expr {$width_m-4}]
            set height_new [expr {$height_m+4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != {place:north place:south}} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # The width increases and the height decreases. Only ease and west are found
            set width_new [expr {$width_m+4}]
            set height_new [expr {$height_m-4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != {place:east place:west}} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # make the box smaller by two meter in each direction to put the coordinate slightly outside the box.
            set height_new [expr {$height_m-4}]
            set width_new [expr {$width_m-4}]
            set res [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]
            if {$res != ""} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }
            unset -nocomplain debuginfo
        }
    }
}
