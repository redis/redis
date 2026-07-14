/* Module used to verify that symbols from Redis' vendored CRoaring archive do
 * not interpose on a module that defines the same symbol. */

#include "redismodule.h"

typedef struct roaring64_bitmap_s roaring64_bitmap_t;

static unsigned char module_bitmap_sentinel;
static int module_bitmap_free_count;

/* These are intentionally named after CRoaring API symbols linked into Redis.
 * The volatile function pointers below keep the module references eligible
 * for ELF symbol interposition, matching a module that embeds CRoaring. */
roaring64_bitmap_t *roaring64_bitmap_create(void) {
    return (roaring64_bitmap_t *)&module_bitmap_sentinel;
}

void roaring64_bitmap_free(roaring64_bitmap_t *bitmap) {
    if (bitmap == (roaring64_bitmap_t *)&module_bitmap_sentinel)
        module_bitmap_free_count++;
}

static roaring64_bitmap_t *(*volatile module_bitmap_create)(void) =
    roaring64_bitmap_create;
static void (*volatile module_bitmap_free)(roaring64_bitmap_t *) =
    roaring64_bitmap_free;

static int CroaringCollisionResolvesLocally(RedisModuleCtx *ctx,
                                            RedisModuleString **argv,
                                            int argc) {
    int free_count_before = module_bitmap_free_count;
    roaring64_bitmap_t *bitmap = module_bitmap_create();
    int resolves_locally;

    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    resolves_locally =
        bitmap == (roaring64_bitmap_t *)&module_bitmap_sentinel;
    module_bitmap_free(bitmap);
    resolves_locally = resolves_locally &&
        module_bitmap_free_count == free_count_before + 1;
    return RedisModule_ReplyWithLongLong(ctx, resolves_locally);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "croaring_collision", 1,
                         REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,
            "croaring_collision.resolves-locally",
            CroaringCollisionResolvesLocally, "", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}
