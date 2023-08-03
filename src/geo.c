/*
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
 * Copyright (c) 2015-2016, Salvatore Sanfilippo <antirez@gmail.com>.
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

#include "geo.h"
#include "geohash_helper.h"
#include "debugmacro.h"
#include "pqsort.h"

/* Things exported from t_zset.c only for geo.c, since it is the only other
 * part of Redis that requires close zset introspection. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
int zslValueLteMax(double value, zrangespec *spec);

/* ====================================================================
 * This file implements the following commands:
 *
 *   - geoadd - add coordinates for value to geoset
 *   - georadius - search radius by coordinates in geoset
 *   - georadiusbymember - search radius based on geoset member position
 * ==================================================================== */

/* ====================================================================
 * geoArray implementation
 * ==================================================================== */

/* Create a new array of geoPoints. */
geoArray *geoArrayCreate(void) {
    geoArray *ga = zmalloc(sizeof(*ga));
    /* It gets allocated on first geoArrayAppend() call. */
    ga->array = NULL;
    ga->buckets = 0;
    ga->used = 0;
    return ga;
}

/* Add and populate with data a new entry to the geoArray. */
geoPoint *geoArrayAppend(geoArray *ga, double *xy, double dist,
                         double score, char *member)
{
    if (ga->used == ga->buckets) {
        ga->buckets = (ga->buckets == 0) ? 8 : ga->buckets*2;
        ga->array = zrealloc(ga->array,sizeof(geoPoint)*ga->buckets);
    }
    geoPoint *gp = ga->array+ga->used;
    gp->longitude = xy[0];
    gp->latitude = xy[1];
    gp->dist = dist;
    gp->member = member;
    gp->score = score;
    ga->used++;
    return gp;
}

/* Destroy a geoArray created with geoArrayCreate(). */
void geoArrayFree(geoArray *ga) {
    size_t i;
    for (i = 0; i < ga->used; i++) sdsfree(ga->array[i].member);
    zfree(ga->array);
    zfree(ga);
}

/* ====================================================================
 * Helpers
 * ==================================================================== */
int decodeGeohash(double bits, double *xy) {
    GeoHashBits hash = { .bits = (uint64_t)bits, .step = GEO_STEP_MAX };
    return geohashDecodeToLongLatWGS84(hash, xy);
}

/* Input Argument Helper */
/* Take a pointer to the latitude arg then use the next arg for longitude.
 * On parse error C_ERR is returned, otherwise C_OK. */
int extractLongLatOrReply(client *c, robj **argv, double *xy) {
    int i;
    for (i = 0; i < 2; i++) {
        if (getDoubleFromObjectOrReply(c, argv[i], xy + i, NULL) !=
            C_OK) {
            return C_ERR;
        }
    }
    if (xy[0] < GEO_LONG_MIN || xy[0] > GEO_LONG_MAX ||
        xy[1] < GEO_LAT_MIN  || xy[1] > GEO_LAT_MAX) {
        addReplyErrorFormat(c,
            "-ERR invalid longitude,latitude pair %f,%f\r\n",xy[0],xy[1]);
        return C_ERR;
    }
    return C_OK;
}

/* Input Argument Helper */
/* Decode lat/long from a zset member's score.
 * Returns C_OK on successful decoding, otherwise C_ERR is returned. */
int longLatFromMember(robj *zobj, robj *member, double *xy) {
    double score = 0;

    if (zsetScore(zobj, member->ptr, &score) == C_ERR) return C_ERR;
    if (!decodeGeohash(score, xy)) return C_ERR;
    return C_OK;
}

/* Check that the unit argument matches one of the known units, and returns
 * the conversion factor to meters (you need to divide meters by the conversion
 * factor to convert to the right unit).
 *
 * If the unit is not valid, an error is reported to the client, and a value
 * less than zero is returned. */
double extractUnitOrReply(client *c, robj *unit) {
    char *u = unit->ptr;

    if (!strcasecmp(u, "m")) {
        return 1;
    } else if (!strcasecmp(u, "km")) {
        return 1000;
    } else if (!strcasecmp(u, "ft")) {
        return 0.3048;
    } else if (!strcasecmp(u, "mi")) {
        return 1609.34;
    } else {
        addReplyError(c,
            "unsupported unit provided. please use M, KM, FT, MI");
        return -1;
    }
}

/* Input Argument Helper.
 * Extract the distance from the specified two arguments starting at 'argv'
 * that should be in the form: <number> <unit>, and return C_OK or C_ERR means success or failure
 * *conversions is populated with the coefficient to use in order to convert meters to the unit.*/
int extractDistanceOrReply(client *c, robj **argv,
                              double *conversion, double *radius) {
    double distance;
    if (getDoubleFromObjectOrReply(c, argv[0], &distance,
                                   "need numeric radius") != C_OK) {
        return C_ERR;
    }

    if (distance < 0) {
        addReplyError(c,"radius cannot be negative");
        return C_ERR;
    }
    if (radius) *radius = distance;

    double to_meters = extractUnitOrReply(c,argv[1]);
    if (to_meters < 0) {
        return C_ERR;
    }

    if (conversion) *conversion = to_meters;
    return C_OK;
}

/* Input Argument Helper.
 * Extract height and width from the specified three arguments starting at 'argv'
 * that should be in the form: <number> <number> <unit>, and return C_OK or C_ERR means success or failure
 * *conversions is populated with the coefficient to use in order to convert meters to the unit.*/
int extractBoxOrReply(client *c, robj **argv, double *conversion,
                         double *width, double *height) {
    double h, w;
    if ((getDoubleFromObjectOrReply(c, argv[0], &w, "need numeric width") != C_OK) ||
        (getDoubleFromObjectOrReply(c, argv[1], &h, "need numeric height") != C_OK)) {
        return C_ERR;
    }

    if (h < 0 || w < 0) {
        addReplyError(c, "height or width cannot be negative");
        return C_ERR;
    }
    if (height) *height = h;
    if (width) *width = w;

    double to_meters = extractUnitOrReply(c,argv[2]);
    if (to_meters < 0) {
        return C_ERR;
    }

    if (conversion) *conversion = to_meters;
    return C_OK;
}

/* The default addReplyDouble has too much accuracy.  We use this
 * for returning location distances. "5.2145 meters away" is nicer
 * than "5.2144992818115 meters away." We provide 4 digits after the dot
 * so that the returned value is decently accurate even when the unit is
 * the kilometer. */
void addReplyDoubleDistance(client *c, double d) {
    char dbuf[128];
    const int dlen = fixedpoint_d2string(dbuf, sizeof(dbuf), d, 4);
    addReplyBulkCBuffer(c, dbuf, dlen);
}

/* Helper function for geoGetPointsInRange(): given a sorted set score
 * representing a point, and a GeoShape, checks if the point is within the search area.
 *
 * shape: the rectangle
 * score: the encoded version of lat,long
 * xy: output variable, the decoded lat,long
 * distance: output variable, the distance between the center of the shape and the point
 *
 * Return values:
 *
 * The return value is C_OK if the point is within search area, or C_ERR if it is outside.
 * "*xy" is populated with the decoded lat,long.
 * "*distance" is populated with the distance between the center of the shape and the point.
 */
int geoWithinShape(GeoShape *shape, double score, double *xy, double *distance) {
    if (!decodeGeohash(score,xy)) return C_ERR; /* Can't decode. */
    /* Note that geohashGetDistanceIfInRadiusWGS84() takes arguments in
     * reverse order: longitude first, latitude later. */
    if (shape->type == CIRCULAR_TYPE) {
        if (!geohashGetDistanceIfInRadiusWGS84(shape->xy[0], shape->xy[1], xy[0], xy[1],
                                               shape->t.radius*shape->conversion, distance))
            return C_ERR;
    } else if (shape->type == RECTANGLE_TYPE) {
        if (!geohashGetDistanceIfInRectangle(shape->t.r.width * shape->conversion,
                                             shape->t.r.height * shape->conversion,
                                             shape->xy[0], shape->xy[1], xy[0], xy[1], distance))
            return C_ERR;
    }
    return C_OK;
}

/* Query a Redis sorted set to extract all the elements between 'min' and
 * 'max', appending them into the array of geoPoint structures 'geoArray'.
 * The command returns the number of elements added to the array.
 *
 * Elements which are farther than 'radius' from the specified 'x' and 'y'
 * coordinates are not included.
 *
 * The ability of this function to append to an existing set of points is
 * important for good performances because querying by radius is performed
 * using multiple queries to the sorted set, that we later need to sort
 * via qsort. Similarly we need to be able to reject points outside the search
 * radius area ASAP in order to allocate and process more points than needed. */
int geoGetPointsInRange(robj *zobj, double min, double max, GeoShape *shape, geoArray *ga, unsigned long limit) {
    /* minex 0 = include min in range; maxex 1 = exclude max in range */
    /* That's: min <= val < max */
    zrangespec range = { .min = min, .max = max, .minex = 0, .maxex = 1 };
    size_t origincount = ga->used;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr = NULL;
        unsigned int vlen = 0;
        long long vlong = 0;
        double score = 0;

        if ((eptr = zzlFirstInRange(zl, &range)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return 0;
        }

        sptr = lpNext(zl, eptr);
        while (eptr) {
            double xy[2];
            double distance = 0;
            score = zzlGetScore(sptr);

            /* If we fell out of range, break. */
            if (!zslValueLteMax(score, &range))
                break;

            vstr = lpGetValue(eptr, &vlen, &vlong);
            if (geoWithinShape(shape, score, xy, &distance) == C_OK) {
                /* Append the new element. */
                char *member = (vstr == NULL) ? sdsfromlonglong(vlong) : sdsnewlen(vstr, vlen);
                geoArrayAppend(ga, xy, distance, score, member);
            }
            if (ga->used && limit && ga->used >= limit) break;
            zzlNext(zl, &eptr, &sptr);
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        if ((ln = zslFirstInRange(zsl, &range)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return 0;
        }

        while (ln) {
            double xy[2];
            double distance = 0;
            /* Abort when the node is no longer in range. */
            if (!zslValueLteMax(ln->score, &range))
                break;
            if (geoWithinShape(shape, ln->score, xy, &distance) == C_OK) {
                /* Append the new element. */
                geoArrayAppend(ga, xy, distance, ln->score, sdsdup(ln->ele));
            }
            if (ga->used && limit && ga->used >= limit) break;
            ln = ln->level[0].forward;
        }
    }
    return ga->used - origincount;
}

/* Compute the sorted set scores min (inclusive), max (exclusive) we should
 * query in order to retrieve all the elements inside the specified area
 * 'hash'. The two scores are returned by reference in *min and *max. */
void scoresOfGeoHashBox(GeoHashBits hash, GeoHashFix52Bits *min, GeoHashFix52Bits *max) {
    /* We want to compute the sorted set scores that will include all the
     * elements inside the specified Geohash 'hash', which has as many
     * bits as specified by hash.step * 2.
     *
     * So if step is, for example, 3, and the hash value in binary
     * is 101010, since our score is 52 bits we want every element which
     * is in binary: 101010?????????????????????????????????????????????
     * Where ? can be 0 or 1.
     *
     * To get the min score we just use the initial hash value left
     * shifted enough to get the 52 bit value. Later we increment the
     * 6 bit prefix (see the hash.bits++ statement), and get the new
     * prefix: 101011, which we align again to 52 bits to get the maximum
     * value (which is excluded from the search). So we get everything
     * between the two following scores (represented in binary):
     *
     * 1010100000000000000000000000000000000000000000000000 (included)
     * and
     * 1010110000000000000000000000000000000000000000000000 (excluded).
     */
    *min = geohashAlign52Bits(hash);
    hash.bits++;
    *max = geohashAlign52Bits(hash);
}

/* Obtain all members between the min/max of this geohash bounding box.
 * Populate a geoArray of GeoPoints by calling geoGetPointsInRange().
 * Return the number of points added to the array. */
int membersOfGeoHashBox(robj *zobj, GeoHashBits hash, geoArray *ga, GeoShape *shape, unsigned long limit) {
    GeoHashFix52Bits min, max;

    scoresOfGeoHashBox(hash,&min,&max);
    return geoGetPointsInRange(zobj, min, max, shape, ga, limit);
}

/* Search all eight neighbors + self geohash box */
int membersOfAllNeighbors(robj *zobj, const GeoHashRadius *n, GeoShape *shape, geoArray *ga, unsigned long limit) {
    GeoHashBits neighbors[9];
    unsigned int i, count = 0, last_processed = 0;
    int debugmsg = 0;

    neighbors[0] = n->hash;
    neighbors[1] = n->neighbors.north;
    neighbors[2] = n->neighbors.south;
    neighbors[3] = n->neighbors.east;
    neighbors[4] = n->neighbors.west;
    neighbors[5] = n->neighbors.north_east;
    neighbors[6] = n->neighbors.north_west;
    neighbors[7] = n->neighbors.south_east;
    neighbors[8] = n->neighbors.south_west;

    /* For each neighbor (*and* our own hashbox), get all the matching
     * members and add them to the potential result list. */
    for (i = 0; i < sizeof(neighbors) / sizeof(*neighbors); i++) {
        if (HASHISZERO(neighbors[i])) {
            if (debugmsg) D("neighbors[%d] is zero",i);
            continue;
        }

        /* Debugging info. */
        if (debugmsg) {
            GeoHashRange long_range, lat_range;
            geohashGetCoordRange(&long_range,&lat_range);
            GeoHashArea myarea = {{0}};
            geohashDecode(long_range, lat_range, neighbors[i], &myarea);

            /* Dump center square. */
            D("neighbors[%d]:\n",i);
            D("area.longitude.min: %f\n", myarea.longitude.min);
            D("area.longitude.max: %f\n", myarea.longitude.max);
            D("area.latitude.min: %f\n", myarea.latitude.min);
            D("area.latitude.max: %f\n", myarea.latitude.max);
            D("\n");
        }

        /* When a huge Radius (in the 5000 km range or more) is used,
         * adjacent neighbors can be the same, leading to duplicated
         * elements. Skip every range which is the same as the one
         * processed previously. */
        if (last_processed &&
            neighbors[i].bits == neighbors[last_processed].bits &&
            neighbors[i].step == neighbors[last_processed].step)
        {
            if (debugmsg)
                D("Skipping processing of %d, same as previous\n",i);
            continue;
        }
        if (ga->used && limit && ga->used >= limit) break;
        count += membersOfGeoHashBox(zobj, neighbors[i], ga, shape, limit);
        last_processed = i;
    }
    return count;
}

/* Sort comparators for qsort() */
static int sort_gp_asc(const void *a, const void *b) {
    const struct geoPoint *gpa = a, *gpb = b;
    /* We can't do adist - bdist because they are doubles and
     * the comparator returns an int. */
    if (gpa->dist > gpb->dist)
        return 1;
    else if (gpa->dist == gpb->dist)
        return 0;
    else
        return -1;
}

static int sort_gp_desc(const void *a, const void *b) {
    return -sort_gp_asc(a, b);
}

/* ====================================================================
 * Commands
 * ==================================================================== */

/* GEOADD key [CH] [NX|XX] long lat name [long2 lat2 name2 ... longN latN nameN] */
void geoaddCommand(client *c) {
    int xx = 0, nx = 0, longidx = 2;
    int i;

    /* Parse options. At the end 'longidx' is set to the argument position
     * of the longitude of the first element. */
    while (longidx < c->argc) {
        char *opt = c->argv[longidx]->ptr;
        if (!strcasecmp(opt,"nx")) nx = 1;
        else if (!strcasecmp(opt,"xx")) xx = 1;
        else if (!strcasecmp(opt,"ch")) { /* Handle in zaddCommand. */ }
        else break;
        longidx++;
    }

    if ((c->argc - longidx) % 3 || (xx && nx)) {
        /* Need an odd number of arguments if we got this far... */
            addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Set up the vector for calling ZADD. */
    int elements = (c->argc - longidx) / 3;
    int argc = longidx+elements*2; /* ZADD key [CH] [NX|XX] score ele ... */
    robj **argv = zcalloc(argc*sizeof(robj*));
    argv[0] = createRawStringObject("zadd",4);
    for (i = 1; i < longidx; i++) {
        argv[i] = c->argv[i];
        incrRefCount(argv[i]);
    }

    /* Create the argument vector to call ZADD in order to add all
     * the score,value pairs to the requested zset, where score is actually
     * an encoded version of lat,long. */
    for (i = 0; i < elements; i++) {
        double xy[2];

        if (extractLongLatOrReply(c, (c->argv+longidx)+(i*3),xy) == C_ERR) {
            for (i = 0; i < argc; i++)
                if (argv[i]) decrRefCount(argv[i]);
            zfree(argv);
            return;
        }

        /* Turn the coordinates into the score of the element. */
        GeoHashBits hash;
        geohashEncodeWGS84(xy[0], xy[1], GEO_STEP_MAX, &hash);
        GeoHashFix52Bits bits = geohashAlign52Bits(hash);
        robj *score = createStringObjectFromLongLongWithSds(bits);
        robj *val = c->argv[longidx + i * 3 + 2];
        argv[longidx+i*2] = score;
        argv[longidx+1+i*2] = val;
        incrRefCount(val);
    }

    /* Finally call ZADD that will do the work for us. */
    replaceClientCommandVector(c,argc,argv);
    zaddCommand(c);
}

#define SORT_NONE 0
#define SORT_ASC 1
#define SORT_DESC 2

#define RADIUS_COORDS (1<<0)    /* Search around coordinates. */
#define RADIUS_MEMBER (1<<1)    /* Search around member. */
#define RADIUS_NOSTORE (1<<2)   /* Do not accept STORE/STOREDIST option. */
#define GEOSEARCH (1<<3)        /* GEOSEARCH command variant (different arguments supported) */
#define GEOSEARCHSTORE (1<<4)   /* GEOSEARCHSTORE just accept STOREDIST option */

/* GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                               [COUNT count [ANY]] [STORE key|STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ...
 * GEOSEARCH key [FROMMEMBER member] [FROMLONLAT long lat] [BYRADIUS radius unit]
 *               [BYBOX width height unit] [WITHCOORD] [WITHDIST] [WITHASH] [COUNT count [ANY]] [ASC|DESC]
 * GEOSEARCHSTORE dest_key src_key [FROMMEMBER member] [FROMLONLAT long lat] [BYRADIUS radius unit]
 *               [BYBOX width height unit] [COUNT count [ANY]] [ASC|DESC] [STOREDIST]
 *  */
void georadiusGeneric(client *c, int srcKeyIndex, int flags) {
    robj *storekey = NULL;
    int storedist = 0; /* 0 for STORE, 1 for STOREDIST. */

    /* Look up the requested zset */
    robj *zobj = lookupKeyRead(c->db, c->argv[srcKeyIndex]);
    if (checkType(c, zobj, OBJ_ZSET)) return;

    /* Find long/lat to use for radius or box search based on inquiry type */
    int base_args;
    GeoShape shape = {0};
    if (flags & RADIUS_COORDS) {
        /* GEORADIUS or GEORADIUS_RO */
        base_args = 6;
        shape.type = CIRCULAR_TYPE;
        if (extractLongLatOrReply(c, c->argv + 2, shape.xy) == C_ERR) return;
        if (extractDistanceOrReply(c, c->argv+base_args-2, &shape.conversion, &shape.t.radius) != C_OK) return;
    } else if ((flags & RADIUS_MEMBER) && !zobj) {
        /* We don't have a source key, but we need to proceed with argument
         * parsing, so we know which reply to use depending on the STORE flag. */
        base_args = 5;
    } else if (flags & RADIUS_MEMBER) {
        /* GEORADIUSBYMEMBER or GEORADIUSBYMEMBER_RO */
        base_args = 5;
        shape.type = CIRCULAR_TYPE;
        robj *member = c->argv[2];
        if (longLatFromMember(zobj, member, shape.xy) == C_ERR) {
            addReplyError(c, "could not decode requested zset member");
            return;
        }
        if (extractDistanceOrReply(c, c->argv+base_args-2, &shape.conversion, &shape.t.radius) != C_OK) return;
    } else if (flags & GEOSEARCH) {
        /* GEOSEARCH or GEOSEARCHSTORE */
        base_args = 2;
        if (flags & GEOSEARCHSTORE) {
            base_args = 3;
            storekey = c->argv[1];
        }
    } else {
        addReplyError(c, "Unknown georadius search type");
        return;
    }

    /* Discover and populate all optional parameters. */
    int withdist = 0, withhash = 0, withcoords = 0;
    int frommember = 0, fromloc = 0, byradius = 0, bybox = 0;
    int sort = SORT_NONE;
    int any = 0; /* any=1 means a limited search, stop as soon as enough results were found. */
    long long count = 0;  /* Max number of results to return. 0 means unlimited. */
    if (c->argc > base_args) {
        int remaining = c->argc - base_args;
        for (int i = 0; i < remaining; i++) {
            char *arg = c->argv[base_args + i]->ptr;
            if (!strcasecmp(arg, "withdist")) {
                withdist = 1;
            } else if (!strcasecmp(arg, "withhash")) {
                withhash = 1;
            } else if (!strcasecmp(arg, "withcoord")) {
                withcoords = 1;
            } else if (!strcasecmp(arg, "any")) {
                any = 1;
            } else if (!strcasecmp(arg, "asc")) {
                sort = SORT_ASC;
            } else if (!strcasecmp(arg, "desc")) {
                sort = SORT_DESC;
            } else if (!strcasecmp(arg, "count") && (i+1) < remaining) {
                if (getLongLongFromObjectOrReply(c, c->argv[base_args+i+1],
                                                 &count, NULL) != C_OK) return;
                if (count <= 0) {
                    addReplyError(c,"COUNT must be > 0");
                    return;
                }
                i++;
            } else if (!strcasecmp(arg, "store") &&
                       (i+1) < remaining &&
                       !(flags & RADIUS_NOSTORE) &&
                       !(flags & GEOSEARCH))
            {
                storekey = c->argv[base_args+i+1];
                storedist = 0;
                i++;
            } else if (!strcasecmp(arg, "storedist") &&
                       (i+1) < remaining &&
                       !(flags & RADIUS_NOSTORE) &&
                       !(flags & GEOSEARCH))
            {
                storekey = c->argv[base_args+i+1];
                storedist = 1;
                i++;
            } else if (!strcasecmp(arg, "storedist") &&
                       (flags & GEOSEARCH) &&
                       (flags & GEOSEARCHSTORE))
            {
                storedist = 1;
            } else if (!strcasecmp(arg, "frommember") &&
                      (i+1) < remaining &&
                      flags & GEOSEARCH &&
                      !fromloc)
            {
                /* No source key, proceed with argument parsing and return an error when done. */
                if (zobj == NULL) {
                    frommember = 1;
                    i++;
                    continue;
                }

                if (longLatFromMember(zobj, c->argv[base_args+i+1], shape.xy) == C_ERR) {
                    addReplyError(c, "could not decode requested zset member");
                    return;
                }
                frommember = 1;
                i++;
            } else if (!strcasecmp(arg, "fromlonlat") &&
                       (i+2) < remaining &&
                       flags & GEOSEARCH &&
                       !frommember)
            {
                if (extractLongLatOrReply(c, c->argv+base_args+i+1, shape.xy) == C_ERR) return;
                fromloc = 1;
                i += 2;
            } else if (!strcasecmp(arg, "byradius") &&
                       (i+2) < remaining &&
                       flags & GEOSEARCH &&
                       !bybox)
            {
                if (extractDistanceOrReply(c, c->argv+base_args+i+1, &shape.conversion, &shape.t.radius) != C_OK)
                    return;
                shape.type = CIRCULAR_TYPE;
                byradius = 1;
                i += 2;
            } else if (!strcasecmp(arg, "bybox") &&
                       (i+3) < remaining &&
                       flags & GEOSEARCH &&
                       !byradius)
            {
                if (extractBoxOrReply(c, c->argv+base_args+i+1, &shape.conversion, &shape.t.r.width,
                        &shape.t.r.height) != C_OK) return;
                shape.type = RECTANGLE_TYPE;
                bybox = 1;
                i += 3;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Trap options not compatible with STORE and STOREDIST. */
    if (storekey && (withdist || withhash || withcoords)) {
        addReplyErrorFormat(c,
            "%s is not compatible with WITHDIST, WITHHASH and WITHCOORD options",
            flags & GEOSEARCHSTORE? "GEOSEARCHSTORE": "STORE option in GEORADIUS");
        return;
    }

    if ((flags & GEOSEARCH) && !(frommember || fromloc)) {
        addReplyErrorFormat(c,
            "exactly one of FROMMEMBER or FROMLONLAT can be specified for %s",
            (char *)c->argv[0]->ptr);
        return;
    }

    if ((flags & GEOSEARCH) && !(byradius || bybox)) {
        addReplyErrorFormat(c,
            "exactly one of BYRADIUS and BYBOX can be specified for %s",
            (char *)c->argv[0]->ptr);
        return;
    }

    if (any && !count) {
        addReplyErrorFormat(c, "the ANY argument requires COUNT argument");
        return;
    }

    /* Return ASAP when src key does not exist. */
    if (zobj == NULL) {
        if (storekey) {
            /* store key is not NULL, try to delete it and return 0. */
            if (dbDelete(c->db, storekey)) {
                signalModifiedKey(c, c->db, storekey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", storekey, c->db->id);
                server.dirty++;
            }
            addReply(c, shared.czero);
        } else {
            /* Otherwise we return an empty array. */
            addReply(c, shared.emptyarray);
        }
        return;
    }

    /* COUNT without ordering does not make much sense (we need to
     * sort in order to return the closest N entries),
     * force ASC ordering if COUNT was specified but no sorting was
     * requested. Note that this is not needed for ANY option. */
    if (count != 0 && sort == SORT_NONE && !any) sort = SORT_ASC;

    /* Get all neighbor geohash boxes for our radius search */
    GeoHashRadius georadius = geohashCalculateAreasByShapeWGS84(&shape);

    /* Search the zset for all matching points */
    geoArray *ga = geoArrayCreate();
    membersOfAllNeighbors(zobj, &georadius, &shape, ga, any ? count : 0);

    /* If no matching results, the user gets an empty reply. */
    if (ga->used == 0 && storekey == NULL) {
        addReply(c,shared.emptyarray);
        geoArrayFree(ga);
        return;
    }

    long result_length = ga->used;
    long returned_items = (count == 0 || result_length < count) ?
                          result_length : count;
    long option_length = 0;

    /* Process [optional] requested sorting */
    if (sort != SORT_NONE) {
        int (*sort_gp_callback)(const void *a, const void *b) = NULL;
        if (sort == SORT_ASC) {
            sort_gp_callback = sort_gp_asc;
        } else if (sort == SORT_DESC) {
            sort_gp_callback = sort_gp_desc;
        }

        if (returned_items == result_length) {
            qsort(ga->array, result_length, sizeof(geoPoint), sort_gp_callback);
        } else {
            pqsort(ga->array, result_length, sizeof(geoPoint), sort_gp_callback,
                0, (returned_items - 1));
        }
    }

    if (storekey == NULL) {
        /* No target key, return results to user. */

        /* Our options are self-contained nested multibulk replies, so we
         * only need to track how many of those nested replies we return. */
        if (withdist)
            option_length++;

        if (withcoords)
            option_length++;

        if (withhash)
            option_length++;

        /* The array len we send is exactly result_length. The result is
         * either all strings of just zset members  *or* a nested multi-bulk
         * reply containing the zset member string _and_ all the additional
         * options the user enabled for this request. */
        addReplyArrayLen(c, returned_items);

        /* Finally send results back to the caller */
        int i;
        for (i = 0; i < returned_items; i++) {
            geoPoint *gp = ga->array+i;
            gp->dist /= shape.conversion; /* Fix according to unit. */

            /* If we have options in option_length, return each sub-result
             * as a nested multi-bulk.  Add 1 to account for result value
             * itself. */
            if (option_length)
                addReplyArrayLen(c, option_length + 1);

            addReplyBulkSds(c,gp->member);
            gp->member = NULL;

            if (withdist)
                addReplyDoubleDistance(c, gp->dist);

            if (withhash)
                addReplyLongLong(c, gp->score);

            if (withcoords) {
                addReplyArrayLen(c, 2);
                addReplyHumanLongDouble(c, gp->longitude);
                addReplyHumanLongDouble(c, gp->latitude);
            }
        }
    } else {
        /* Target key, create a sorted set with the results. */
        robj *zobj;
        zset *zs;
        int i;
        size_t maxelelen = 0, totelelen = 0;

        if (returned_items) {
            zobj = createZsetObject();
            zs = zobj->ptr;
        }

        for (i = 0; i < returned_items; i++) {
            zskiplistNode *znode;
            geoPoint *gp = ga->array+i;
            gp->dist /= shape.conversion; /* Fix according to unit. */
            double score = storedist ? gp->dist : gp->score;
            size_t elelen = sdslen(gp->member);

            if (maxelelen < elelen) maxelelen = elelen;
            totelelen += elelen;
            znode = zslInsert(zs->zsl,score,gp->member);
            serverAssert(dictAdd(zs->dict,gp->member,&znode->score) == DICT_OK);
            gp->member = NULL;
        }

        if (returned_items) {
            zsetConvertToListpackIfNeeded(zobj,maxelelen,totelelen);
            setKey(c,c->db,storekey,zobj,0);
            decrRefCount(zobj);
            notifyKeyspaceEvent(NOTIFY_ZSET,flags & GEOSEARCH ? "geosearchstore" : "georadiusstore",storekey,
                                c->db->id);
            server.dirty += returned_items;
        } else if (dbDelete(c->db,storekey)) {
            signalModifiedKey(c,c->db,storekey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",storekey,c->db->id);
            server.dirty++;
        }
        addReplyLongLong(c, returned_items);
    }
    geoArrayFree(ga);
}

/* GEORADIUS wrapper function. */
void georadiusCommand(client *c) {
    georadiusGeneric(c, 1, RADIUS_COORDS);
}

/* GEORADIUSBYMEMBER wrapper function. */
void georadiusbymemberCommand(client *c) {
    georadiusGeneric(c, 1, RADIUS_MEMBER);
}

/* GEORADIUS_RO wrapper function. */
void georadiusroCommand(client *c) {
    georadiusGeneric(c, 1, RADIUS_COORDS|RADIUS_NOSTORE);
}

/* GEORADIUSBYMEMBER_RO wrapper function. */
void georadiusbymemberroCommand(client *c) {
    georadiusGeneric(c, 1, RADIUS_MEMBER|RADIUS_NOSTORE);
}

void geosearchCommand(client *c) {
    georadiusGeneric(c, 1, GEOSEARCH);
}

void geosearchstoreCommand(client *c) {
    georadiusGeneric(c, 2, GEOSEARCH|GEOSEARCHSTORE);
}

/* GEOHASH key ele1 ele2 ... eleN
 *
 * Returns an array with an 11 characters geohash representation of the
 * position of the specified elements. */
void geohashCommand(client *c) {
    char *geoalphabet= "0123456789bcdefghjkmnpqrstuvwxyz";
    int j;

    /* Look up the requested zset */
    robj *zobj = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, zobj, OBJ_ZSET)) return;

    /* Geohash elements one after the other, using a null bulk reply for
     * missing elements. */
    addReplyArrayLen(c,c->argc-2);
    for (j = 2; j < c->argc; j++) {
        double score;
        if (!zobj || zsetScore(zobj, c->argv[j]->ptr, &score) == C_ERR) {
            addReplyNull(c);
        } else {
            /* The internal format we use for geocoding is a bit different
             * than the standard, since we use as initial latitude range
             * -85,85, while the normal geohashing algorithm uses -90,90.
             * So we have to decode our position and re-encode using the
             * standard ranges in order to output a valid geohash string. */

            /* Decode... */
            double xy[2];
            if (!decodeGeohash(score,xy)) {
                addReplyNull(c);
                continue;
            }

            /* Re-encode */
            GeoHashRange r[2];
            GeoHashBits hash;
            r[0].min = -180;
            r[0].max = 180;
            r[1].min = -90;
            r[1].max = 90;
            geohashEncode(&r[0],&r[1],xy[0],xy[1],26,&hash);

            char buf[12];
            int i;
            for (i = 0; i < 11; i++) {
                int idx;
                if (i == 10) {
                    /* We have just 52 bits, but the API used to output
                     * an 11 bytes geohash. For compatibility we assume
                     * zero. */
                    idx = 0;
                } else {
                    idx = (hash.bits >> (52-((i+1)*5))) & 0x1f;
                }
                buf[i] = geoalphabet[idx];
            }
            buf[11] = '\0';
            addReplyBulkCBuffer(c,buf,11);
        }
    }
}

/* GEOPOS key ele1 ele2 ... eleN
 *
 * Returns an array of two-items arrays representing the x,y position of each
 * element specified in the arguments. For missing elements NULL is returned. */
void geoposCommand(client *c) {
    int j;

    /* Look up the requested zset */
    robj *zobj = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, zobj, OBJ_ZSET)) return;

    /* Report elements one after the other, using a null bulk reply for
     * missing elements. */
    addReplyArrayLen(c,c->argc-2);
    for (j = 2; j < c->argc; j++) {
        double score;
        if (!zobj || zsetScore(zobj, c->argv[j]->ptr, &score) == C_ERR) {
            addReplyNullArray(c);
        } else {
            /* Decode... */
            double xy[2];
            if (!decodeGeohash(score,xy)) {
                addReplyNullArray(c);
                continue;
            }
            addReplyArrayLen(c,2);
            addReplyHumanLongDouble(c,xy[0]);
            addReplyHumanLongDouble(c,xy[1]);
        }
    }
}

/* GEODIST key ele1 ele2 [unit]
 *
 * Return the distance, in meters by default, otherwise according to "unit",
 * between points ele1 and ele2. If one or more elements are missing NULL
 * is returned. */
void geodistCommand(client *c) {
    double to_meter = 1;

    /* Check if there is the unit to extract, otherwise assume meters. */
    if (c->argc == 5) {
        to_meter = extractUnitOrReply(c,c->argv[4]);
        if (to_meter < 0) return;
    } else if (c->argc > 5) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Look up the requested zset */
    robj *zobj = NULL;
    if ((zobj = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp]))
        == NULL || checkType(c, zobj, OBJ_ZSET)) return;

    /* Get the scores. We need both otherwise NULL is returned. */
    double score1, score2, xyxy[4];
    if (zsetScore(zobj, c->argv[2]->ptr, &score1) == C_ERR ||
        zsetScore(zobj, c->argv[3]->ptr, &score2) == C_ERR)
    {
        addReplyNull(c);
        return;
    }

    /* Decode & compute the distance. */
    if (!decodeGeohash(score1,xyxy) || !decodeGeohash(score2,xyxy+2))
        addReplyNull(c);
    else
        addReplyDoubleDistance(c,
            geohashGetDistance(xyxy[0],xyxy[1],xyxy[2],xyxy[3]) / to_meter);
}
