#ifndef __GEO_H__
#define __GEO_H__

#include "redis.h"

void geoEncodeCommand(redisClient *c);
void geoDecodeCommand(redisClient *c);
void geoRadiusByMemberCommand(redisClient *c);
void geoRadiusCommand(redisClient *c);
void geoAddCommand(redisClient *c);

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
typedef struct geoPoint {
    double latitude;
    double longitude;
    double dist;
    double score;
    char *member;
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array;
    size_t buckets;
    size_t used;
} geoArray;

#endif
