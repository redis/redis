/*
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
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
#include "zset.h"

/* ====================================================================
 * Redis Add-on Module: geo
 * Provides commands: geoadd, georadius, georadiusbymember,
 *                    geoencode, geodecode
 * Behaviors:
 *   - geoadd - add coordinates for value to geoset
 *   - georadius - search radius by coordinates in geoset
 *   - georadiusbymember - search radius based on geoset member position
 *   - geoencode - encode coordinates to a geohash integer
 *   - geodecode - decode geohash integer to representative coordinates
 * ==================================================================== */

/* ====================================================================
 * Helpers
 * ==================================================================== */
static inline int decodeGeohash(double bits, double *latlong) {
    GeoHashBits hash = { .bits = (uint64_t)bits, .step = GEO_STEP_MAX };
    return geohashDecodeToLatLongWGS84(hash, latlong);
}

/* Input Argument Helper */
/* Take a pointer to the latitude arg then use the next arg for longitude */
static inline int extractLatLongOrReply(redisClient *c, robj **argv,
                                         double *latlong) {
    for (int i = 0; i < 2; i++) {
        if (getDoubleFromObjectOrReply(c, argv[i], latlong + i, NULL) !=
            REDIS_OK) {
            return 0;
        }
    }
    return 1;
}

/* Input Argument Helper */
/* Decode lat/long from a zset member's score */
static int latLongFromMember(robj *zobj, robj *member, double *latlong) {
    double score = 0;

    if (!zsetScore(zobj, member, &score))
        return 0;

    if (!decodeGeohash(score, latlong))
        return 0;

    return 1;
}

/* Input Argument Helper */
static double extractDistanceOrReply(redisClient *c, robj **argv,
                                     double *conversion) {
    double distance;
    if (getDoubleFromObjectOrReply(c, argv[0], &distance,
                                   "need numeric radius") != REDIS_OK) {
        return -1;
    }

    double to_meters;
    sds units = argv[1]->ptr;
    if (!strcmp(units, "m") || !strncmp(units, "meter", 5)) {
        to_meters = 1;
    } else if (!strcmp(units, "ft") || !strncmp(units, "feet", 4)) {
        to_meters = 0.3048;
    } else if (!strcmp(units, "mi") || !strncmp(units, "mile", 4)) {
        to_meters = 1609.34;
    } else if (!strcmp(units, "km") || !strncmp(units, "kilometer", 9)) {
        to_meters = 1000;
    } else {
        addReplyError(c, "unsupported unit provided. please use meters (m), "
                         "kilometers (km), miles (mi), or feet (ft)");
        return -1;
    }

    if (conversion)
        *conversion = to_meters;

    return distance * to_meters;
}

/* The defailt addReplyDouble has too much accuracy.  We use this
 * for returning location distances. "5.2145 meters away" is nicer
 * than "5.2144992818115 meters away." We provide 4 digits after the dot
 * so that the returned value is decently accurate even when the unit is
 * the kilometer. */
static inline void addReplyDoubleDistance(redisClient *c, double d) {
    char dbuf[128];
    int dlen = snprintf(dbuf, sizeof(dbuf), "%.4f", d);
    addReplyBulkCBuffer(c, dbuf, dlen);
}

/* geohash range+zset access helper */
/* Obtain all members between the min/max of this geohash bounding box. */
/* Returns list of results.  List must be listRelease()'d later. */
static list *membersOfGeoHashBox(robj *zobj, GeoHashBits hash) {
    GeoHashFix52Bits min, max;

    min = geohashAlign52Bits(hash);
    hash.bits++;
    max = geohashAlign52Bits(hash);

    return geozrangebyscore(zobj, min, max, -1); /* -1 = no limit */
}

/* Search all eight neighbors + self geohash box */
static list *membersOfAllNeighbors(robj *zobj, GeoHashRadius n, double x,
                                   double y, double radius) {
    list *l = NULL;
    GeoHashBits neighbors[9];
    unsigned int i;

    neighbors[0] = n.hash;
    neighbors[1] = n.neighbors.north;
    neighbors[2] = n.neighbors.south;
    neighbors[3] = n.neighbors.east;
    neighbors[4] = n.neighbors.west;
    neighbors[5] = n.neighbors.north_east;
    neighbors[6] = n.neighbors.north_west;
    neighbors[7] = n.neighbors.south_east;
    neighbors[8] = n.neighbors.south_west;

    /* For each neighbor (*and* our own hashbox), get all the matching
     * members and add them to the potential result list. */
    for (i = 0; i < sizeof(neighbors) / sizeof(*neighbors); i++) {
        list *r;

        if (HASHISZERO(neighbors[i]))
            continue;

        r = membersOfGeoHashBox(zobj, neighbors[i]);
        if (!r)
            continue;

        if (!l) {
            l = r;
        } else {
            listJoin(l, r);
        }
    }

    /* if no results across any neighbors (*and* ourself, which is unlikely),
     * then just give up. */
    if (!l)
        return NULL;

    /* Iterate over all matching results in the combined 9-grid search area */
    /* Remove any results outside of our search radius. */
    listIter li;
    listNode *ln;
    listRewind(l, &li);
    while ((ln = listNext(&li))) {
        struct zipresult *zr = listNodeValue(ln);
        GeoHashArea area = {{0,0},{0,0},{0,0}};
        GeoHashBits hash = { .bits = (uint64_t)zr->score,
                             .step = GEO_STEP_MAX };

        if (!geohashDecodeWGS84(hash, &area)) {
            /* Perhaps we should delete this node if the decode fails? */
            continue;
        }

        double neighbor_y = (area.latitude.min + area.latitude.max) / 2;
        double neighbor_x = (area.longitude.min + area.longitude.max) / 2;

        double distance;
        if (!geohashGetDistanceIfInRadiusWGS84(x, y, neighbor_x, neighbor_y,
                                               radius, &distance)) {
            /* If result is in the grid, but not in our radius, remove it. */
            listDelNode(l, ln);
#ifdef DEBUG
            fprintf(stderr, "No match for neighbor (%f, %f) within (%f, %f) at "
                            "distance %f\n",
                    neighbor_y, neighbor_x, y, x, distance);
#endif
        } else {
/* Else: bueno. */
#ifdef DEBUG
            fprintf(
                stderr,
                "Matched neighbor (%f, %f) within (%f, %f) at distance %f\n",
                neighbor_y, neighbor_x, y, x, distance);
#endif
            zr->distance = distance;
        }
    }

    /* We found results, but rejected all of them as out of range. Clean up. */
    if (!listLength(l)) {
        listRelease(l);
        l = NULL;
    }

    /* Success! */
    return l;
}

/* With no subscribers, each call of this function adds a median latency of 2
 * microseconds. */
/* We aren't participating in any keyspace/keyevent notifications other than
 * what's provided by the underlying zset itself, but it's probably not useful
 * for clients to get the 52-bit integer geohash as an "update" value. */
static int publishLocationUpdate(const sds zset, const sds member,
                                 const double latitude,
                                 const double longitude) {
    int published;

    /* event is: "<latitude> <longitude>" */
    sds event = sdscatprintf(sdsempty(), "%.7f %.7f", latitude, longitude);
    robj *eventobj = createObject(REDIS_STRING, event);

    /* channel is: __geo:<zset>:<member> */
    /* If you want all events for this zset then just psubscribe
     * to "__geo:<zset>:*" */
    sds chan = sdsnewlen("__geo:", 6);
    chan = sdscatsds(chan, zset);
    chan = sdscatlen(chan, ":", 1);
    chan = sdscatsds(chan, member);
    robj *chanobj = createObject(REDIS_STRING, chan);

    published = pubsubPublishMessage(chanobj, eventobj);

    decrRefCount(chanobj);
    decrRefCount(eventobj);

    return published;
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
void geoAddCommand(redisClient *c) {
    /* args 0-4: [cmd, key, lat, lng, val]; optional 5-6: [radius, units]
     * - OR -
     * args 0-N: [cmd, key, lat, lng, val, lat2, lng2, val2, ...] */
    robj *cmd = c->argv[0];
    robj *key = c->argv[1];

    /* Prepare for the three different forms of the add command. */
    double radius_meters = 0;
    if (c->argc == 7) {
        if ((radius_meters = extractDistanceOrReply(c, c->argv + 5, NULL)) <
            0) {
            return;
        }
    } else if (c->argc == 6) {
        addReplyError(c, "must provide units when asking for radius encode");
        return;
    } else if ((c->argc - 2) % 3 != 0) {
        /* Need an odd number of arguments if we got this far... */
        addReplyError(c, "format is: geoadd [key] [lat1] [long1] [member1] "
                         "[lat2] [long2] [member2] ... ");
        return;
    }

    redisClient *client = c;
    int elements = (c->argc - 2) / 3;
    /* elements will always be correct size (integer math floors for us if we
     * have 6 or 7 total arguments) */
    if (elements > 1) {
        /* We should probably use a static client and not create/free it
         * for every multi-add */
        client = createClient(-1); /* fake client for multi-zadd */

        /* Tell fake client to use the same DB as our calling client. */
        selectDb(client, c->db->id);
    }

    /* Capture all lat/long components up front so if we encounter an error we
     * return before making any changes to the database. */
    double latlong[elements * 2];
    for (int i = 0; i < elements; i++) {
        if (!extractLatLongOrReply(c, (c->argv + 2) + (i * 3),
                                   latlong + (i * 2)))
            return;
    }

    /* Add all (lat, long, value) triples to the requested zset */
    for (int i = 0; i < elements; i++) {
        uint8_t step = geohashEstimateStepsByRadius(radius_meters);

#ifdef DEBUG
        printf("Adding with step size: %d\n", step);
#endif
        GeoHashBits hash;
        int ll_offset = i * 2;
        double latitude = latlong[ll_offset];
        double longitude = latlong[ll_offset + 1];
        geohashEncodeWGS84(latitude, longitude, step, &hash);

        GeoHashFix52Bits bits = geohashAlign52Bits(hash);
        robj *score = createObject(REDIS_STRING, sdsfromlonglong(bits));
        robj *val = c->argv[2 + i * 3 + 2];
        /* (base args) + (offset for this triple) + (offset of value arg) */

        rewriteClientCommandVector(client, 4, cmd, key, score, val);
        decrRefCount(score);
        zaddCommand(client);
        publishLocationUpdate(key->ptr, val->ptr, latitude, longitude);
    }

    /* If we used a fake client, return a real reply then free fake client. */
    if (client != c) {
        addReplyLongLong(c, elements);
        freeClient(client);
    }
}

#define SORT_NONE 0
#define SORT_ASC 1
#define SORT_DESC 2

#define RADIUS_COORDS 1
#define RADIUS_MEMBER 2

static void geoRadiusGeneric(redisClient *c, int type) {
    /* type == cords:  [cmd, key, lat, long, radius, units, [optionals]]
     * type == member: [cmd, key, member,    radius, units, [optionals]] */
    robj *key = c->argv[1];

    /* Look up the requested zset */
    robj *zobj = NULL;
    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptymultibulk)) == NULL ||
        checkType(c, zobj, REDIS_ZSET)) {
        return;
    }

    /* Find lat/long to use for radius search based on inquiry type */
    int base_args;
    double latlong[2] = { 0 };
    if (type == RADIUS_COORDS) {
        base_args = 6;
        if (!extractLatLongOrReply(c, c->argv + 2, latlong))
            return;
    } else if (type == RADIUS_MEMBER) {
        base_args = 5;
        robj *member = c->argv[2];
        if (!latLongFromMember(zobj, member, latlong)) {
            addReplyError(c, "could not decode requested zset member");
            return;
        }
    } else {
        addReplyError(c, "unknown georadius search type");
        return;
    }

    /* Extract radius and units from arguments */
    double radius_meters = 0, conversion = 1;
    if ((radius_meters = extractDistanceOrReply(c, c->argv + base_args - 2,
                                                &conversion)) < 0) {
        return;
    }

    /* Discover and populate all optional parameters. */
    int withdist = 0, withhash = 0, withcoords = 0, noproperties = 0;
    int sort = SORT_NONE;
    if (c->argc > base_args) {
        int remaining = c->argc - base_args;
        for (int i = 0; i < remaining; i++) {
            char *arg = c->argv[base_args + i]->ptr;
            if (!strncasecmp(arg, "withdist", 8))
                withdist = 1;
            else if (!strcasecmp(arg, "withhash"))
                withhash = 1;
            else if (!strncasecmp(arg, "withcoord", 9))
                withcoords = 1;
            else if (!strncasecmp(arg, "noprop", 6) ||
                     !strncasecmp(arg, "withoutprop", 11))
                noproperties = 1;
            else if (!strncasecmp(arg, "asc", 3) ||
                     !strncasecmp(arg, "sort", 4))
                sort = SORT_ASC;
            else if (!strncasecmp(arg, "desc", 4))
                sort = SORT_DESC;
            else {
                addReply(c, shared.syntaxerr);
                return;
            }
        }
    }

    /* Get all neighbor geohash boxes for our radius search */
    GeoHashRadius georadius =
        geohashGetAreasByRadiusWGS84(latlong[0], latlong[1], radius_meters);

#ifdef DEBUG
    printf("Searching with step size: %d\n", georadius.hash.step);
#endif
    /* {Lat, Long} = {y, x} */
    double y = latlong[0];
    double x = latlong[1];

    /* Search the zset for all matching points */
    list *found_matches =
        membersOfAllNeighbors(zobj, georadius, x, y, radius_meters);

    /* If no matching results, the user gets an empty reply. */
    if (!found_matches) {
        addReply(c, shared.emptymultibulk);
        return;
    }

    long result_length = listLength(found_matches);
    long option_length = 0;

    /* Our options are self-contained nested multibulk replies, so we
     * only need to track how many of those nested replies we return. */
    if (withdist)
        option_length++;

    if (withcoords)
        option_length++;

    if (withhash)
        option_length++;

    /* The multibulk len we send is exactly result_length. The result is either
     * all strings of just zset members  *or* a nested multi-bulk reply
     * containing the zset member string _and_ all the additional options the
     * user enabled for this request. */
    addReplyMultiBulkLen(c, result_length);

    /* Iterate over results, populate struct used for sorting and result sending
     */
    listIter li;
    listRewind(found_matches, &li);
    struct geoPoint gp[result_length];
    /* populate gp array from our results */
    for (int i = 0; i < result_length; i++) {
        struct zipresult *zr = listNodeValue(listNext(&li));

        gp[i].member = NULL;
        gp[i].set = key->ptr;
        gp[i].dist = zr->distance / conversion;
        gp[i].userdata = zr;

        /* The layout of geoPoint allows us to pass the start offset
         * of the struct directly to decodeGeohash. */
        decodeGeohash(zr->score, (double *)(gp + i));
    }

    /* Process [optional] requested sorting */
    if (sort == SORT_ASC) {
        qsort(gp, result_length, sizeof(*gp), sort_gp_asc);
    } else if (sort == SORT_DESC) {
        qsort(gp, result_length, sizeof(*gp), sort_gp_desc);
    }

    /* Finally send results back to the caller */
    for (int i = 0; i < result_length; i++) {
        struct zipresult *zr = gp[i].userdata;

        /* If we have options in option_length, return each sub-result
         * as a nested multi-bulk.  Add 1 to account for result value itself. */
        if (option_length)
            addReplyMultiBulkLen(c, option_length + 1);

        switch (zr->type) {
        case ZR_LONG:
            addReplyBulkLongLong(c, zr->val.v);
            break;
        case ZR_STRING:
            addReplyBulkCBuffer(c, zr->val.s, sdslen(zr->val.s));
            break;
        }

        if (withdist)
            addReplyDoubleDistance(c, gp[i].dist);

        if (withhash)
            addReplyLongLong(c, zr->score);

        if (withcoords) {
            addReplyMultiBulkLen(c, 2);
            addReplyDouble(c, gp[i].latitude);
            addReplyDouble(c, gp[i].longitude);
        }
    }
    listRelease(found_matches);
}

void geoRadiusCommand(redisClient *c) {
    /* args 0-5: ["georadius", key, lat, long, radius, units];
     * optionals: [withdist, withcoords, asc|desc] */
    geoRadiusGeneric(c, RADIUS_COORDS);
}

void geoRadiusByMemberCommand(redisClient *c) {
    /* args 0-4: ["georadius", key, compare-against-member, radius, units];
     * optionals: [withdist, withcoords, asc|desc] */
    geoRadiusGeneric(c, RADIUS_MEMBER);
}

void geoDecodeCommand(redisClient *c) {
    GeoHashBits geohash;
    if (getLongLongFromObjectOrReply(c, c->argv[1], (long long *)&geohash.bits,
                                     NULL) != REDIS_OK)
        return;

    GeoHashArea area;
    geohash.step = GEO_STEP_MAX;
    geohashDecodeWGS84(geohash, &area);

    double y = (area.latitude.min + area.latitude.max) / 2;
    double x = (area.longitude.min + area.longitude.max) / 2;

    /* Returning three nested replies */
    addReplyMultiBulkLen(c, 3);

    /* First, the minimum corner */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, area.latitude.min);
    addReplyDouble(c, area.longitude.min);

    /* Next, the maximum corner */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, area.latitude.max);
    addReplyDouble(c, area.longitude.max);

    /* Last, the averaged center of this bounding box */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, y);
    addReplyDouble(c, x);
}

void geoEncodeCommand(redisClient *c) {
    /* args 0-2: ["geoencode", lat, long];
     * optionals: [radius, units] */

    double radius_meters = 0;
    if (c->argc >= 5) {
        if ((radius_meters = extractDistanceOrReply(c, c->argv + 3, NULL)) < 0)
            return;
    } else if (c->argc == 4) {
        addReplyError(c, "must provide units when asking for radius encode");
        return;
    }

    double latlong[2];
    if (!extractLatLongOrReply(c, c->argv + 1, latlong)) return;

    /* Encode lat/long into our geohash */
    GeoHashBits geohash;
    uint8_t step = geohashEstimateStepsByRadius(radius_meters);
    geohashEncodeWGS84(latlong[0], latlong[1], step, &geohash);

    /* Align the hash to a valid 52-bit integer based on step size */
    GeoHashFix52Bits bits = geohashAlign52Bits(geohash);

/* Decode the hash so we can return its bounding box */
#ifdef DEBUG
    printf("Decoding with step size: %d\n", geohash.step);
#endif
    GeoHashArea area;
    geohashDecodeWGS84(geohash, &area);

    double y = (area.latitude.min + area.latitude.max) / 2;
    double x = (area.longitude.min + area.longitude.max) / 2;

    /* Return four nested multibulk replies. */
    addReplyMultiBulkLen(c, 4);

    /* Return the binary geohash we calculated as 52-bit integer */
    addReplyLongLong(c, bits);

    /* Return the minimum corner */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, area.latitude.min);
    addReplyDouble(c, area.longitude.min);

    /* Return the maximum corner */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, area.latitude.max);
    addReplyDouble(c, area.longitude.max);

    /* Return the averaged center */
    addReplyMultiBulkLen(c, 2);
    addReplyDouble(c, y);
    addReplyDouble(c, x);
}
