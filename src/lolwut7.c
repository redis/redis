/*
 * Copyright (c) 2023-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * ----------------------------------------------------------------------------
 *
 * This file implements the LOLWUT command for Redis 7.
 * It generates a fractal tree pattern using ASCII art.
 */

#include "server.h"
#include "lolwut.h"
#include <math.h>

/* Draw a tree branch using line drawing */
void drawBranch(lwCanvas *canvas, int x1, int y1, int x2, int y2, int color) {
    lwDrawLine(canvas, x1, y1, x2, y2, color);
}

/* Recursive function to draw a fractal tree */
void drawTree(lwCanvas *canvas, int x, int y, int length, double angle, int depth, int color) {
    if (depth == 0 || length < 2) return;
    
    /* Calculate end point of current branch */
    int x2 = x + (int)(length * cos(angle));
    int y2 = y + (int)(length * sin(angle));
    
    /* Draw the branch */
    drawBranch(canvas, x, y, x2, y2, color);
    
    /* Draw child branches with different angles and reduced length */
    double angleLeft = angle - M_PI / 6;   /* 30 degrees left */
    double angleRight = angle + M_PI / 6;  /* 30 degrees right */
    int newLength = length * 0.7;         /* Reduce length by 30% */
    
    /* Recursively draw left and right branches */
    drawTree(canvas, x2, y2, newLength, angleLeft, depth - 1, color);
    drawTree(canvas, x2, y2, newLength, angleRight, depth - 1, color);
}

/* Generate multiple trees to create a forest */
void generateFractalForest(lwCanvas *canvas, int num_trees, int max_depth) {
    int tree_spacing = canvas->width / (num_trees + 1);
    
    for (int i = 0; i < num_trees; i++) {
        int x = tree_spacing * (i + 1);
        int y = canvas->height - 1;  /* Start from bottom */
        int initial_length = canvas->height / 4 + (rand() % (canvas->height / 6));
        double angle = -M_PI / 2;  /* Point upward */
        int depth = max_depth - (rand() % 2); /* Slight variation in depth */
        
        drawTree(canvas, x, y, initial_length, angle, depth, 1);
    }
}

/* Render the canvas to ASCII art using simple characters */
static sds renderTreeCanvas(lwCanvas *canvas) {
    sds text = sdsempty();
    
    for (int y = 0; y < canvas->height; y++) {
        for (int x = 0; x < canvas->width; x++) {
            int pixel = lwGetPixel(canvas, x, y);
            if (pixel) {
                text = sdscatlen(text, "*", 1);
            } else {
                text = sdscatlen(text, " ", 1);
            }
        }
        if (y != canvas->height - 1) {
            text = sdscatlen(text, "\n", 1);
        }
    }
    return text;
}

/* The LOLWUT 7 command:
 *
 * LOLWUT [width] [height] [trees] [depth]
 *
 * By default the command uses 80 columns, 25 rows, 5 trees, depth 6
 */
void lolwut7Command(client *c) {
    long cols = 80;
    long rows = 25;
    long num_trees = 5;
    long max_depth = 6;

    /* Parse the optional arguments if any. */
    if (c->argc > 1 &&
        getLongFromObjectOrReply(c,c->argv[1],&cols,NULL) != C_OK)
        return;

    if (c->argc > 2 &&
        getLongFromObjectOrReply(c,c->argv[2],&rows,NULL) != C_OK)
        return;

    if (c->argc > 3 &&
        getLongFromObjectOrReply(c,c->argv[3],&num_trees,NULL) != C_OK)
        return;

    if (c->argc > 4 &&
        getLongFromObjectOrReply(c,c->argv[4],&max_depth,NULL) != C_OK)
        return;

    /* Limits to ensure reasonable performance */
    if (cols < 10) cols = 10;
    if (cols > 200) cols = 200;
    if (rows < 10) rows = 10;
    if (rows > 100) rows = 100;
    if (num_trees < 1) num_trees = 1;
    if (num_trees > 20) num_trees = 20;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 10) max_depth = 10;

    /* Generate the fractal forest and reply. */
    lwCanvas *canvas = lwCreateCanvas(cols, rows, 0);
    generateFractalForest(canvas, num_trees, max_depth);
    sds rendered = renderTreeCanvas(canvas);
    rendered = sdscat(rendered,
        "\nFractal trees generated using recursive algorithms.\n"
        "Each tree grows with mathematical precision, branching at angles\n"
        "that follow the golden ratio found in nature.\n"
        "\"In every walk in nature, one receives far more than they seek.\" - John Muir\n"
        "Redis ver. ");
    rendered = sdscat(rendered, REDIS_VERSION);
    rendered = sdscatlen(rendered, "\n", 1);
    addReplyVerbatim(c, rendered, sdslen(rendered), "txt");
    sdsfree(rendered);
    lwFreeCanvas(canvas);
} 
