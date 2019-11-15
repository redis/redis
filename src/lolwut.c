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
#include "lolwut.h"
#include <math.h>

void lolwut5Command(client *c);
void lolwut6Command(client *c);

/* The default target for LOLWUT if no matching version was found.
 * This is what unstable versions of Redis will display. */
void lolwutUnstableCommand(client *c) {
    sds rendered = sdsnew("Redis ver. ");
    rendered = sdscat(rendered,REDIS_VERSION);
    rendered = sdscatlen(rendered,"\n",1);
    addReplyVerbatim(c,rendered,sdslen(rendered),"txt");
    sdsfree(rendered);
}

/* LOLWUT [VERSION <version>] [... version specific arguments ...] */
void lolwutCommand(client *c) {
    char *v = REDIS_VERSION;
    char verstr[64];

    if (c->argc >= 3 && !strcasecmp(c->argv[1]->ptr,"version")) {
        long ver;
        if (getLongFromObjectOrReply(c,c->argv[2],&ver,NULL) != C_OK) return;
        snprintf(verstr,sizeof(verstr),"%u.0.0",(unsigned int)ver);
        v = verstr;

        /* Adjust argv/argc to filter the "VERSION ..." option, since the
         * specific LOLWUT version implementations don't know about it
         * and expect their arguments. */
        c->argv += 2;
        c->argc -= 2;
    }

    if ((v[0] == '5' && v[1] == '.' && v[2] != '9') ||
        (v[0] == '4' && v[1] == '.' && v[2] == '9'))
        lolwut5Command(c);
    else if ((v[0] == '6' && v[1] == '.' && v[2] != '9') ||
             (v[0] == '5' && v[1] == '.' && v[2] == '9'))
        lolwut6Command(c);
    else
        lolwutUnstableCommand(c);

    /* Fix back argc/argv in case of VERSION argument. */
    if (v == verstr) {
        c->argv -= 2;
        c->argc += 2;
    }
}

/* ========================== LOLWUT Canvase ===============================
 * Many LOLWUT versions will likely print some computer art to the screen.
 * This is the case with LOLWUT 5 and LOLWUT 6, so here there is a generic
 * canvas implementation that can be reused.  */

/* Allocate and return a new canvas of the specified size. */
lwCanvas *lwCreateCanvas(int width, int height, int bgcolor) {
    lwCanvas *canvas = zmalloc(sizeof(*canvas));
    canvas->width = width;
    canvas->height = height;
    canvas->pixels = zmalloc(width*height);
    memset(canvas->pixels,bgcolor,width*height);
    return canvas;
}

/* Free the canvas created by lwCreateCanvas(). */
void lwFreeCanvas(lwCanvas *canvas) {
    zfree(canvas->pixels);
    zfree(canvas);
}

/* Set a pixel to the specified color. Color is 0 or 1, where zero means no
 * dot will be displayed, and 1 means dot will be displayed.
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
void lwDrawSquare(lwCanvas *canvas, int x, int y, float size, float angle, int color) {
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
        lwDrawLine(canvas,px[j],py[j],px[(j+1)%4],py[(j+1)%4],color);
}
