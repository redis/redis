/* Redis Object extension implementation.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* This file handle all the aspects of dynamic extending the kvobj
   Extension / modules that would like to store the metadata need to call the registration method. 
   After that the extension can call set/get of metadata in the appropriate size.
   The extension may provide also serializaton / deserializion defrag etc.. 
 */

#ifndef __REDIS_KVOBJ_EXTENSION_H
#define __REDIS_KVOBJ_EXTENSION_H

struct RedisModuleRdbStream;

#ifndef BIT_IS_SET
#define BIT_IS_SET(o,index) (((o) & (1 << (index))) != 0)
#define BIT_SET(o,index) ((o) |= (1 <<(index)))
#endif

#define ExtensionSetBit(o,index) BIT_SET(o,index) 

/* metadata methods allow Extensions to add and retrive metadata to kvobj */
typedef struct {
    size_t metadata_size_bytes; 
	size_t default_value;	
	void (*serializeMetadata)(void *metadata,
							  struct RedisModuleRdbStream *stream, int flags);
	void (*deserializeMetadata)(void *data,
								struct RedisModuleRdbStream *stream,
								int flags);
	void (*freeMetadata)(void *metadata) ;
	void *(*defragMetadata)(void *metadata, void *((*defragRealloc)(void *)));
} RedisMetadataMethods;


typedef enum {
    METADATA_EXTENSION_OK = 0,
	METADATA_NO_MORE_EXTENSIONS,
	METADATA_EXTENSION_ALREADY_EXISTS
} RedisMetadtaStatus;

/* Register the extension.
   The extension is defined by a unique name.
   internal Extensions such as expire should use  this method as well to register and get an index.
   This function return  METADATA_EXTENSION_OK in case of success and other values in case of error */
RedisMetadtaStatus registerExtension(const char *name,
									 RedisMetadataMethods *metadata_methods);

/* service methods to call the apprpriate function */

void freeExtension(void *p ,int  modle_index);
void freeAllExtensions(size_t bitmaps, void *kvobj);
void defragAllExtensions(size_t bitmaps, void *kvobj);

void  serializeAllExtensions(size_t bitmaps, void *kvobj, struct RedisModuleRdbStream *stream,
						   int flags);
void  deserilaizeAllExtensions(size_t bitmaps, void *kvobj, struct RedisModuleRdbStream *stream, int flags);

size_t ExtensionDefaultVal(int index);
int    ExtensionGet(size_t bitmaps, int index, const void *kvobj, size_t *retval);
void   ExtensionSet(size_t bitmaps, int index, void *kvobj, const size_t *val);
void   *ExtensionGetAllocPtr(size_t bitmaps, void *kvobj);
void   ExtensionPush(size_t src_bitmaps, int index, const void *kvobj, void *dstkvobj, const size_t *val);
size_t ExtensionAllocSize(const size_t bitmaps);
int    ExtensionName2Index(const char *name);
#endif
