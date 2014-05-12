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
#include "geohash.h"

/**
 * Hashing works like this:
 * Divide the world into 4 buckets.  Label each one as such:
 *  -----------------
 *  |       |       |
 *  |       |       |
 *  | 0,1   | 1,1   |
 *  -----------------
 *  |       |       |
 *  |       |       |
 *  | 0,0   | 1,0   |
 *  -----------------
 */

bool geohashGetCoordRange(uint8_t coord_type, GeoHashRange *lat_range,
                          GeoHashRange *long_range) {
    switch (coord_type) {
    case GEO_WGS84_TYPE: {
        /* These are constraints from EPSG:900913 / EPSG:3785 / OSGEO:41001 */
        /* We can't geocode at the north/south pole. */
        lat_range->max = 85.05112878;
        lat_range->min = -85.05112878;
        long_range->max = 180.0;
        long_range->min = -180.0;
        break;
    }
    case GEO_MERCATOR_TYPE: {
        lat_range->max = 20037726.37;
        lat_range->min = -20037726.37;
        long_range->max = 20037726.37;
        long_range->min = -20037726.37;
        break;
    }
    default: { return false; }
    }
    return true;
}

bool geohashEncode(GeoHashRange *lat_range, GeoHashRange *long_range,
                   double latitude, double longitude, uint8_t step,
                   GeoHashBits *hash) {
    uint8_t i;

    if (NULL == hash || step > 32 || step == 0 || RANGEPISZERO(lat_range) ||
        RANGEPISZERO(long_range)) {
        return false;
    }

    hash->bits = 0;
    hash->step = step;

    if (latitude < lat_range->min || latitude > lat_range->max ||
        longitude < long_range->min || longitude > long_range->max) {
        return false;
    }

    for (i = 0; i < step; i++) {
        uint8_t lat_bit, long_bit;

        if (lat_range->max - latitude >= latitude - lat_range->min) {
            lat_bit = 0;
            lat_range->max = (lat_range->max + lat_range->min) / 2;
        } else {
            lat_bit = 1;
            lat_range->min = (lat_range->max + lat_range->min) / 2;
        }
        if (long_range->max - longitude >= longitude - long_range->min) {
            long_bit = 0;
            long_range->max = (long_range->max + long_range->min) / 2;
        } else {
            long_bit = 1;
            long_range->min = (long_range->max + long_range->min) / 2;
        }

        hash->bits <<= 1;
        hash->bits += long_bit;
        hash->bits <<= 1;
        hash->bits += lat_bit;
    }
    return true;
}

bool geohashEncodeType(uint8_t coord_type, double latitude, double longitude,
                       uint8_t step, GeoHashBits *hash) {
    GeoHashRange r[2] = { { 0 } };
    geohashGetCoordRange(coord_type, &r[0], &r[1]);
    return geohashEncode(&r[0], &r[1], latitude, longitude, step, hash);
}

bool geohashEncodeWGS84(double latitude, double longitude, uint8_t step,
                        GeoHashBits *hash) {
    return geohashEncodeType(GEO_WGS84_TYPE, latitude, longitude, step, hash);
}

bool geohashEncodeMercator(double latitude, double longitude, uint8_t step,
                           GeoHashBits *hash) {
    return geohashEncodeType(GEO_MERCATOR_TYPE, latitude, longitude, step,
                             hash);
}

static inline uint8_t get_bit(uint64_t bits, uint8_t pos) {
    return (bits >> pos) & 0x01;
}

bool geohashDecode(const GeoHashRange lat_range, const GeoHashRange long_range,
                   const GeoHashBits hash, GeoHashArea *area) {
    uint8_t i;

    if (HASHISZERO(hash) || NULL == area || RANGEISZERO(lat_range) ||
        RANGEISZERO(long_range)) {
        return false;
    }

    area->hash = hash;
    area->latitude.min = lat_range.min;
    area->latitude.max = lat_range.max;
    area->longitude.min = long_range.min;
    area->longitude.max = long_range.max;

    for (i = 0; i < hash.step; i++) {
        uint8_t lat_bit, long_bit;

        long_bit = get_bit(hash.bits, (hash.step - i) * 2 - 1);
        lat_bit = get_bit(hash.bits, (hash.step - i) * 2 - 2);

        if (lat_bit == 0) {
            area->latitude.max = (area->latitude.max + area->latitude.min) / 2;
        } else {
            area->latitude.min = (area->latitude.max + area->latitude.min) / 2;
        }

        if (long_bit == 0) {
            area->longitude.max =
                (area->longitude.max + area->longitude.min) / 2;
        } else {
            area->longitude.min =
                (area->longitude.max + area->longitude.min) / 2;
        }
    }
    return true;
}

bool geohashDecodeType(uint8_t coord_type, const GeoHashBits hash,
                       GeoHashArea *area) {
    GeoHashRange r[2] = { { 0 } };
    geohashGetCoordRange(coord_type, &r[0], &r[1]);
    return geohashDecode(r[0], r[1], hash, area);
}

bool geohashDecodeWGS84(const GeoHashBits hash, GeoHashArea *area) {
    return geohashDecodeType(GEO_WGS84_TYPE, hash, area);
}

bool geohashDecodeMercator(const GeoHashBits hash, GeoHashArea *area) {
    return geohashDecodeType(GEO_MERCATOR_TYPE, hash, area);
}

bool geohashDecodeAreaToLatLong(const GeoHashArea *area, double *latlong) {
    double y, x;

    if (!latlong)
        return false;

    y = (area->latitude.min + area->latitude.max) / 2;
    x = (area->longitude.min + area->longitude.max) / 2;

    latlong[0] = y;
    latlong[1] = x;
    return true;
}

bool geohashDecodeToLatLongType(uint8_t coord_type, const GeoHashBits hash,
                                double *latlong) {
    GeoHashArea area = { { 0 } };
    if (!latlong || !geohashDecodeType(coord_type, hash, &area))
        return false;
    return geohashDecodeAreaToLatLong(&area, latlong);
}

bool geohashDecodeToLatLongWGS84(const GeoHashBits hash, double *latlong) {
    return geohashDecodeToLatLongType(GEO_WGS84_TYPE, hash, latlong);
}

bool geohashDecodeToLatLongMercator(const GeoHashBits hash, double *latlong) {
    return geohashDecodeToLatLongType(GEO_MERCATOR_TYPE, hash, latlong);
}

static void geohash_move_x(GeoHashBits *hash, int8_t d) {
    if (d == 0)
        return;

    uint64_t x = hash->bits & 0xaaaaaaaaaaaaaaaaLL;
    uint64_t y = hash->bits & 0x5555555555555555LL;

    uint64_t zz = 0x5555555555555555LL >> (64 - hash->step * 2);

    if (d > 0) {
        x = x + (zz + 1);
    } else {
        x = x | zz;
        x = x - (zz + 1);
    }

    x &= (0xaaaaaaaaaaaaaaaaLL >> (64 - hash->step * 2));
    hash->bits = (x | y);
}

static void geohash_move_y(GeoHashBits *hash, int8_t d) {
    if (d == 0)
        return;

    uint64_t x = hash->bits & 0xaaaaaaaaaaaaaaaaLL;
    uint64_t y = hash->bits & 0x5555555555555555LL;

    uint64_t zz = 0xaaaaaaaaaaaaaaaaLL >> (64 - hash->step * 2);
    if (d > 0) {
        y = y + (zz + 1);
    } else {
        y = y | zz;
        y = y - (zz + 1);
    }
    y &= (0x5555555555555555LL >> (64 - hash->step * 2));
    hash->bits = (x | y);
}

void geohashNeighbors(const GeoHashBits *hash, GeoHashNeighbors *neighbors) {
    neighbors->east = *hash;
    neighbors->west = *hash;
    neighbors->north = *hash;
    neighbors->south = *hash;
    neighbors->south_east = *hash;
    neighbors->south_west = *hash;
    neighbors->north_east = *hash;
    neighbors->north_west = *hash;

    geohash_move_x(&neighbors->east, 1);
    geohash_move_y(&neighbors->east, 0);

    geohash_move_x(&neighbors->west, -1);
    geohash_move_y(&neighbors->west, 0);

    geohash_move_x(&neighbors->south, 0);
    geohash_move_y(&neighbors->south, -1);

    geohash_move_x(&neighbors->north, 0);
    geohash_move_y(&neighbors->north, 1);

    geohash_move_x(&neighbors->north_west, -1);
    geohash_move_y(&neighbors->north_west, 1);

    geohash_move_x(&neighbors->north_east, 1);
    geohash_move_y(&neighbors->north_east, 1);

    geohash_move_x(&neighbors->south_east, 1);
    geohash_move_y(&neighbors->south_east, -1);

    geohash_move_x(&neighbors->south_west, -1);
    geohash_move_y(&neighbors->south_west, -1);
}
