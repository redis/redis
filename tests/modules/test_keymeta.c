/* An example module for attaching metadata to keys.
 *
 * This example lets tests create metadata-key classes and then SET and GET metadata
 * to keys. The 8-byte slot stores a handle to a module-managed allocation; here
 * we use to attach a string per-key.
 *
 * The module pre-registers several metadata classes during initialization and exposes
 * the following commands (via RedisModule_CreateCommand):
 *
 * 1) KEYMETA.REGISTER <4-char-id> <version> [FLAGS]
 *    Register a new metadata-key class during module load.
 *    Returns the <keymeta-class-id> index (Returned from RedisModule_CreateKeyMetaClass)
 *    On failure, returns nil
 *    In a real module it should be registered "automatically" via OnLoad.
 *
 *    FLAGS (colon-separated):
 *      KEEPONCOPY     - Keep metadata on COPY operation
 *      KEEPONRENAME   - Keep metadata on RENAME operation
 *      KEEPONMOVE     - Keep metadata on MOVE operation
 *      UNLINKFREE     - Use unlink callback for async free
 *      RDBLOAD        - Enable rdb_load callback (metadata can be loaded from RDB)
 *      RDBSAVE        - Enable rdb_save callback (metadata can be saved to RDB)
 *      ALLOWIGNORE    - Enable ALLOW_IGNORE flag (graceful discard on load if
 *                       class not registered or no rdb_load callback)
 *
 *    Example: > keymeta.register KMT1 1 KEEPONCOPY:KEEPONRENAME:ALLOWIGNORE:RDBLOAD:RDBSAVE
 *    Example: > keymeta.register KMT2 1 ALLOWIGNORE
 *
 * 2) KEYMETA.SET <4-char-id> <key> <string-value>
 *    Set the string value as metadata to given key.
 *    Note:
 *    - If already set earlier, then it is expected that it will released before setting a
 *      new string. That is why this command should start with trying to get first
 *      metadata for given key.
 *
 * 3) KEYMETA.GET <4-char-id> <key>
 *    Get the metadata attached to the key for the given class.
 *    Returns a string attached to the given key. Or nil if nothing is attached.
 *
 * 4) KEYMETA.UNREGISTER <4-char-id>
 *    This will mark the key metadata class as released. It can later be reused again
 *    by the same class (consider comment above).
 *    Return REDISMODULE_OK/REDISMODULE_ERR.
 *
 * 5) KEYMETA.ACTIVE
 *    Return total number of active metadata at the moment.
 */

#include "redismodule.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Virtualize class IDs for testing. Values: 0 unused, 1..7 used, -1 released */
RedisModuleKeyMetaClassId class_ids[8] = { 0 };

/* Mapping from 4-char-id to class-id */
typedef struct {
    char name[5];  /* 4 chars + null terminator */
    RedisModuleKeyMetaClassId class_id;
} ClassMapping;

#define MAX_CLASS_MAPPINGS 8
static ClassMapping class_mappings[MAX_CLASS_MAPPINGS];
static int num_class_mappings = 0;

/* Reverse lookup: given a class_id, find the 4-char-id name */
static const char* lookupClassName(RedisModuleKeyMetaClassId class_id) {
    for (int i = 0; i < num_class_mappings; i++) {
        if (class_mappings[i].class_id == class_id) {
            return class_mappings[i].name;
        }
    }
    return NULL;
}

/* Track active metadata instances (not yet freed) */
static long long active_metadata_count = 0;

/* Helper functions for class mapping */

/* Add a mapping from 4-char-id to class-id */
static int addClassMapping(const char *name, RedisModuleKeyMetaClassId class_id) {
    if (num_class_mappings >= MAX_CLASS_MAPPINGS) {
        return 0; /* No space */
    }
    strncpy(class_mappings[num_class_mappings].name, name, 4);
    class_mappings[num_class_mappings].name[4] = '\0';
    class_mappings[num_class_mappings].class_id = class_id;
    num_class_mappings++;
    return 1;
}

/* Lookup class-id by 4-char-id. Returns -1 if not found. */
static RedisModuleKeyMetaClassId lookupClassId(const char *name) {
    for (int i = 0; i < num_class_mappings; i++) {
        if (strncmp(class_mappings[i].name, name, 4) == 0) {
            return class_mappings[i].class_id;
        }
    }
    return -1;
}

/* Remove a mapping by 4-char-id */
static int removeClassMapping(const char *name) {
    for (int i = 0; i < num_class_mappings; i++) {
        if (strncmp(class_mappings[i].name, name, 4) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < num_class_mappings - 1; j++) {
                class_mappings[j] = class_mappings[j + 1];
            }
            num_class_mappings--;
            return 1;
        }
    }
    return 0;
}

/* Callback functions for metadata lifecycle */

/* Copy callback - called when a key is copied */
static int KeyMetaCopyCallback(RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    REDISMODULE_NOT_USED(ctx);
    char *str = (char *)*meta;
    /* Note, condition is redundant since cb only invoked when meta != reset_value */
    if (str) {
        char *new_str = strdup(str);
        *meta = (uint64_t)new_str;
        active_metadata_count++; /* New metadata instance created */
    }
    return 1; /* Keep metadata */
}

/* Rename callback - called when a key is renamed. */
static int KeyMetaRenameDiscardCallback(RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(meta);
    return 0;
}

/* Unlink callback - called when a key is unlinked */
static void KeyMetaUnlinkCallback(RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    /* Let's challenge and free early on before free callback */
    /* Note, condition is redundant since cb only invoked when meta != reset_value */
    if (*meta != 0) {
        char *str = (char *)*meta;
        free(str);
        *meta = 0;  /* Set to reset_value !!! */
        active_metadata_count--; /* Metadata instance freed */
    }
    REDISMODULE_NOT_USED(ctx);
}

/* Free callback - called when metadata needs to be freed */
static void KeyMetaFreeCallback(const char *keyname, uint64_t meta) {
    REDISMODULE_NOT_USED(keyname);
    /* Note, condition is redundant since cb only invoked when meta != reset_value */
    if (meta != 0) {
        char *str = (char *)meta;
        free(str);
        active_metadata_count--; /* Metadata instance freed */
    }
}

static int KeyMetaMoveDiscardCallback(RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(meta);
    return 0; /* discard metadata */
}

/* RDB Save Callback - Serialize metadata to RDB
 * This callback is invoked during RDB save to write the metadata value.
 *
 * Parameters:
 *   - rdb: RedisModuleIO context for writing to RDB
 *   - value: The kvobj (key-value object) - not used in this implementation
 *   - meta: Pointer to the 8-byte metadata value (pointer to our string)
 */
static void KeyMetaRDBSaveCallback(RedisModuleIO *rdb, void *value, uint64_t *meta) {
    REDISMODULE_NOT_USED(value);

    /* If metadata is NULL (reset_value), don't save anything */
    if (*meta == 0) return;

    /* Extract the string from the metadata pointer */
    char *metadata_string = (char *)*meta;

    /* Save the string to RDB using SaveStringBuffer */
    RedisModule_SaveStringBuffer(rdb, metadata_string, strlen(metadata_string));
    /* Save more silly data */
    RedisModule_SaveSigned(rdb, 1);
    RedisModule_SaveFloat(rdb, 1.5);
    RedisModule_SaveLongDouble(rdb, 0.333333333333333333L);
}

/* RDB Load Callback - Deserialize metadata from RDB
 * This callback is invoked during RDB load to read the metadata value.
 *
 * Parameters:
 *   - rdb: RedisModuleIO context for reading from RDB
 *   - meta: Pointer to store the loaded 8-byte metadata value
 *   - encver: Encoding version (class version from RDB)
 *
 * Returns:
 *   - 1: Attach metadata to key (success)
 *   - 0: Ignore/skip metadata (not an error)
 *   - -1: Error - abort RDB load
 */
static int KeyMetaRDBLoadCallback(RedisModuleIO *rdb, uint64_t *meta, int encver) {
    REDISMODULE_NOT_USED(encver);

    /* Load the string from RDB using LoadStringBuffer */
    size_t len;
    char *loaded_string = RedisModule_LoadStringBuffer(rdb, &len);

    if (loaded_string == NULL) {
        /* Error loading string */
        return -1;
    }

    /* Allocate and copy the string (LoadStringBuffer returns a buffer that must be freed) */
    char *metadata_string = malloc(len + 1);
    if (metadata_string == NULL) {
        RedisModule_Free(loaded_string);
        return -1;
    }

    memcpy(metadata_string, loaded_string, len);
    metadata_string[len] = '\0';
    RedisModule_Free(loaded_string);

    /* Load the additional data that was saved (must match rdb_save) */
    int64_t signed_val = RedisModule_LoadSigned(rdb);
    float float_val = RedisModule_LoadFloat(rdb);
    long double ldouble_val = RedisModule_LoadLongDouble(rdb);
    /* We don't use these values, just need to consume them from the stream */
    (void)signed_val;
    (void)float_val;
    (void)ldouble_val;

    /* Store the pointer in metadata */
    *meta = (uint64_t)metadata_string;
    active_metadata_count++; /* New metadata instance created */

    /* Return 1 to attach metadata to the key */
    return 1;
}

/* AOF Rewrite Callback - Common implementation for all classes
 * This callback is invoked during AOF rewrite to emit commands that will
 * recreate the metadata when the AOF is loaded.
 *
 * Parameters:
 *   - aof: RedisModuleIO context for writing to AOF
 *   - value: The kvobj (key-value object) - not used in this implementation
 *   - meta: The 8-byte metadata value (pointer to our string)
 *   - class_id: The class ID for this metadata
 */
static void KeyMetaAOFRewriteCallback_Class(RedisModuleIO *aof, void *value, uint64_t meta, RedisModuleKeyMetaClassId class_id) {
    REDISMODULE_NOT_USED(value);

    /* If metadata is NULL (reset_value), don't emit anything */
    if (meta == 0) return;

    /* Extract the string from the metadata pointer */
    char *metadata_string = (char *)meta;

    /* Lookup the 9-byte-id name for this class */
    const char *class_name = lookupClassName(class_id);
    if (!class_name) {
        /* This shouldn't happen, but handle gracefully */
        return;
    }

    /* Get the key name from the AOF IO context */
    const RedisModuleString *key = RedisModule_GetKeyNameFromIO(aof);
    if (!key) {
        /* Key name not available - shouldn't happen during AOF rewrite */
        return;
    }

    /* Emit the KEYMETA.SET command to recreate this metadata
     * Format: KEYMETA.SET <9-byte-id> <key> <string-value> */
    RedisModule_EmitAOF(aof, "KEYMETA.SET", "csc",
                        class_name,           /* c: 9-byte-id (C string) */
                        key,                  /* s: key name (RedisModuleString) */
                        metadata_string);     /* c: metadata value (C string) */
}

/* Individual AOF rewrite callbacks for each class (1-7)
 * Each callback wraps the common implementation with its specific class ID */
static void KeyMetaAOFRewriteCb1(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 1);
}

static void KeyMetaAOFRewriteCb2(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 2);
}

static void KeyMetaAOFRewriteCb3(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 3);
}

static void KeyMetaAOFRewriteCb4(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 4);
}

static void KeyMetaAOFRewriteCb5(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 5);
}

static void KeyMetaAOFRewriteCb6(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 6);
}

static void KeyMetaAOFRewriteCb7(RedisModuleIO *aof, void *value, uint64_t meta) {
    KeyMetaAOFRewriteCallback_Class(aof, value, meta, 7);
}

/* KEYMETA.REGISTER <4-char-id> <version> [KEEPONCOPY:KEEPONRENAME:UNLINKFREE:ALLOWIGNORE:NORDBLOAD:NORDBSAVE] */
static int KeyMetaRegister_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3 || argc > 4) {
        return RedisModule_WrongArity(ctx);
    }

    /* argv[1]: key metadata class name */
    size_t namelen;
    const char *metaname = RedisModule_StringPtrLen(argv[1], &namelen);

    /* argv[2]: key metadata class version */
    long long metaver;
    if (RedisModule_StringToLongLong(argv[2], &metaver) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid version number");
        return REDISMODULE_OK;
    }

    /* Parse optional callback flags */
    int keep_on_copy = 0, keep_on_rename = 0, unlink_free = 0, keep_on_move = 0;
    int allow_ignore = 0;  /* Default: ALLOW_IGNORE disabled */
    int rdb_load = 0;      /* Default: rdb_load disabled */
    int rdb_save = 0;      /* Default: rdb_save disabled */

    if (argc == 4) {
        const char *flags = RedisModule_StringPtrLen(argv[3], NULL);
        if (strstr(flags, "KEEPONCOPY")) keep_on_copy = 1;
        if (strstr(flags, "KEEPONRENAME")) keep_on_rename = 1;
        if (strstr(flags, "UNLINKFREE")) unlink_free = 1;
        if (strstr(flags, "KEEPONMOVE")) keep_on_move = 1;
        if (strstr(flags, "ALLOWIGNORE")) allow_ignore = 1;   /* Enable ALLOW_IGNORE */
        if (strstr(flags, "RDBLOAD")) rdb_load = 1;           /* Enable rdb_load */
        if (strstr(flags, "RDBSAVE")) rdb_save = 1;           /* Enable rdb_save */
    }

    /* Setup configuration */
    RedisModuleKeyMetaClassConfig config = {0};
    config.version = REDISMODULE_KEY_META_VERSION;
    config.flags = allow_ignore ? (1 << REDISMODULE_META_ALLOW_IGNORE) : 0;
    config.reset_value = (uint64_t)NULL;  /* NULL pointer means no resource to free */
    config.rdb_load = rdb_load ? KeyMetaRDBLoadCallback : NULL;
    config.rdb_save = rdb_save ? KeyMetaRDBSaveCallback : NULL;
    switch (num_class_mappings + 1) { /* distinct cb per class */
        case 1: config.aof_rewrite = KeyMetaAOFRewriteCb1; break;
        case 2: config.aof_rewrite = KeyMetaAOFRewriteCb2; break;
        case 3: config.aof_rewrite = KeyMetaAOFRewriteCb3; break;
        case 4: config.aof_rewrite = KeyMetaAOFRewriteCb4; break;
        case 5: config.aof_rewrite = KeyMetaAOFRewriteCb5; break;
        case 6: config.aof_rewrite = KeyMetaAOFRewriteCb6; break;
        case 7: config.aof_rewrite = KeyMetaAOFRewriteCb7; break;
        default: config.aof_rewrite = NULL; break;
    }
    config.free = KeyMetaFreeCallback;
    config.copy = keep_on_copy ? KeyMetaCopyCallback : NULL;
    config.rename = keep_on_rename ? NULL : KeyMetaRenameDiscardCallback;
    config.move = keep_on_move ? NULL : KeyMetaMoveDiscardCallback;
    config.defrag = NULL;
    config.unlink = unlink_free ? KeyMetaUnlinkCallback : NULL;
    config.mem_usage = NULL;
    config.free_effort = NULL;

    /* Create the metadata class */
    RedisModuleKeyMetaClassId class_id = RedisModule_CreateKeyMetaClass(ctx, metaname, (int)metaver, &config);

    if (class_id < 0) {
        RedisModule_ReplyWithError(ctx, "ERR failed to create metadata class");
        return REDISMODULE_OK;
    } else {
        /* Store the mapping from 9-byte-id to class-id */
        if (!addClassMapping(metaname, class_id)) {
            RedisModule_ReplyWithError(ctx, "ERR failed to store class mapping");
            return REDISMODULE_OK;
        }
        RedisModule_ReplyWithLongLong(ctx, class_id);
    }

    return REDISMODULE_OK;
}

/* KEYMETA.SET <9-byte-id> <key> <string-value> */
static int KeyMetaSet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    const char *metaname = RedisModule_StringPtrLen(argv[1], NULL);
    RedisModuleString *keyname = argv[2];
    const char *value = RedisModule_StringPtrLen(argv[3], NULL);

    /* Lookup the metadata class by name */
    RedisModuleKeyMetaClassId class_id = lookupClassId(metaname);
    if (class_id < 0) {
        RedisModule_ReplyWithError(ctx, "ERR metadata class not found");
        return REDISMODULE_OK;
    }

    /* Open the key for writing */
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ | REDISMODULE_WRITE);
    
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_ReplyWithNull(ctx);
        RedisModule_CloseKey(key);
        return REDISMODULE_OK;
    }    

    /* Check if metadata already exists and free it first. 
     * 
     * Note: The caller is responsible for retrieving and freeing any existing 
     *       pointer-based metadata before RM_SetKeyMeta() to a new value 
     */
    uint64_t meta = 0;
    if (RedisModule_GetKeyMeta(class_id, key, &meta) == REDISMODULE_OK) {
        if (meta != 0) {
            free((char *)meta);
            active_metadata_count--; /* Old metadata freed */
        }
    }

    char *new_str = strdup(value);
    int res = RedisModule_SetKeyMeta(class_id, key, (uint64_t)new_str);

    if (res == REDISMODULE_OK) {
        active_metadata_count++; /* New metadata instance created */
    }

    RedisModule_CloseKey(key);

    if (res == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        free(new_str);
        RedisModule_ReplyWithError(ctx, "ERR failed to set metadata");
    }
    return REDISMODULE_OK;
}

/* KEYMETA.GET <9-byte-id> <key> */
static int KeyMetaGet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    const char *metaname = RedisModule_StringPtrLen(argv[1], NULL);
    RedisModuleString *keyname = argv[2];

    /* Lookup the metadata class by name */
    RedisModuleKeyMetaClassId class_id = lookupClassId(metaname);
    if (class_id < 0) {
        RedisModule_ReplyWithError(ctx, "ERR metadata class not found");
        return REDISMODULE_OK;
    }

    /* Open the key for reading */
    RedisModuleKey *key = RedisModule_OpenKey(ctx, keyname, REDISMODULE_READ);
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_ReplyWithNull(ctx);
        RedisModule_CloseKey(key);
        return REDISMODULE_OK;
    }

    /* Get the metadata */
    uint64_t meta = 0;
    int result = RedisModule_GetKeyMeta(class_id, key, &meta);

    RedisModule_CloseKey(key);

    if (result == REDISMODULE_OK && meta != 0) {
        char *str = (char *)meta;
        RedisModule_ReplyWithCString(ctx, str);
    } else {
        RedisModule_ReplyWithNull(ctx);
    }

    return REDISMODULE_OK;
}

/* KEYMETA.UNREGISTER <9-byte-id> */
static int KeyMetaUnregister_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    const char *metaname = RedisModule_StringPtrLen(argv[1], NULL);

    /* Lookup the metadata class by name */
    RedisModuleKeyMetaClassId class_id = lookupClassId(metaname);
    if (class_id < 0) {
        RedisModule_ReplyWithError(ctx, "ERR metadata class not found");
        return REDISMODULE_OK;
    }

    /* Release the metadata class */
    int result = RedisModule_ReleaseKeyMetaClass(class_id);

    if (result == REDISMODULE_OK) {
        /* Remove the mapping */
        removeClassMapping(metaname);
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        RedisModule_ReplyWithError(ctx, "ERR failed to unregister class");
    }
    return REDISMODULE_OK;
}

/* KEYMETA.ACTIVE
 * Returns the total number of active metadata instances that haven't been freed yet.
 * This is useful for testing to verify that metadata is properly cleaned up. */
static int KeyMetaActive_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }
    REDISMODULE_NOT_USED(argv);

    RedisModule_ReplyWithLongLong(ctx, active_metadata_count);
    return REDISMODULE_OK;
}

/* Module initialization */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "test_metakey", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    /* Register commands */
    if (RedisModule_CreateCommand(ctx, "keymeta.register",
        KeyMetaRegister_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "keymeta.set",
        KeyMetaSet_RedisCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "keymeta.get",
        KeyMetaGet_RedisCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "keymeta.unregister",
        KeyMetaUnregister_RedisCommand, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "keymeta.active",
        KeyMetaActive_RedisCommand, "readonly fast", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    long unsigned int i;
    for (i = 0 ; i < sizeof(class_ids) / sizeof(class_ids[0]); i++) {
        if (class_ids[i] > 0)
            RedisModule_ReleaseKeyMetaClass(class_ids[i]);
    }
    return REDISMODULE_OK;
}
