/* Translate a group of 8 pixels (2x8 vertical rectangle) to the corresponding
 * braille character. The byte should correspond to the pixels arranged as
 * follows, where 0 is the least significant bit, and 7 the most significant
 * bit:
 *
 *   0 3
 *   1 4
 *   2 5
 *   6 7
 *
 * The corresponding utf8 encoded character is set into the three bytes
 * pointed by 'output'.
 */
#include <stdio.h>
void lwEmitPixelsGroup(int byte, char *output) {
    int code = 0x2800 + byte;
    /* Convert to unicode. This is in the U0800-UFFFF range, so we need to
     * emit it like this in three bytes:
     * 1110xxxx 10xxxxxx 10xxxxxx. */
    output[1] = 0xE0 | (code >> 12);          /* 1110-xxxx */
    output[2] = 0x80 | ((code >> 6) & 0x3F);  /* 10-xxxxxx */
    output[3] = 0x80 | (code & 0x3F);         /* 10-xxxxxx */
}
