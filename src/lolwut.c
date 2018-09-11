/*
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------------
 *
 * This file implements the LOLWUT command. The command should do something
 * fun and interesting, and should be replaced by a new implementation at
 * each new version of Redis.
 */

/* This function represents our canvas. Drawing functions will take a pointer
 * to a canvas to write to it. Later the canvas can be rendered to a string
 * suitable to be printed on the screen, using unicode Braille characters. */
struct lwCanvas {
    int width;
    int height;
    char *pixels;
};

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


