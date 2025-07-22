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
   Extension / modules that would like to store the metadata needs to call the registration method. 
   After that the extension can call set/get of metadata in the appropriate size.
   The extension may provide also serializaton / deserializion and 
 */

#include "server.h"
#include "kvobj_extensions.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>


struct {
	const char *name;
	RedisMetadataMethods metadata_methods;
} registerdExtensions[NUM_EXTENSIONS_SUPPORTED] ;


RedisMetadtaStatus registerExtension(const char *name,
									 RedisMetadataMethods *metadata_methods)
{
	for(int i = 0;  i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (registerdExtensions[i].name == 0) {
			registerdExtensions[i].name = name;
			registerdExtensions[i].metadata_methods = *metadata_methods;
			return METADATA_EXTENSION_OK;
		} else if (strcmp(registerdExtensions[i].name , name) == 0) {
			return METADATA_EXTENSION_ALREADY_EXISTS;
		}
	}
	return METADATA_NO_MORE_EXTENSIONS;
}

int ExtensionName2Index(const char *name) {
	for(int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (registerdExtensions[i].name &&
			strcmp(registerdExtensions[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

size_t ExtensionDefaultVal(int i) {
	return registerdExtensions[i].metadata_methods.default_value;
	
}

void freeExtension(void *data, int i) {
	if (registerdExtensions[i].metadata_methods.freeMetadata)
		registerdExtensions[i].metadata_methods.freeMetadata(data);
}

void freeAllExtensions(size_t bitsmap, void *kv) {
	if (bitsmap == 0)
		return;
	char *data = ExtensionGetAllocPtr(bitsmap, kv);
	
	for(int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (BIT_IS_SET(bitsmap, i)) {
			if (registerdExtensions[i].metadata_methods.freeMetadata) {				
				registerdExtensions[i].metadata_methods.freeMetadata(data);
			}
			data += registerdExtensions[i].metadata_methods.metadata_size_bytes;
		}
	}
}


void defragAllExtensions(size_t bitsmap, void *kv) {
	if (bitsmap == 0)
		return;
	char *data = ExtensionGetAllocPtr(bitsmap, kv);
	
	for(int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (BIT_IS_SET(bitsmap, i)) {
			if (registerdExtensions[i].metadata_methods.defragMetadata) {				
				registerdExtensions[i].metadata_methods.defragMetadata(data, activeDefragAlloc);
			}
			data += registerdExtensions[i].metadata_methods.metadata_size_bytes;
		}
	}
}

void serializeAllExtensions(size_t bitsmap, void *kv, struct RedisModuleRdbStream *stream,
							int flags) {
	if (bitsmap == 0)
		return;
	char *data = ExtensionGetAllocPtr(bitsmap, kv);
	for (int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (BIT_IS_SET(bitsmap, i)) {
			if (registerdExtensions[i].metadata_methods.serializeMetadata) {				
				registerdExtensions[i].metadata_methods.serializeMetadata(data, stream, flags);
			}
			data += registerdExtensions[i].metadata_methods.metadata_size_bytes;
		}
	}
}

void deserilaizeAllExtensions(size_t bitsmap, void *kv, struct RedisModuleRdbStream *stream, int flags) {
	if (bitsmap == 0)
		return;
	char *data = ExtensionGetAllocPtr(bitsmap, kv);
	for (int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
		if (BIT_IS_SET(bitsmap, i)) {
			if (registerdExtensions[i].metadata_methods.deserializeMetadata) {				
				registerdExtensions[i].metadata_methods.deserializeMetadata(data, stream, flags);
			}
			data += registerdExtensions[i].metadata_methods.metadata_size_bytes;
		}
	}
}






size_t ExtensionAllocSize(const size_t bitsmap) {
	size_t ret = 0;
	if (bitsmap != 0) {
		for (int i = 0; i < NUM_EXTENSIONS_SUPPORTED; i++) {
			if (BIT_IS_SET(bitsmap, i)) {
				ret += registerdExtensions[i].metadata_methods.metadata_size_bytes;			   
			}
		}
	}
	return ret;
}

// calculate offset in bytes from the alloc ptr 
static size_t ExtensionOffset(const size_t bitsmap, int Extensions_index) {
	size_t ret = 0;
	for (int i = 0; i < Extensions_index; i++) {
		if (BIT_IS_SET(bitsmap, i)) {
			ret += registerdExtensions[i].metadata_methods.metadata_size_bytes;
		}
	}
	return ret;
}


/* return boolean variable (true if set)
   if the extension bit is set copy it to the retval */
int ExtensionGet(size_t bitsmap, int index, const void *kv, size_t *retval) {
	if (!BIT_IS_SET(bitsmap, index)) 		
		return 0;
	if (retval) {
		size_t offset = ExtensionAllocSize(bitsmap) - ExtensionOffset(bitsmap, index);	
		const char *p = ((char *)kv) - offset;
		memcpy(retval, p, registerdExtensions[index].metadata_methods.metadata_size_bytes);
	}
	return 1;
}

/* set the extension  value  the extension bit must be set */

void ExtensionSet(size_t bitsmap, int index, void *kv, const size_t *val) {
	serverAssert(BIT_IS_SET(bitsmap, index));
	size_t offset = ExtensionAllocSize(bitsmap) - ExtensionOffset(bitsmap, index);	
	char *p = ((char *)kv) - offset;
	memcpy(p, val, registerdExtensions[index].metadata_methods.metadata_size_bytes);
}

/* Copy the src metadata and push the val in the correct place */

void ExtensionPush(size_t src_bitsmap, int index, const void *kv, void *dstkv, const size_t *val) {
	serverAssert(!BIT_IS_SET(src_bitsmap, index));
	char *src = ((char *)kv) - ExtensionAllocSize(src_bitsmap);
	BIT_SET(src_bitsmap, index); 
	char *trg = ((char *)dstkv) - ExtensionAllocSize(src_bitsmap);
				 
	for (int i =0; i < NUM_EXTENSIONS_SUPPORTED;  i++) {
		if (i == index) {
			size_t size = registerdExtensions[index].metadata_methods.metadata_size_bytes;
			memcpy(trg, val, size );
			trg+= size;
		} else if (BIT_IS_SET(src_bitsmap, i)) {
			size_t size = registerdExtensions[i].metadata_methods.metadata_size_bytes;
			memcpy(trg, src, size);
			/* clear the memory so free will not be called */
			memset(src, 0, size); 
			src+= size;
			trg+= size;
		}
	}
}

void *ExtensionGetAllocPtr(size_t bitsmap, void *kv) {
	return ((char *)kv) - ExtensionAllocSize(bitsmap);
}
		






