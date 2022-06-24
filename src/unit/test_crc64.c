#include <stdio.h>
#include "../crc64.h"

#include "testhelp.h"

extern uint64_t _crc64(uint_fast64_t crc, const void *in_data, const uint64_t len);

int crc64Test(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    crc64_init();

    unsigned char numbers[] = "123456789";
    ASSERT("[calcula]: CRC64 '123456789'", (uint64_t)_crc64(0, numbers, 9) == 16845390139448941002ull);
    ASSERT("[calcula]: CRC64 '123456789'", (uint64_t)crc64(0, numbers, 9) == 16845390139448941002ull);

    unsigned char li[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
                "do eiusmod tempor incididunt ut labore et dolore magna "
                "aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
                "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis "
                "aute irure dolor in reprehenderit in voluptate velit esse "
                "cillum dolore eu fugiat nulla pariatur. Excepteur sint "
                "occaecat cupidatat non proident, sunt in culpa qui officia "
                "deserunt mollit anim id est laborum.";

    ASSERT("[calcula]: CRC64 TEXT'", (uint64_t)_crc64(0, li, sizeof(li)) == 14373597793578550195ull);
    ASSERT("[calcula]: CRC64 TEXT", (uint64_t)crc64(0, li, sizeof(li)) == 14373597793578550195ull);
    return 0;
}
