/*
 * Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
 * Copyright (c) 2015-2016, Salvatore Sanfilippo <antirez@gmail.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This is a C++ to C conversion from the ardb project.
 * This file started out as:
 * https://github.com/yinqiwen/ardb/blob/d42503/src/geo/geohash_helper.cpp
 */

#include "geohash_helper.h"
#include <math.h>

#define D_R (M_PI / 180.0)
#define R_MAJOR 6378137.0
#define R_MINOR 6356752.3142
#define RATIO (R_MINOR / R_MAJOR)
#define ECCENT (sqrt(1.0 - (RATIO *RATIO)))
#define COM (0.5 * ECCENT)

/// @brief The usual PI/180 constant
const double DEG_TO_RAD = 0.017453292519943295769236907684886;
/// @brief Earth's quatratic mean radius for WGS-84
const double EARTH_RADIUS_IN_METERS = 6372797.560856;

const double MERCATOR_MAX = 20037726.37;
const double MERCATOR_MIN = -20037726.37;

static inline double deg_rad(double ang) { return ang * D_R; }
static inline double rad_deg(double ang) { return ang / D_R; }

/* This function is used in order to estimate the step (bits precision)
 * of the 9 search area boxes during radius queries. */
uint8_t geohashEstimateStepsByRadius(double range_meters, double lat) {
    if (range_meters == 0) return 26;
    int step = 1;
    while (range_meters < MERCATOR_MAX) {
        range_meters *= 2;
        step++;
    }
    step -= 2; /* Make sure range is included in most of the base cases. */

    /* Wider range torwards the poles... Note: it is possible to do better
     * than this approximation by computing the distance between meridians
     * at this latitude, but this does the trick for now. */
    if (lat > 66 || lat < -66) {
        step--;
        if (lat > 80 || lat < -80) step--;
    }

    /* Frame to valid range. */
    if (step < 1) step = 1;
    if (step > 26) step = 26;
    return step;
}

/* Return the bounding box of the search area centered at latitude,longitude
 * having a radius of radius_meter. bounds[0] - bounds[2] is the minimum
 * and maxium longitude, while bounds[1] - bounds[3] is the minimum and
 * maximum latitude. */
int geohashBoundingBox(double longitude, double latitude, double radius_meters,
                       double *bounds) {
    if (!bounds) return 0;

    bounds[0] = longitude - rad_deg(radius_meters/EARTH_RADIUS_IN_METERS/cos(deg_rad(latitude)));
    bounds[2] = longitude + rad_deg(radius_meters/EARTH_RADIUS_IN_METERS/cos(deg_rad(latitude)));
    bounds[1] = latitude - rad_deg(radius_meters/EARTH_RADIUS_IN_METERS);
    bounds[3] = latitude + rad_deg(radius_meters/EARTH_RADIUS_IN_METERS);
    return 1;
}

/* Return a set of areas (center + 8) that are able to cover a range query
 * for the specified position and radius. */
GeoHashRadius geohashGetAreasByRadius(double longitude, double latitude, double radius_meters) {
    GeoHashRange long_range, lat_range;
    GeoHashRadius radius;
    GeoHashBits hash;
    GeoHashNeighbors neighbors;
    GeoHashArea area;
    double min_lon, max_lon, min_lat, max_lat;
    double bounds[4];
    int steps;

    geohashBoundingBox(longitude, latitude, radius_meters, bounds);
    min_lon = bounds[0];
    min_lat = bounds[1];
    max_lon = bounds[2];
    max_lat = bounds[3];

    steps = geohashEstimateStepsByRadius(radius_meters,latitude);

    geohashGetCoordRange(&long_range,&lat_range);
    geohashEncode(&long_range,&lat_range,longitude,latitude,steps,&hash);
    geohashNeighbors(&hash,&neighbors);
    geohashDecode(long_range,lat_range,hash,&area);

    /* Check if the step is enough at the limits of the covered area.
     * Sometimes when the search area is near an edge of the
     * area, the estimated step is not small enough, since one of the
     * north / south / west / east square is too near to the search area
     * to cover everything. */
    int decrease_step = 0;
    {
        GeoHashArea north, south, east, west;

        geohashDecode(long_range, lat_range, neighbors.north, &north);
        geohashDecode(long_range, lat_range, neighbors.south, &south);
        geohashDecode(long_range, lat_range, neighbors.east, &east);
        geohashDecode(long_range, lat_range, neighbors.west, &west);

        if (geohashGetDistance(longitude,latitude,longitude,north.latitude.max)
            < radius_meters) decrease_step = 1;
        if (geohashGetDistance(longitude,latitude,longitude,south.latitude.min)
            < radius_meters) decrease_step = 1;
        if (geohashGetDistance(longitude,latitude,east.longitude.max,latitude)
            < radius_meters) decrease_step = 1;
        if (geohashGetDistance(longitude,latitude,west.longitude.min,latitude)
            < radius_meters) decrease_step = 1;
    }

    if (steps > 1 && decrease_step) {
        steps--;
        geohashEncode(&long_range,&lat_range,longitude,latitude,steps,&hash);
        geohashNeighbors(&hash,&neighbors);
        geohashDecode(long_range,lat_range,hash,&area);
    }

    /* Exclude the search areas that are useless. */
    if (area.latitude.min < min_lat) {
        GZERO(neighbors.south);
        GZERO(neighbors.south_west);
        GZERO(neighbors.south_east);
    }
    if (area.latitude.max > max_lat) {
        GZERO(neighbors.north);
        GZERO(neighbors.north_east);
        GZERO(neighbors.north_west);
    }
    if (area.longitude.min < min_lon) {
        GZERO(neighbors.west);
        GZERO(neighbors.south_west);
        GZERO(neighbors.north_west);
    }
    if (area.longitude.max > max_lon) {
        GZERO(neighbors.east);
        GZERO(neighbors.south_east);
        GZERO(neighbors.north_east);
    }
    radius.hash = hash;
    radius.neighbors = neighbors;
    radius.area = area;
    return radius;
}

GeoHashRadius geohashGetAreasByRadiusWGS84(double longitude, double latitude,
                                           double radius_meters) {
    return geohashGetAreasByRadius(longitude, latitude, radius_meters);
}

GeoHashFix52Bits geohashAlign52Bits(const GeoHashBits hash) {
    uint64_t bits = hash.bits;
    bits <<= (52 - hash.step * 2);
    return bits;
}

/* Calculate distance using haversin great circle distance formula. */
double geohashGetDistance(double lon1d, double lat1d, double lon2d, double lat2d) {
    double lat1r, lon1r, lat2r, lon2r, u, v;
    lat1r = deg_rad(lat1d);
    lon1r = deg_rad(lon1d);
    lat2r = deg_rad(lat2d);
    lon2r = deg_rad(lon2d);
    u = sin((lat2r - lat1r) / 2);
    v = sin((lon2r - lon1r) / 2);
    return 2.0 * EARTH_RADIUS_IN_METERS *
           asin(sqrt(u * u + cos(lat1r) * cos(lat2r) * v * v));
}

int geohashGetDistanceIfInRadius(double x1, double y1,
                                 double x2, double y2, double radius,
                                 double *distance) {
    *distance = geohashGetDistance(x1, y1, x2, y2);
    if (*distance > radius) return 0;
    return 1;
}

int geohashGetDistanceIfInRadiusWGS84(double x1, double y1, double x2,
                                      double y2, double radius,
                                      double *distance) {
    return geohashGetDistanceIfInRadius(x1, y1, x2, y2, radius, distance);
}
