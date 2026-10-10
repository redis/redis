# Fake rdb-channel master: streams a corrupt compressed frame. Usage: tclsh fake_rdbchannel_master.tcl PORT RDB_FILE

set port [lindex $argv 0]
set f [open [lindex $argv 1] rb]
fconfigure $f -translation binary
set rdb_bytes [read $f]
close $f
set rdb_len [string length $rdb_bytes]

proc read_command {sock} {
    set char [read $sock 1]
    if {$char eq ""} { return "" }
    if {$char eq "*"} {
        set n [string trimright [gets $sock] "\r\n"]
        set parts {}
        for {set i 0} {$i < $n} {incr i} {
            read $sock 1
            set len [string trimright [gets $sock] "\r\n"]
            set s [read $sock $len]
            read $sock 2
            lappend parts $s
        }
        return [join $parts " "]
    }
    set rest [string trimright [gets $sock] "\r\n"]
    return "$char$rest"
}

proc handle {sock} {
    global rdb_bytes rdb_len
    if {[eof $sock]} { catch {close $sock}; return }
    set cmd [read_command $sock]
    if {$cmd eq ""} return
    if {[string match "REPLCONF *rdb-only*" $cmd]} { set ::rdb_ch($sock) 1 }
    if {[string match "PSYNC*" $cmd]} {
        if {[info exists ::rdb_ch($sock)]} {
            puts -nonewline $sock "+FULLRESYNC [string repeat a 40] 0\r\n"
            puts -nonewline $sock "\$$rdb_len\r\n$rdb_bytes\r\n"
        } else {
            puts $sock "+RDBCHANNELSYNC 1"
            puts -nonewline $sock [string repeat "\xDE\xAD\xBE\xEF" 51200]
        }
        flush $sock
        return
    }
    puts $sock "+OK"
    flush $sock
}

proc accept {sock host port} {
    fconfigure $sock -translation binary
    fileevent $sock readable [list handle $sock]
}

socket -server accept -myaddr 127.0.0.1 $port
after 120000 exit
vwait forever
