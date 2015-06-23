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
}
