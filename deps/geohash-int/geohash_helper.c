/*
 * Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
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

double mercator_y(double lat) {
    lat = fmin(89.5, fmax(lat, -89.5));
    double phi = deg_rad(lat);
    double sinphi = sin(phi);
    double con = ECCENT * sinphi;
    con = pow((1.0 - con) / (1.0 + con), COM);
    double ts = tan(0.5 * (M_PI * 0.5 - phi)) / con;
    return 0 - R_MAJOR * log(ts);
}

double mercator_x(double lon) { return R_MAJOR * deg_rad(lon); }
double merc_lon(double x) { return rad_deg(x) / R_MAJOR; }

double merc_lat(double y) {
    double ts = exp(-y / R_MAJOR);
    double phi = M_PI_2 - 2 * atan(ts);
    double dphi = 1.0;
    int i;
    for (i = 0; fabs(dphi) > 0.000000001 && i < 15; i++) {
        double con = ECCENT * sin(phi);
        dphi =
            M_PI_2 - 2 * atan(ts * pow((1.0 - con) / (1.0 + con), COM)) - phi;
        phi += dphi;
    }
    return rad_deg(phi);
}

/* You must *ONLY* estimate steps when you are encoding.
 * If you are decoding, always decode to GEO_STEP_MAX (26). */
uint8_t geohashEstimateStepsByRadius(double range_meters) {
    uint8_t step = 1;
    while (range_meters > 0 && range_meters < MERCATOR_MAX) {
        range_meters *= 2;
        step++;
    }
    step--;
    if (!step)
        step = 26; /* if range = 0, give max resolution */
    return step > 26 ? 26 : step;
}

double geohashGetXWGS84(double x) { return merc_lon(x); }
double geohashGetYWGS84(double y) { return merc_lat(y); }

double geohashGetXMercator(double longtitude) {
    if (longtitude > 180 || longtitude < -180) {
        return longtitude;
    }
    return mercator_x(longtitude);
}
double geohashGetYMercator(double latitude) {
    if (latitude > 90 || latitude < -90) {
        return latitude;
    }
    return mercator_y(latitude);
}

int geohashBitsComparator(const GeoHashBits *a, const GeoHashBits *b) {
    /* If step not equal, compare on step.  Else, compare on bits. */
    return a->step != b->step ? a->step - b->step : a->bits - b->bits;
}

bool geohashBoundingBox(double latitude, double longitude, double radius_meters,
                        double *bounds) {
    if (!bounds)
        return false;

    double latr, lonr;
    latr = deg_rad(latitude);
    lonr = deg_rad(longitude);

    double distance = radius_meters / EARTH_RADIUS_IN_METERS;
    double min_latitude = latr - distance;
    double max_latitude = latr + distance;

    /* Note: we're being lazy and not accounting for coordinates near poles */
    double min_longitude, max_longitude;
    double difference_longitude = asin(sin(distance) / cos(latr));
    min_longitude = lonr - difference_longitude;
    max_longitude = lonr + difference_longitude;

    bounds[0] = rad_deg(min_latitude);
    bounds[1] = rad_deg(min_longitude);
    bounds[2] = rad_deg(max_latitude);
    bounds[3] = rad_deg(max_longitude);

    return true;
}

GeoHashRadius geohashGetAreasByRadius(uint8_t coord_type, double latitude,
                                      double longitude, double radius_meters) {
    GeoHashRange lat_range, long_range;
    GeoHashRadius radius = { { 0 } };
    GeoHashBits hash = { 0 };
    GeoHashNeighbors neighbors = { { 0 } };
    GeoHashArea area = { { 0 } };
    double delta_longitude, delta_latitude;
    double min_lat, max_lat, min_lon, max_lon;
    int steps;

    if (coord_type == GEO_WGS84_TYPE) {
        double bounds[4];
        geohashBoundingBox(latitude, longitude, radius_meters, bounds);
        min_lat = bounds[0];
        min_lon = bounds[1];
        max_lat = bounds[2];
        max_lon = bounds[3];
    } else {
        delta_latitude = delta_longitude = radius_meters;
        min_lat = latitude - delta_latitude;
        max_lat = latitude + delta_latitude;
        min_lon = longitude - delta_longitude;
        max_lon = longitude + delta_longitude;
    }

    steps = geohashEstimateStepsByRadius(radius_meters);

    geohashGetCoordRange(coord_type, &lat_range, &long_range);
    geohashEncode(&lat_range, &long_range, latitude, longitude, steps, &hash);
    geohashNeighbors(&hash, &neighbors);
    geohashDecode(lat_range, long_range, hash, &area);

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

GeoHashRadius geohashGetAreasByRadiusWGS84(double latitude, double longitude,
                                           double radius_meters) {
    return geohashGetAreasByRadius(GEO_WGS84_TYPE, latitude, longitude,
                                   radius_meters);
}

GeoHashRadius geohashGetAreasByRadiusMercator(double latitude, double longitude,
                                              double radius_meters) {
    return geohashGetAreasByRadius(GEO_MERCATOR_TYPE, latitude, longitude,
                                   radius_meters);
}

GeoHashFix52Bits geohashAlign52Bits(const GeoHashBits hash) {
    uint64_t bits = hash.bits;
    bits <<= (52 - hash.step * 2);
    return bits;
}

/* calculate distance using haversin great circle distance formula */
double distanceEarth(double lat1d, double lon1d, double lat2d, double lon2d) {
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

bool geohashGetDistanceIfInRadius(uint8_t coord_type, double x1, double y1,
                                  double x2, double y2, double radius,
                                  double *distance) {
    if (coord_type == GEO_WGS84_TYPE) {
        *distance = distanceEarth(y1, x1, y2, x2);
        if (*distance > radius) {
            return false;
        }
    } else {
        double xx = (x1 - x2) * (x1 - x2);
        double yy = (y1 - y2) * (y1 - y2);
        double dd = xx + yy;
        *distance = dd;
        if (dd > (radius * radius)) {
            return false;
        }
    }
    return true;
}

bool geohashGetDistanceIfInRadiusWGS84(double x1, double y1, double x2,
                                       double y2, double radius,
                                       double *distance) {
    return geohashGetDistanceIfInRadius(GEO_WGS84_TYPE, x1, y1, x2, y2, radius,
                                        distance);
}

bool geohashGetDistanceSquaredIfInRadiusMercator(double x1, double y1,
                                                 double x2, double y2,
                                                 double radius,
                                                 double *distance) {
    return geohashGetDistanceIfInRadius(GEO_MERCATOR_TYPE, x1, y1, x2, y2,
                                        radius, distance);
}

bool geohashVerifyCoordinates(uint8_t coord_type, double x, double y) {
    GeoHashRange lat_range, long_range;
    geohashGetCoordRange(coord_type, &lat_range, &long_range);

    if (x < long_range.min || x > long_range.max || y < lat_range.min ||
        y > lat_range.max) {
        return false;
    }
    return true;
}
