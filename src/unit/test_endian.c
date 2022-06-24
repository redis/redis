#include <stdio.h>
#include "testhelp.h"
#include "../endianconv.h"

int endianconvTest(int argc, char *argv[], int flags) {
    char buf[32];

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev16(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev32(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev64(buf);
    printf("%s\n", buf);

    return 0;
}
