/*
 * Copyright (c) 2019-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * ----------------------------------------------------------------------------
 *
 * This file implements the LOLWUT command. The command should do something
 * fun and interesting, and should be replaced by a new implementation at
 * each new version of Redis.
 *
 * Thanks to Michele Hiki Falcone for the original image that inspired
 * the image, part of his game, Plaguemon.
 *
 * Thanks to the Shhh computer art collective for the help in tuning the
 * output to have a better artistic effect.
 */

#include "server.h"
#include "lolwut.h"

/* Render the canvas using the four gray levels of the standard color
 * terminal: they match very well to the grayscale display of the gameboy. */
static sds renderCanvas(lwCanvas *canvas) {
    sds text = sdsempty();
    for (int y = 0; y < canvas->height; y++) {
        for (int x = 0; x < canvas->width; x++) {
            int color = lwGetPixel(canvas,x,y);
            char *ce; /* Color escape sequence. */

            /* Note that we set both the foreground and background color.
             * This way we are able to get a more consistent result among
             * different terminals implementations. */
            switch(color) {
            case 0: ce = "0;30;40m"; break;    /* Black */
            case 1: ce = "0;90;100m"; break;   /* Gray 1 */
            case 2: ce = "0;37;47m"; break;    /* Gray 2 */
            case 3: ce = "0;97;107m"; break;   /* White */
            default: ce = "0;30;40m"; break;   /* Just for safety. */
            }
            text = sdscatprintf(text,"\033[%s \033[0m",ce);
        }
        if (y != canvas->height-1) text = sdscatlen(text,"\n",1);
    }
    return text;
}

/* Draw a skyscraper on the canvas, according to the parameters in the
 * 'skyscraper' structure. Window colors are random and are always one
 * of the two grays. */
struct skyscraper {
    int xoff;       /* X offset. */
    int width;      /* Pixels width. */
    int height;     /* Pixels height. */
    int windows;    /* Draw windows if true. */
    int color;      /* Color of the skyscraper. */
};

void generateSkyscraper(lwCanvas *canvas, struct skyscraper *si) {
    int starty = canvas->height-1;
    int endy = starty - si->height + 1;
    for (int y = starty; y >= endy; y--) {
        for (int x = si->xoff; x < si->xoff+si->width; x++) {
            /* The roof is four pixels less wide. */
            if (y == endy && (x <= si->xoff+1 || x >= si->xoff+si->width-2))
                continue;
            int color = si->color;
            /* Alter the color if this is a place where we want to
             * draw a window. We check that we are in the inner part of the
             * skyscraper, so that windows are far from the borders. */
            if (si->windows &&
                x > si->xoff+1 &&
                x < si->xoff+si->width-2 &&
                y > endy+1 &&
                y < starty-1)
            {
                /* Calculate the x,y position relative to the start of
                 * the window area. */
                int relx = x - (si->xoff+1);
                int rely = y - (endy+1);

                /* Note that we want the windows to be two pixels wide
                 * but just one pixel tall, because terminal "pixels"
                 * (characters) are not square. */
                if (relx/2 % 2 && rely % 2) {
                    do {
                        color = 1 + rand() % 2;
                    } while (color == si->color);
                    /* Except we want adjacent pixels creating the same
                     * window to be the same color. */
                    if (relx % 2) color = lwGetPixel(canvas,x-1,y);
                }
            }
            lwDrawPixel(canvas,x,y,color);
        }
    }
}

/* Generate a skyline inspired by the parallax backgrounds of 8 bit games. */
void generateSkyline(lwCanvas *canvas) {
    struct skyscraper si;

    /* First draw the background skyscraper without windows, using the
     * two different grays. We use two passes to make sure that the lighter
     * ones are always in the background. */
    for (int color = 2; color >= 1; color--) {
        si.color = color;
        for (int offset = -10; offset < canvas->width;) {
            offset += rand() % 8;
            si.xoff = offset;
            si.width = 10 + rand()%9;
            if (color == 2)
                si.height = canvas->height/2 + rand()%canvas->height/2;
            else
                si.height = canvas->height/2 + rand()%canvas->height/3;
            si.windows = 0;
            generateSkyscraper(canvas, &si);
            if (color == 2)
                offset += si.width/2;
            else
                offset += si.width+1;
        }
    }

    /* Now draw the foreground skyscraper with the windows. */
    si.color = 0;
    for (int offset = -10; offset < canvas->width;) {
        offset += rand() % 8;
        si.xoff = offset;
        si.width = 5 + rand()%14;
        if (si.width % 4) si.width += (si.width % 3);
        si.height = canvas->height/3 + rand()%canvas->height/2;
        si.windows = 1;
        generateSkyscraper(canvas, &si);
        offset += si.width+5;
    }
}

/* The LOLWUT 6 command:
 *
 * LOLWUT [columns] [rows]
 *
 * By default the command uses 80 columns, 40 squares per row
 * per column.
 */
void lolwut6Command(client *c) {
    long cols = 80;
    long rows = 20;

    /* Parse the optional arguments if any. */
    if (c->argc > 1 &&
        getLongFromObjectOrReply(c,c->argv[1],&cols,NULL) != C_OK)
        return;

    if (c->argc > 2 &&
        getLongFromObjectOrReply(c,c->argv[2],&rows,NULL) != C_OK)
        return;

    /* Limits. We want LOLWUT to be always reasonably fast and cheap to execute
     * so we have maximum number of columns, rows, and output resolution. */
    if (cols < 1) cols = 1;
    if (cols > 1000) cols = 1000;
    if (rows < 1) rows = 1;
    if (rows > 1000) rows = 1000;

    /* Generate the city skyline and reply. */
    lwCanvas *canvas = lwCreateCanvas(cols,rows,3);
    generateSkyline(canvas);
    sds rendered = renderCanvas(canvas);
    rendered = sdscat(rendered,
        "\nDedicated to the 8 bit game developers of past and present.\n"
        "Original 8 bit image from Plaguemon by hikikomori. Redis ver. ");
    rendered = sdscat(rendered,REDIS_VERSION);
    rendered = sdscatlen(rendered,"\n",1);
    addReplyVerbatim(c,rendered,sdslen(rendered),"txt");
    sdsfree(rendered);
    lwFreeCanvas(canvas);
}
