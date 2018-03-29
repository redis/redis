# Helper functions to simulate search-in-radius in the Tcl side in order to
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
}
set rv_idx 0

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
        set attempt 30
        while {[incr attempt -1]} {
            set rv [lindex $regression_vectors $rv_idx]
            incr rv_idx

            unset -nocomplain debuginfo
            set srand_seed [clock milliseconds]
            if {$rv ne {}} {set srand_seed [lindex $rv 0]}
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints

            if {[randomInt 10] == 0} {
                # From time to time use very big radiuses
                set radius_km [expr {[randomInt 50000]+10}]
            } else {
                # Normally use a few - ~200km radiuses to stress
                # test the code the most in edge cases.
                set radius_km [expr {[randomInt 200]+10}]
            }
            if {$rv ne {}} {set radius_km [lindex $rv 1]}
            set radius_m [expr {$radius_km*1000}]
            geo_random_point search_lon search_lat
            if {$rv ne {}} {
                set search_lon [lindex $rv 2]
                set search_lat [lindex $rv 3]
            }
            lappend debuginfo "Search area: $search_lon,$search_lat $radius_km km"
            set tcl_result {}
            set argv {}
            for {set j 0} {$j < 20000} {incr j} {
                geo_random_point lon lat
                lappend argv $lon $lat "place:$j"
                set distance [geo_distance $lon $lat $search_lon $search_lat]
                if {$distance < $radius_m} {
                    lappend tcl_result "place:$j"
                }
                lappend debuginfo "place:$j $lon $lat [expr {$distance/1000}] km"
            }
            r geoadd mypoints {*}$argv
            set res [lsort [r georadius mypoints $search_lon $search_lat $radius_km km]]
            set res2 [lsort $tcl_result]
            set test_result OK

            if {$res != $res2} {
                set rounding_errors 0
                set diff [compare_lists $res $res2]
                foreach place $diff {
                    set mydist [geo_distance $lon $lat $search_lon $search_lat]
                    set mydist [expr $mydist/1000]
                    if {($mydist / $radius_km) > 0.999} {incr rounding_errors}
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
                    if {($mydist / $radius_km) > 0.999} {incr rounding_errors}
                }
                set test_result FAIL
            }
            unset -nocomplain debuginfo
            if {$test_result ne {OK}} break
        }
        set test_result
    } {OK}
}
