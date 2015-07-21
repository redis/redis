/*
 * Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
 * Copyright (c) 2015, Salvatore Sanfilippo <antirez@gmail.com>.
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

#ifndef GEOHASH_HELPER_HPP_
#define GEOHASH_HELPER_HPP_

#include <math.h>
#include "geohash.h"

#define GZERO(s) s.bits = s.step = 0;
#define GISZERO(s) (!s.bits && !s.step)
#define GISNOTZERO(s) (s.bits || s.step)

typedef uint64_t GeoHashFix52Bits;
typedef uint64_t GeoHashVarBits;

typedef struct {
    GeoHashBits hash;
    GeoHashArea area;
    GeoHashNeighbors neighbors;
} GeoHashRadius;

int GeoHashBitsComparator(const GeoHashBits *a, const GeoHashBits *b);
uint8_t geohashEstimateStepsByRadius(double range_meters, double lat);
int geohashBoundingBox(double longitude, double latitude, double radius_meters,
                        double *bounds);
GeoHashRadius geohashGetAreasByRadius(double longitude,
                                      double latitude, double radius_meters);
GeoHashRadius geohashGetAreasByRadiusWGS84(double longitude, double latitude,
                                           double radius_meters);
GeoHashRadius geohashGetAreasByRadiusMercator(double longitude, double latitude,
                                              double radius_meters);
GeoHashFix52Bits geohashAlign52Bits(const GeoHashBits hash);
double geohashGetDistance(double lon1d, double lat1d,
                          double lon2d, double lat2d);
int geohashGetDistanceIfInRadius(double x1, double y1,
                                 double x2, double y2, double radius,
                                 double *distance);
int geohashGetDistanceIfInRadiusWGS84(double x1, double y1, double x2,
                                      double y2, double radius,
                                      double *distance);

#endif /* GEOHASH_HELPER_HPP_ */
