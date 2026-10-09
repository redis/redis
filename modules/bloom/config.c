/* Included by bf.c. Copyright Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
static long long bloomCapacity = 100;
static long long bloomExpansion = 2;
static double bloomErrorRate = 0.01;
static RedisModuleString *bloomErrorString;

static long long getIntegerConfig(const char *name, void *data) {
    REDISMODULE_NOT_USED(name);
    return *(long long *)data;
}

static int setIntegerConfig(const char *name, long long value, void *data,
                            RedisModuleString **error) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(error);
    *(long long *)data = value;
    return REDISMODULE_OK;
}

static RedisModuleString *getErrorConfig(const char *name, void *data) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(data);
    return bloomErrorString;
}

static int setErrorConfig(const char *name, RedisModuleString *value, void *data,
                          RedisModuleString **error) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(data);
    double rate;
    if (RedisModule_StringToDouble(value, &rate) != REDISMODULE_OK ||
        !isfinite(rate) || rate <= 0 || rate >= 1) {
        const char *message = "error rate must be between 0 and 1";
        *error = RedisModule_CreateString(NULL, message, strlen(message));
        return REDISMODULE_ERR;
    }
    if (rate > 0.25) rate = 0.25;
    RedisModuleString *replacement = RedisModule_CreateStringFromDouble(NULL, rate);
    if (bloomErrorString) RedisModule_FreeString(NULL, bloomErrorString);
    bloomErrorString = replacement;
    bloomErrorRate = rate;
    return REDISMODULE_OK;
}

static int registerBloomConfigs(RedisModuleCtx *ctx) {
    bloomErrorString = RedisModule_CreateStringFromDouble(NULL, bloomErrorRate);
    if (RedisModule_RegisterNumericConfig(ctx, "bf-initial-size", 100,
            REDISMODULE_CONFIG_UNPREFIXED, 1, 1LL << 30, getIntegerConfig,
            setIntegerConfig, NULL, &bloomCapacity) != REDISMODULE_OK ||
        RedisModule_RegisterNumericConfig(ctx, "bf-expansion-factor", 2,
            REDISMODULE_CONFIG_UNPREFIXED, 0, 32768, getIntegerConfig,
            setIntegerConfig, NULL, &bloomExpansion) != REDISMODULE_OK ||
        RedisModule_RegisterStringConfig(ctx, "bf-error-rate", "0.01",
            REDISMODULE_CONFIG_UNPREFIXED, getErrorConfig, setErrorConfig,
            NULL, NULL) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    return RedisModule_LoadConfigs(ctx);
}
