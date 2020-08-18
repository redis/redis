add_library(hiredis STATIC
        hiredis.c
        net.c
        dict.c
        sds.c
        async.c
        read.c
        )