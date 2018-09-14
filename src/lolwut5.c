/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include <math.h>

/* This structure represents our canvas. Drawing functions will take a pointer
 * to a canvas to write to it. Later the canvas can be rendered to a string
 * suitable to be printed on the screen, using unicode Braille characters. */
typedef struct lwCanvas {
    int width;
    int height;
    char *pixels;
} lwCanvas;

/* Translate a group of 8 pixels (2x4 vertical rectangle) to the corresponding
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
void lwTranslatePixelsGroup(int byte, char *output) {
    int code = 0x2800 + byte;
    /* Convert to unicode. This is in the U0800-UFFFF range, so we need to
     * emit it like this in three bytes:
     * 1110xxxx 10xxxxxx 10xxxxxx. */
    output[0] = 0xE0 | (code >> 12);          /* 1110-xxxx */
    output[1] = 0x80 | ((code >> 6) & 0x3F);  /* 10-xxxxxx */
    output[2] = 0x80 | (code & 0x3F);         /* 10-xxxxxx */
}

/* Allocate and return a new canvas of the specified size. */
lwCanvas *lwCreateCanvas(int width, int height) {
    lwCanvas *canvas = zmalloc(sizeof(*canvas));
    canvas->width = width;
    canvas->height = height;
    canvas->pixels = zmalloc(width*height);
    memset(canvas->pixels,0,width*height);
    return canvas;
}

/* Free the canvas created by lwCreateCanvas(). */
void lwFreeCanvas(lwCanvas *canvas) {
    zfree(canvas->pixels);
    zfree(canvas);
}

/* Set a pixel to the specified color. Color is 0 or 1, where zero means no
 * dot will be displyed, and 1 means dot will be displayed.
 * Coordinates are arranged so that left-top corner is 0,0. You can write
 * out of the size of the canvas without issues. */
void lwDrawPixel(lwCanvas *canvas, int x, int y, int color) {
    if (x < 0 || x >= canvas->width ||
        y < 0 || y >= canvas->height) return;
    canvas->pixels[x+y*canvas->width] = color;
}

/* Return the value of the specified pixel on the canvas. */
int lwGetPixel(lwCanvas *canvas, int x, int y) {
    if (x < 0 || x >= canvas->width ||
        y < 0 || y >= canvas->height) return 0;
    return canvas->pixels[x+y*canvas->width];
}

/* Draw a line from x1,y1 to x2,y2 using the Bresenham algorithm. */
void lwDrawLine(lwCanvas *canvas, int x1, int y1, int x2, int y2, int color) {
    int dx = abs(x2-x1);
    int dy = abs(y2-y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx-dy, e2;

    while(1) {
        lwDrawPixel(canvas,x1,y1,color);
        if (x1 == x2 && y1 == y2) break;
        e2 = err*2;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/* Draw a square centered at the specified x,y coordinates, with the specified
 * rotation angle and size. In order to write a rotated square, we use the
 * trivial fact that the parametric equation:
 *
 *  x = sin(k)
 *  y = cos(k)
 *
 * Describes a circle for values going from 0 to 2*PI. So basically if we start
 * at 45 degrees, that is k = PI/4, with the first point, and then we find
 * the other three points incrementing K by PI/2 (90 degrees), we'll have the
 * points of the square. In order to rotate the square, we just start with
 * k = PI/4 + rotation_angle, and we are done.
 *
 * Of course the vanilla equations above will describe the square inside a
 * circle of radius 1, so in order to draw larger squares we'll have to
 * multiply the obtained coordinates, and then translate them. However this
 * is much simpler than implementing the abstract concept of 2D shape and then
 * performing the rotation/translation transformation, so for LOLWUT it's
 * a good approach. */
void lwDrawSquare(lwCanvas *canvas, int x, int y, float size, float angle) {
    int px[4], py[4];

    /* Adjust the desired size according to the fact that the square inscribed
     * into a circle of radius 1 has the side of length SQRT(2). This way
     * size becomes a simple multiplication factor we can use with our
     * coordinates to magnify them. */
    size /= 1.4142135623;
    size = round(size);

    /* Compute the four points. */
    float k = M_PI/4 + angle;
    for (int j = 0; j < 4; j++) {
        px[j] = round(sin(k) * size + x);
        py[j] = round(cos(k) * size + y);
        k += M_PI/2;
    }

    /* Draw the square. */
    for (int j = 0; j < 4; j++)
        lwDrawLine(canvas,px[j],py[j],px[(j+1)%4],py[(j+1)%4],1);
}

/* Schotter, the output of LOLWUT of Redis 5, is a computer graphic art piece
 * generated by Georg Nees in the 60s. It explores the relationship between
 * caos and order.
 *
 * The function creates the canvas itself, depending on the columns available
 * in the output display and the number of squares per row and per column
 * requested by the caller. */
lwCanvas *lwDrawSchotter(int console_cols, int squares_per_row, int squares_per_col) {
    /* Calculate the canvas size. */
    int canvas_width = console_cols*2;
    int padding = canvas_width > 4 ? 2 : 0;
    float square_side = (float)(canvas_width-padding*2) / squares_per_row;
    int canvas_height = square_side * squares_per_col + padding*2;
    lwCanvas *canvas = lwCreateCanvas(canvas_width, canvas_height);

    for (int y = 0; y < squares_per_col; y++) {
        for (int x = 0; x < squares_per_row; x++) {
            int sx = x * square_side + square_side/2 + padding;
            int sy = y * square_side + square_side/2 + padding;
            /* Rotate and translate randomly as we go down to lower
             * rows. */
            float angle = 0;
            if (y > 1) {
                float r1 = (float)rand() / RAND_MAX / squares_per_col * y;
                float r2 = (float)rand() / RAND_MAX / squares_per_col * y;
                float r3 = (float)rand() / RAND_MAX / squares_per_col * y;
                if (rand() % 2) r1 = -r1;
                if (rand() % 2) r2 = -r2;
                if (rand() % 2) r3 = -r3;
                angle = r1;
                sx += r2*square_side/3;
                sy += r3*square_side/3;
            }
            lwDrawSquare(canvas,sx,sy,square_side,angle);
        }
    }

    return canvas;
}

/* Converts the canvas to an SDS string representing the UTF8 characters to
 * print to the terminal in order to obtain a graphical representaiton of the
 * logical canvas. The actual returned string will require a terminal that is
 * width/2 large and height/4 tall in order to hold the whole image without
 * overflowing or scrolling, since each Barille character is 2x4. */
sds lwRenderCanvas(lwCanvas *canvas) {
    sds text = sdsempty();
    for (int y = 0; y < canvas->height; y += 4) {
        for (int x = 0; x < canvas->width; x += 2) {
            /* We need to emit groups of 8 bits according to a specific
             * arrangement. See lwTranslatePixelsGroup() for more info. */
            int byte = 0;
            if (lwGetPixel(canvas,x,y)) byte |= (1<<0);
            if (lwGetPixel(canvas,x,y+1)) byte |= (1<<1);
            if (lwGetPixel(canvas,x,y+2)) byte |= (1<<2);
            if (lwGetPixel(canvas,x+1,y)) byte |= (1<<3);
            if (lwGetPixel(canvas,x+1,y+1)) byte |= (1<<4);
            if (lwGetPixel(canvas,x+1,y+2)) byte |= (1<<5);
            if (lwGetPixel(canvas,x,y+3)) byte |= (1<<6);
            if (lwGetPixel(canvas,x+1,y+3)) byte |= (1<<7);
            char unicode[3];
            lwTranslatePixelsGroup(byte,unicode);
            text = sdscatlen(text,unicode,3);
        }
        if (y != canvas->height-1) text = sdscatlen(text,"\n",1);
    }
    return text;
}

/* The LOLWUT command:
 *
 * LOLWUT [terminal columns] [squares-per-row] [squares-per-col]
 *
 * By default the command uses 66 columns, 8 squares per row, 12 squares
 * per column.
 */
void lolwut5Command(client *c) {
    long cols = 66;
    long squares_per_row = 8;
    long squares_per_col = 12;

    /* Parse the optional arguments if any. */
    if (c->argc > 1 &&
        getLongFromObjectOrReply(c,c->argv[1],&cols,NULL) != C_OK)
        return;

    if (c->argc > 2 &&
        getLongFromObjectOrReply(c,c->argv[2],&squares_per_row,NULL) != C_OK)
        return;

    if (c->argc > 3 &&
        getLongFromObjectOrReply(c,c->argv[3],&squares_per_col,NULL) != C_OK)
        return;

    /* Limits. We want LOLWUT to be always reasonably fast and cheap to execute
     * so we have maximum number of columns, rows, and output resulution. */
    if (cols < 1) cols = 1;
    if (cols > 1000) cols = 1000;
    if (squares_per_row < 1) squares_per_row = 1;
    if (squares_per_row > 200) squares_per_row = 200;
    if (squares_per_col < 1) squares_per_col = 1;
    if (squares_per_col > 200) squares_per_col = 200;

    /* Generate some computer art and reply. */
    lwCanvas *canvas = lwDrawSchotter(cols,squares_per_row,squares_per_col);
    sds rendered = lwRenderCanvas(canvas);
    rendered = sdscat(rendered,
        "\nGeorg nees - schotter, plotter on paper, 1968. Redis ver. ");
    rendered = sdscat(rendered,REDIS_VERSION);
    rendered = sdscatlen(rendered,"\n",1);
    addReplyBulkSds(c,rendered);
    lwFreeCanvas(canvas);
}
