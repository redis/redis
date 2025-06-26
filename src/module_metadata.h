/* this file is inteded to be used both by module.c and internally 
 * for "modues" such as expire support and FLEX
 */

struct RedisModuleRdbStream;

/* metadata methods allow modules to add and retrive metadata to kvobj */
typedef struct {
	void (*serializeMetadata)(void *metadata,
							  struct RedisModuleRdbStream *stream, int flags);
	void *(*deserializeMetadata)( struct RedisModuleRdbStream *stream,
								 int flags);

	void (*freeMetadata)(void *metadata) ;
	void *(*defragMetadata)(void *metadata, void *(defragPtrMethod(void *)));
} RedisMetadataMethods;



typedef enum {
    METADATA_MODULE_OK = 0,
	METADATA_NO_MORE_MODULES,
	METADATA_MODULE_ALREADY_EXISTS
} RedisMetadtaStatus;

/* internal modules such as expire and FLEX should use  this method to register and get an index
 this function return  METADATA_MODULE_OK in case of success and other values in case of error */
RedisMetadtaStatus registerInternalMetadataModule(const char *name,
												  RedisMetadataMethods *metadata_methods);

/* service methods to call the apprpriate function for the specific index */
void freeModuleMetadata(void *p ,int  modle_index);
void *defragModuleMetadata(void *p ,int  modle_index);
void serializeModuleMetadata(void *p ,int  modle_index, struct RedisModuleRdbStream *stream,
							 int flags);
void* deserilaizeModuleMetadata(int  modle_index, struct RedisModuleRdbStream *stream, int flags);

