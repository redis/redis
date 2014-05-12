start_server {tags {"geo"}} {
    test {GEOADD create} {
        r geoadd nyc 40.747533 -73.9454966 "lic market"
    } {1}

    test {GEOADD update} {
        r geoadd nyc 40.747533 -73.9454966 "lic market"
    } {0}

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
    } {{{central park n/q/r} 0.78} {4545 2.37} {{union square} 2.77}}

    test {GEORADIUSBYMEMBER simple (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market}}

    test {GEORADIUSBYMEMBER simple (sorted, json)} {
        r georadiusbymember nyc "wtc one" 7 km withgeojson
    } {{{wtc one} {{"type":"Feature","geometry":{"type":"Point","coordinates":[-74.01316255331,40.712667181451]},"properties":{"distance":0,"member":"wtc one","units":"km","set":"nyc"}}}}\
       {{union square} {{"type":"Feature","geometry":{"type":"Point","coordinates":[-73.990310132504,40.736250227118]},"properties":{"distance":3.2543954573354,"member":"union square","units":"km","set":"nyc"}}}}\
       {{central park n/q/r} {{"type":"Feature","geometry":{"type":"Point","coordinates":[-73.973347842693,40.764806395699]},"properties":{"distance":6.7000029092796,"member":"central park n\/q\/r","units":"km","set":"nyc"}}}}\
       {4545 {{"type":"Feature","geometry":{"type":"Point","coordinates":[-73.956412374973,40.748097513816]},"properties":{"distance":6.1975173818008,"member":"4545","units":"km","set":"nyc"}}}}\
       {{lic market} {{"type":"Feature","geometry":{"type":"Point","coordinates":[-73.945495784283,40.747532270998]},"properties":{"distance":6.8968709532081,"member":"lic market","units":"km","set":"nyc"}}}}}

    test {GEORADIUSBYMEMBER withdistance (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km withdist
    } {{{wtc one} 0.00} {{union square} 3.25} {{central park n/q/r} 6.70} {4545 6.20} {{lic market} 6.90}}

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
}
