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

#include "geojson.h"

#define L server.lua

/* ====================================================================
 * The Encoder
 * ==================================================================== */
static sds jsonEncode() {
    /* When entering this function, stack is: [1:[geojson table to encode]] */
    lua_getglobal(L, "cjson");
    lua_getfield(L, -1, "encode");

    /* Stack is now: [1:[geojson table], 2:'cjson', 3:'encode'] */

    /* Move current top ('encode') to bottom of stack */
    lua_insert(L, 1);

    /* Move current top ('cjson') to bottom of stack so we can 'cjson.encode' */
    lua_insert(L, 1);

    /* Stack is now: [1:'cjson', 2:'encode', 3:[table of geojson to encode]] */

    /* Call cjson.encode on the element above it on the stack;
     * obtain one return value */
    if (lua_pcall(L, 1, 1, 0) != 0)
        redisLog(REDIS_WARNING, "Could not encode geojson: %s",
                 lua_tostring(L, -1));

    sds geojson = sdsnew(lua_tostring(L, -1));

    /* We're done.  Remove entire stack.  Drop mic.  Walk away. */
    lua_pop(L, lua_gettop(L));

    /* Return sds the caller must sdsfree() on their own */
    return geojson;
}

/* ====================================================================
 * The Lua Helpers
 * ==================================================================== */
static inline void luaCreateFieldFromPrevious(const char *field) {
    lua_setfield(L, -2, field);
}

static inline void luaCreateFieldStr(const char *field, const char *value) {
    lua_pushstring(L, value);
    luaCreateFieldFromPrevious(field);
}

/* Creates [Lat, Long] array attached to "coordinates" key */
static void luaCreateCoordinates(const double x, const double y) {
    /* Create array table with two elements */
    lua_createtable(L, 2, 0);

    lua_pushnumber(L, x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, y);
    lua_rawseti(L, -2, 2);
}

static void luaCreatePropertyNull(void) {
    /* Create empty table and give it a name.  This is a json {} value. */
    lua_createtable(L, 0, 0);
    luaCreateFieldFromPrevious("properties");
}

static void _luaCreateProperties(const char *k1, const char *v1, const char *k2,
                                 const char *v2, const int noclose) {
    /* we may add additional properties outside of here, so newtable instead of
     * fixed-size createtable */
    lua_newtable(L);

    luaCreateFieldStr(k1, v1);
    luaCreateFieldStr(k2, v2);

    if (!noclose)
        luaCreateFieldFromPrevious("properties");
}

static void luaCreateProperties(const char *k1, const char *v1, const char *k2,
                                const char *v2) {
    _luaCreateProperties(k1, v1, k2, v2, 0);
}

/* ====================================================================
 * The Lua Aggregation Helpers
 * ==================================================================== */
static void attachProperties(const char *set, const char *member) {
    if (member)
        luaCreateProperties("set", set, "member", member);
    else
        luaCreatePropertyNull();
}

static void attachPropertiesWithDist(const char *set, const char *member,
                                     double dist, const char *units) {
    if (member) {
        _luaCreateProperties("set", set, "member", member, 1);
        if (units) {
            /* Add units then distance. After encoding it comes
             * out as distance followed by units in the json. */
            lua_pushstring(L, units);
            luaCreateFieldFromPrevious("units");
            lua_pushnumber(L, dist);
            luaCreateFieldFromPrevious("distance");
        }

        /* We requested to leave the properties table open, but now we
         * are done and can close it. */
        luaCreateFieldFromPrevious("properties");
    } else {
        luaCreatePropertyNull();
    }
}

static void createGeometryPoint(const double x, const double y) {
    lua_createtable(L, 0, 2);

    /* coordinates = [x, y] */
    luaCreateCoordinates(x, y);
    luaCreateFieldFromPrevious("coordinates");

    /* type = Point */
    luaCreateFieldStr("type", "Point");

    /* geometry = (coordinates = [x, y]) */
    luaCreateFieldFromPrevious("geometry");
}

static void createGeometryBox(const double x1, const double y1, const double x2,
                              const double y2) {
    lua_createtable(L, 0, 2);

    /* Result = [[[x1,y1],[x2,y1],[x2,y2],[x1,y2], [x1,y1]] */
    /* The end coord is the start coord to make a closed polygon */
    lua_createtable(L, 1, 0);
    lua_createtable(L, 5, 0);

    /* Bottom left */
    luaCreateCoordinates(x1, y1);
    lua_rawseti(L, -2, 1);

    /* Top Left */
    luaCreateCoordinates(x2, y1);
    lua_rawseti(L, -2, 2);

    /* Top Right */
    luaCreateCoordinates(x2, y2);
    lua_rawseti(L, -2, 3);

    /* Bottom Right */
    luaCreateCoordinates(x1, y2);
    lua_rawseti(L, -2, 4);

    /* Bottom Left (Again) */
    luaCreateCoordinates(x1, y1);
    lua_rawseti(L, -2, 5);

    /* Set the outer array of our inner array of the inner coords */
    lua_rawseti(L, -2, 1);

    /* Bundle those together in coordinates: [a, b, c, d] */
    luaCreateFieldFromPrevious("coordinates");

    /* Add type field */
    luaCreateFieldStr("type", "Polygon");

    luaCreateFieldFromPrevious("geometry");
}

static void createFeature() {
    /* Features have three fields: type, geometry, and properties */
    lua_createtable(L, 0, 3);

    luaCreateFieldStr("type", "Feature");

    /* You must call attachProperties on your own */
}

static void createCollection(size_t size) {
    /* FeatureCollections have two fields: type and features */
    lua_createtable(L, 0, 2);

    luaCreateFieldStr("type", "FeatureCollection");
}

static void pointsToCollection(const struct geojsonPoint *pts, const size_t len,
                               const char *units) {
    createCollection(len);

    lua_createtable(L, len, 0);
    for (int i = 0; i < len; i++) {
        createFeature();
        createGeometryPoint(pts[i].longitude, pts[i].latitude); /* x, y */
        attachPropertiesWithDist(pts[i].set, pts[i].member, pts[i].dist, units);
        lua_rawseti(L, -2, i + 1); /* Attach this Feature to "features" array */
    }
    luaCreateFieldFromPrevious("features");
}

static void latLongToPointFeature(const double latitude,
                                  const double longitude) {
    createFeature();
    createGeometryPoint(longitude, latitude); /* geojson is: x,y */
}

static void squareToPolygonFeature(const double x1, const double y1,
                                   const double x2, const double y2) {
    createFeature();
    createGeometryBox(x1, y1, x2, y2);
}

/* ====================================================================
 * The Interface Functions
 * ==================================================================== */
sds geojsonFeatureCollection(const struct geojsonPoint *pts, const size_t len,
                             const char *units) {
    pointsToCollection(pts, len, units);
    return jsonEncode();
}

sds geojsonLatLongToPointFeature(const double latitude, const double longitude,
                                 const char *set, const char *member,
                                 const double dist, const char *units) {
    latLongToPointFeature(latitude, longitude);
    attachPropertiesWithDist(set, member, dist, units);
    return jsonEncode();
}

sds geojsonBoxToPolygonFeature(const double y1, const double x1,
                               const double y2, const double x2,
                               const char *set, const char *member) {
    squareToPolygonFeature(x1, y1, x2, y2);
    attachProperties(set, member);
    return jsonEncode();
}
