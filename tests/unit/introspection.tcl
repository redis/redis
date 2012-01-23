start_server {tags {"introspection"}} {
    test {CLIENT LIST} {
        r client list
    } {*addr=*:* fd=* idle=* flags=N db=9 sub=0 psub=0 qbuf=0 obl=0 oll=0 omem=0 events=r cmd=client*}
}
