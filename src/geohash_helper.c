/*
 * Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
 * Copyright (c) 2015-current, Redis Ltd.
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

#include "fmacros.h"
#include "geohash_helper.h"
#include "debugmacro.h"
#include <math.h>

#define D_R (M_PI / 180.0)
#define R_MAJOR 6378137.0
#define R_MINOR 6356752.3142
#define RATIO (R_MINOR / R_MAJOR)
#define ECCENT (sqrt(1.0 - (RATIO *RATIO)))
#define COM (0.5 * ECCENT)

/// @brief The usual PI/180 constant
const double DEG_TO_RAD = 0.017453292519943295769236907684886;
/// @brief Earth's quadratic mean radius for WGS-84
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

    /* Wider range towards the poles... Note: it is possible to do better
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

/* Maximum longitude delta (in degrees, [0, 180]) of points at latitude
 * 'lat_rad' within haversine angular radius 'alpha' (passed as sin(alpha/2))
 * of a query center at latitude 'lat0_rad'. */
static double geohashCircleLonDeltaAtLat(double lat0_rad, double lat_rad, double sin_half_alpha) {
    double u = sin((lat_rad - lat0_rad) / 2.0);
    double num = sin_half_alpha * sin_half_alpha - u * u;
    if (num <= 0.0) return 0.0;
    double denom = cos(lat0_rad) * cos(lat_rad);
    if (denom <= 0.0 || num >= denom) return 180.0;
    return 2.0 * rad_deg(asin(sqrt(num / denom)));
}

/* Return the bounding box of the search area by shape (see geohash.h GeoShape)
 * bounds[0] - bounds[2] is the minimum and maximum longitude
 * while bounds[1] - bounds[3] is the minimum and maximum latitude.
 * since the higher the latitude, the shorter the arc length, the box shape is as follows
 * (left and right edges are actually bent), as shown in the following diagram:
 *
 *    \-----------------/          --------               \-----------------/
 *     \               /         /          \              \               /
 *      \  (long,lat) /         / (long,lat) \              \  (long,lat) /
 *       \           /         /              \             /             \
 *         ---------          /----------------\           /---------------\
 *  Northern Hemisphere       Southern Hemisphere         Around the equator
 */
int geohashBoundingBox(GeoShape *shape, double *bounds) {
    if (!bounds) return 0;
    double longitude = shape->xy[0];
    double latitude = shape->xy[1];
    double height = shape->conversion * (shape->type == CIRCULAR_TYPE ? shape->t.radius : shape->t.r.height/2);
    double width = shape->conversion * (shape->type == CIRCULAR_TYPE ? shape->t.radius : shape->t.r.width/2);

    const double lat_delta = rad_deg(height/EARTH_RADIUS_IN_METERS);
    double min_lat = latitude - lat_delta;
    double max_lat = latitude + lat_delta;
    if (min_lat < GEO_LAT_MIN) min_lat = GEO_LAT_MIN;
    if (min_lat > GEO_LAT_MAX) min_lat = GEO_LAT_MAX;
    if (max_lat < GEO_LAT_MIN) max_lat = GEO_LAT_MIN;
    if (max_lat > GEO_LAT_MAX) max_lat = GEO_LAT_MAX;

    double lon_delta;
    if (shape->type == CIRCULAR_TYPE) {
        double angular_rad = height / EARTH_RADIUS_IN_METERS;
        if (angular_rad >= M_PI) {
            lon_delta = 180.0;
        } else {
            double lat_rad = deg_rad(latitude);
            double sin_half_alpha = sin(angular_rad / 2.0);
            double delta_min = geohashCircleLonDeltaAtLat(lat_rad, deg_rad(min_lat), sin_half_alpha);
            double delta_max = geohashCircleLonDeltaAtLat(lat_rad, deg_rad(max_lat), sin_half_alpha);
            lon_delta = delta_min > delta_max ? delta_min : delta_max;

            /* When angular_rad < pi/2, the circle's longitude span reaches an
             * interior maximum at the spherical tangent latitude
             * sin(tan_lat) = sin(lat) / cos(angular_rad), if within [min_lat, max_lat]. */
            double cos_alpha = cos(angular_rad);
            if (cos_alpha > 0.0) {
                double sin_tan_lat = sin(lat_rad) / cos_alpha;
                if (sin_tan_lat > -1.0 && sin_tan_lat < 1.0) {
                    double tan_lat = rad_deg(asin(sin_tan_lat));
                    if (tan_lat >= min_lat && tan_lat <= max_lat) {
                        double delta_tan = geohashCircleLonDeltaAtLat(lat_rad, deg_rad(tan_lat), sin_half_alpha);
                        if (delta_tan > lon_delta) lon_delta = delta_tan;
                    }
                }
            }
        }
    } else {
        /* RECTANGLE_TYPE: geohashGetDistanceIfInRectangle checks the east-west
         * great-circle distance at the candidate point's own latitude y2:
         * 2 * R * asin(cos(y2) * |sin(dlon/2)|) <= width.
         * On [GEO_LAT_MIN, GEO_LAT_MAX], cos(y2) > 0 decreases with |y2|, so
         * the longitude span is maximized at max(|min_lat|, |max_lat|). */
        if (width >= M_PI * EARTH_RADIUS_IN_METERS) {
            lon_delta = 180.0;
        } else {
            double max_abs_lat = fabs(min_lat) > fabs(max_lat) ? fabs(min_lat) : fabs(max_lat);
            double ratio = sin(width / (2.0 * EARTH_RADIUS_IN_METERS)) / cos(deg_rad(max_abs_lat));
            lon_delta = ratio >= 1.0 ? 180.0 : 2.0 * rad_deg(asin(ratio));
        }
    }

    /* Pad slightly against floating-point rounding on exact cell edges. */
    if (lon_delta > 0.0) {
        lon_delta += 1e-9;
        if (lon_delta > 180.0) lon_delta = 180.0;
    }
    if (lat_delta > 0.0) {
        if (min_lat > GEO_LAT_MIN) {
            min_lat -= 1e-9;
            if (min_lat < GEO_LAT_MIN) min_lat = GEO_LAT_MIN;
        }
        if (max_lat < GEO_LAT_MAX) {
            max_lat += 1e-9;
            if (max_lat > GEO_LAT_MAX) max_lat = GEO_LAT_MAX;
        }
    }

    bounds[0] = longitude - lon_delta;
    bounds[1] = min_lat;
    bounds[2] = longitude + lon_delta;
    bounds[3] = max_lat;
    return 1;
}

/* Calculate a set of areas (center + 8) that are able to cover a range query
 * for the specified position and shape (see geohash.h GeoShape).
 * the bounding box saved in shaple.bounds */
GeoHashRadius geohashCalculateAreasByShapeWGS84(GeoShape *shape) {
    GeoHashRange long_range, lat_range;
    GeoHashRadius radius;
    GeoHashBits hash;
    GeoHashNeighbors neighbors;
    GeoHashArea area, north, south, east, west;
    double min_lon, max_lon, min_lat, max_lat;
    int steps;

    geohashBoundingBox(shape, shape->bounds);
    min_lon = shape->bounds[0];
    min_lat = shape->bounds[1];
    max_lon = shape->bounds[2];
    max_lat = shape->bounds[3];

    double longitude = shape->xy[0];
    double latitude = shape->xy[1];
    /* radius_meters is calculated differently in different search types:
     * 1) CIRCULAR_TYPE, just use radius.
     * 2) RECTANGLE_TYPE, we use sqrt((width/2)^2 + (height/2)^2) to
     * calculate the distance from the center point to the corner */
    double radius_meters = shape->type == CIRCULAR_TYPE ? shape->t.radius :
            sqrt((shape->t.r.width/2)*(shape->t.r.width/2) + (shape->t.r.height/2)*(shape->t.r.height/2));
    radius_meters *= shape->conversion;

    steps = geohashEstimateStepsByRadius(radius_meters,latitude);

    geohashGetCoordRange(&long_range,&lat_range);

    /* Reduce step until the 3x3 neighborhood covers [min_lon, max_lon] and
     * [min_lat, max_lat]. Near the poles or the antimeridian, neighbors can
     * wrap around the grid edges, and wide longitude spans near the poles may
     * need more than a single step reduction. */
    while (1) {
        geohashEncode(&long_range,&lat_range,longitude,latitude,steps,&hash);
        geohashNeighbors(&hash,&neighbors);
        geohashDecode(long_range,lat_range,hash,&area);

        geohashDecode(long_range, lat_range, neighbors.north, &north);
        geohashDecode(long_range, lat_range, neighbors.south, &south);
        geohashDecode(long_range, lat_range, neighbors.east, &east);
        geohashDecode(long_range, lat_range, neighbors.west, &west);

        if (steps <= 1) break;

        int decrease_step = 0;
        if (north.latitude.min > area.latitude.min && north.latitude.max < max_lat)
            decrease_step = 1;
        if (south.latitude.max < area.latitude.max && south.latitude.min > min_lat)
            decrease_step = 1;
        double east_max_lon = east.longitude.max < area.longitude.max ?
                              east.longitude.max + 360.0 : east.longitude.max;
        double west_min_lon = west.longitude.min > area.longitude.min ?
                              west.longitude.min - 360.0 : west.longitude.min;
        if (east_max_lon < max_lon)
            decrease_step = 1;
        if (west_min_lon > min_lon)
            decrease_step = 1;

        if (!decrease_step) break;
        steps--;
    }

    /* Exclude the search areas that are useless, including latitude neighbors
     * that wrapped across GEO_LAT_MIN / GEO_LAT_MAX to the opposite pole. */
    if (steps >= 2) {
        if (area.latitude.min < min_lat ||
            (south.latitude.min > area.latitude.min && south.latitude.min > max_lat))
        {
            GZERO(neighbors.south);
            GZERO(neighbors.south_west);
            GZERO(neighbors.south_east);
        }
        if (area.latitude.max > max_lat ||
            (north.latitude.max < area.latitude.max && north.latitude.max < min_lat))
        {
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
    }
    radius.hash = hash;
    radius.neighbors = neighbors;
    radius.area = area;
    return radius;
}

GeoHashFix52Bits geohashAlign52Bits(const GeoHashBits hash) {
    uint64_t bits = hash.bits;
    bits <<= (52 - hash.step * 2);
    return bits;
}

/* Calculate distance using simplified haversine great circle distance formula.
 * Given longitude diff is 0 the asin(sqrt(a)) on the haversine is asin(sin(abs(u))).
 * arcsin(sin(x)) equal to x when x ∈[−𝜋/2,𝜋/2]. Given latitude is between [−𝜋/2,𝜋/2]
 * we can simplify arcsin(sin(x)) to x.
 */
double geohashGetLatDistance(double lat1d, double lat2d) {
    return EARTH_RADIUS_IN_METERS * fabs(deg_rad(lat2d) - deg_rad(lat1d));
}

/* Calculate distance using haversine great circle distance formula. */
double geohashGetDistance(double lon1d, double lat1d, double lon2d, double lat2d) {
    double lat1r, lon1r, lat2r, lon2r, u, v, a;
    lon1r = deg_rad(lon1d);
    lon2r = deg_rad(lon2d);
    v = sin((lon2r - lon1r) / 2);
    /* if v == 0 we can avoid doing expensive math when lons are practically the same */
    if (v == 0.0)
        return geohashGetLatDistance(lat1d, lat2d);
    lat1r = deg_rad(lat1d);
    lat2r = deg_rad(lat2d);
    u = sin((lat2r - lat1r) / 2);
    a = u * u + cos(lat1r) * cos(lat2r) * v * v;
    return 2.0 * EARTH_RADIUS_IN_METERS * asin(sqrt(a));
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

/* Judge whether a point is in the axis-aligned rectangle, when the distance
 * between a searched point and the center point is less than or equal to
 * height/2 or width/2 in height and width, the point is in the rectangle.
 *
 * width_m, height_m: the rectangle
 * x1, y1 : the center of the box
 * x2, y2 : the point to be searched
 */
int geohashGetDistanceIfInRectangle(double width_m, double height_m, double x1, double y1,
                                    double x2, double y2, double *distance) {
    /* latitude distance is less expensive to compute than longitude distance
     * so we check first for the latitude condition */
    double lat_distance = geohashGetLatDistance(y2, y1);
    if (lat_distance > height_m/2) {
        return 0;
    }
    double lon_distance = geohashGetDistance(x2, y2, x1, y2);
    if (lon_distance > width_m/2) {
        return 0;
    }
    *distance = geohashGetDistance(x1, y1, x2, y2);
    return 1;
}
