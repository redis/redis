/* An example module for attaching metadata to keys.
 *
 * This example lets tests create metadata-key classes and then SET and GET metadata
 * to keys. The 8-byte slot stores a handle to a module-managed allocation; here
 * we use to attach a string per-key.
 *
 * The module pre-registers several metadata classes during initialization and exposes
 * the following commands (via RedisModule_CreateCommand):
 *
 * 1) KEYMETA.REGISTER <9-byte-id> <version> [KEEPONCOPY:KEEPONRENAME:UNLINKFREE]
 *    Register a new metadata-key class during module load.
 *    Returns the <keymeta-class-id> index (Returned from RedisModule_CreateKeyMetaClass)
 *    On failure, returns nil
 *
 *    Example: > keymeta.register KMTEST001 1 KEEPONCOPY:KEEPONRENAME
 *
 * 2) KEYMETA.SET <key> <metadata-class-id> <string-value>
 *    Set the string value as metadata to given key.
 *    Note:
 *    - If already set earlier, then it is expected that it will released before setting a
 *      new string. That is why this command should start with trying to get first
 *      metadata for given key.
 *
 * 3) KEYMETA.GET <key> <metadata-class-id>
 *    Get the metadata attached to the key for the given class.
 *    Returns a string attached to the given key. Or nil if nothing is attached.
 *
 * 4) KEYMETA.UNREGISTER <keymeta-class-id>
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

/* Virtualize class IDs for testing. Values: 0 unused, 1..7 used, -1 released */
RedisModuleKeyMetaClassId class_ids[8] = { 0 };

/* Track active metadata instances (not yet freed) */
static long long active_metadata_count = 0;

/* Callback functions for metadata lifecycle */

/* Copy callback - called when a key is copied */
static int KeyMetaCopyCallback(RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    REDISMODULE_NOT_USED(ctx);
    char *str = (char *)*meta;
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
    if (*meta != 0) {
        char *str = (char *)*meta;
        free(str);
        *meta = 0;
        active_metadata_count--; /* Metadata instance freed */
    }
    REDISMODULE_NOT_USED(ctx);
}

/* Free callback - called when metadata needs to be freed */
static void KeyMetaFreeCallback(const char *keyname, uint64_t meta) {
    REDISMODULE_NOT_USED(keyname);
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

/* KEYMETA.REGISTER <9-byte-id> <version> [KEEPONCOPY:KEEPONRENAME:UNLINKFREE] */
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
    if (argc == 4) {
        const char *flags = RedisModule_StringPtrLen(argv[3], NULL);
        if (strstr(flags, "KEEPONCOPY")) keep_on_copy = 1;
        if (strstr(flags, "KEEPONRENAME")) keep_on_rename = 1;
        if (strstr(flags, "UNLINKFREE")) unlink_free = 1;
        if (strstr(flags, "KEEPONMOVE")) keep_on_move = 1;
    }

    /* Setup configuration */
    RedisModuleKeyMetaClassConfig config = {0};
    config.version = REDISMODULE_KEY_META_VERSION;
    config.flags = REDISMODULE_META_ALLOW_IGNORE;
    config.reset_value = (uint64_t)NULL;  /* NULL pointer means no resource to free */
    config.rdb_load = NULL;
    config.rdb_save = NULL;
    config.aof_rewrite = NULL;
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
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithLongLong(ctx, class_id);
    }

    return REDISMODULE_OK;
}

/* KEYMETA.SET <key> <metadata-class-id> <string-value> */
static int KeyMetaSet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    RedisModuleString *keyname = argv[1];
    long long class_id_ll;
    if (RedisModule_StringToLongLong(argv[2], &class_id_ll) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid class id");
        return REDISMODULE_OK;
    }
    RedisModuleKeyMetaClassId class_id = (RedisModuleKeyMetaClassId)class_id_ll;

    const char *value = RedisModule_StringPtrLen(argv[3], NULL);

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

/* KEYMETA.GET <key> <metadata-class-id> */
static int KeyMetaGet_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    RedisModuleString *keyname = argv[1];
    long long class_id_ll;
    if (RedisModule_StringToLongLong(argv[2], &class_id_ll) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid class id");
        return REDISMODULE_OK;
    }
    RedisModuleKeyMetaClassId class_id = (RedisModuleKeyMetaClassId)class_id_ll;

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

/* KEYMETA.UNREGISTER <keymeta-class-id> */
static int KeyMetaUnregister_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    /* Parse arguments */
    long long class_id_ll;
    if (RedisModule_StringToLongLong(argv[1], &class_id_ll) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid class id");
        return REDISMODULE_OK;
    }
    RedisModuleKeyMetaClassId class_id = (RedisModuleKeyMetaClassId)class_id_ll;

    /* Release the metadata class */
    int result = RedisModule_ReleaseKeyMetaClass(class_id);

    if (result == REDISMODULE_OK) {
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
