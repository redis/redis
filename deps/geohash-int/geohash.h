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

#ifndef GEOHASH_H_
#define GEOHASH_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define HASHISZERO(r) (!(r).bits && !(r).step)
#define RANGEISZERO(r) (!(r).max && !(r).min)
#define RANGEPISZERO(r) (r == NULL || RANGEISZERO(*r))

#define GEO_WGS84_TYPE 1
#define GEO_MERCATOR_TYPE 2

#define GEO_STEP_MAX 26

typedef enum {
    GEOHASH_NORTH = 0,
    GEOHASH_EAST,
    GEOHASH_WEST,
    GEOHASH_SOUTH,
    GEOHASH_SOUTH_WEST,
    GEOHASH_SOUTH_EAST,
    GEOHASH_NORT_WEST,
    GEOHASH_NORT_EAST
} GeoDirection;

typedef struct {
    uint64_t bits;
    uint8_t step;
} GeoHashBits;

typedef struct {
    double max;
    double min;
} GeoHashRange;

typedef struct {
    GeoHashBits hash;
    GeoHashRange latitude;
    GeoHashRange longitude;
} GeoHashArea;

typedef struct {
    GeoHashBits north;
    GeoHashBits east;
    GeoHashBits west;
    GeoHashBits south;
    GeoHashBits north_east;
    GeoHashBits south_east;
    GeoHashBits north_west;
    GeoHashBits south_west;
} GeoHashNeighbors;

/*
 * 0:success
 * -1:failed
 */
bool geohashGetCoordRange(uint8_t coord_type, GeoHashRange *lat_range,
                          GeoHashRange *long_range);
bool geohashEncode(GeoHashRange *lat_range, GeoHashRange *long_range,
                   double latitude, double longitude, uint8_t step,
                   GeoHashBits *hash);
bool geohashEncodeType(uint8_t coord_type, double latitude, double longitude,
                       uint8_t step, GeoHashBits *hash);
bool geohashEncodeMercator(double latitude, double longitude, uint8_t step,
                           GeoHashBits *hash);
bool geohashEncodeWGS84(double latitude, double longitude, uint8_t step,
                        GeoHashBits *hash);
bool geohashDecode(const GeoHashRange lat_range, const GeoHashRange long_range,
                   const GeoHashBits hash, GeoHashArea *area);
bool geohashDecodeType(uint8_t coord_type, const GeoHashBits hash,
                       GeoHashArea *area);
bool geohashDecodeMercator(const GeoHashBits hash, GeoHashArea *area);
bool geohashDecodeWGS84(const GeoHashBits hash, GeoHashArea *area);
bool geohashDecodeAreaToLatLong(const GeoHashArea *area, double *latlong);
bool geohashDecodeToLatLongType(uint8_t coord_type, const GeoHashBits hash,
                                double *latlong);
bool geohashDecodeToLatLongWGS84(const GeoHashBits hash, double *latlong);
bool geohashDecodeToLatLongMercator(const GeoHashBits hash, double *latlong);
void geohashNeighbors(const GeoHashBits *hash, GeoHashNeighbors *neighbors);

#if defined(__cplusplus)
}
#endif
#endif /* GEOHASH_H_ */
