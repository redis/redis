/*
 * Copyright (c) 2018-2019, Salvatore Sanfilippo <antirez at gmail dot com>
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
 */

/* This structure represents our canvas. Drawing functions will take a pointer
 * to a canvas to write to it. Later the canvas can be rendered to a string
 * suitable to be printed on the screen, using unicode Braille characters. */

/* This represents a very simple generic canvas in order to draw stuff.
 * It's up to each LOLWUT versions to translate what they draw to the
 * screen, depending on the result to accomplish. */

#ifndef __LOLWUT_H
#define __LOLWUT_H

typedef struct lwCanvas {
    int width;
    int height;
    char *pixels;
} lwCanvas;

/* Drawing functions implemented inside lolwut.c. */
lwCanvas *lwCreateCanvas(int width, int height, int bgcolor);
void lwFreeCanvas(lwCanvas *canvas);
void lwDrawPixel(lwCanvas *canvas, int x, int y, int color);
int lwGetPixel(lwCanvas *canvas, int x, int y);
void lwDrawLine(lwCanvas *canvas, int x1, int y1, int x2, int y2, int color);
void lwDrawSquare(lwCanvas *canvas, int x, int y, float size, float angle, int color);

#endif
