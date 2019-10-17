set testmodule [file normalize tests/modules/scan.so]

proc count_log_message {pattern} {
    set result [exec grep -c $pattern < [srv 0 stdout]]
}

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module scan} {
        # the module create a scan command which also return values
        r set x 1
        r set y 2
        r set z 3
        lsort [r scan.scankeysvalues]
    } {{x 1} {y 2} {z 3}}

}