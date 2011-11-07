#include <stdio.h>
#include "redis.h"
#include "redis_jni_Redis.h"

redisClient *jniClient = 0;

JNIEXPORT void JNICALL Java_redis_jni_Redis_start(JNIEnv *env, jclass class, jstring file) {
  jboolean isCopy;
  char **argv = zmalloc(sizeof(*argv)*2);

  // Start redis
  argv[0] = "";
  argv[1] = (*env)->GetStringUTFChars(env, file, &isCopy);
  printf("Config: %s\n", argv[1]);
  main(2, argv);
}

JNIEXPORT void JNICALL Java_redis_jni_Redis_init(JNIEnv *env, jclass class) {
  if (!jniClient) {
    // Pretend to be the in-proc LUA client
    jniClient = createClient(-1);
    jniClient->flags |= REDIS_LUA_CLIENT;
    selectDb(jniClient, 0);
    printf("Redis client initialized\n");
  }
}

JNIEXPORT void JNICALL Java_redis_jni_Redis_command(JNIEnv *env, jobject obj, jobjectArray paramArray) {
    int j, argc;
    struct redisCommand *cmd;
    robj **argv;
    jobject *params;
    jbyte **bytes;
    redisClient *c = jniClient;
    sds reply;
    jboolean isCopy;
    int len;

    argc = (*env)->GetArrayLength(env, paramArray);

printf("arguments: %d\n", argc);
fflush(0);

     /* Build the arguments vector */
    argv = zmalloc(sizeof(robj*)*argc);
    params = zmalloc(sizeof(robj*)*argc);
    bytes = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        params[j] = (*env)->GetObjectArrayElement(env, paramArray, j);
        bytes[j] = (*env)->GetByteArrayElements(env, params[j], &isCopy);
        argv[j] = createStringObject(bytes[j], (*env)->GetArrayLength(env, params[j]));
    }

printf("arguments created, command: %s\n", argv[0]->ptr);
fflush(0);

    /* Setup our fake client for command execution */
    c->argv = argv;
    c->argc = argc;

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);

    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        if (cmd)
            printf("Wrong number of args calling Redis command\n");
        else
            printf("Unknown Redis command called\n");
        goto cleanup;
    }

printf("Command lookup successful: %s\n", cmd->name);
fflush(0);

//    if (cmd->flags & REDIS_CMD_NOSCRIPT) {
//        luaPushError(lua, "This Redis command is not allowed from scripts");
//        goto cleanup;
//    }
//
//    if (cmd->flags & REDIS_CMD_WRITE && server.lua_random_dirty) {
//        luaPushError(lua,
//            "Write commands not allowed after non deterministic commands");
//        goto cleanup;
//    }
//
//    if (cmd->flags & REDIS_CMD_RANDOM) server.lua_random_dirty = 1;

printf("calling: %d %d\n", cmd, cmd->proc);
fflush(0);
    /* Run the command */
    cmd->proc(c);

printf("called\n");
fflush(0);

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    reply = sdsempty();
//    if (c->bufpos) {
//        reply = sdscatlen(reply,c->buf,c->bufpos);
//        c->bufpos = 0;
//    }
//    while(listLength(c->reply)) {
//        robj *o = listNodeValue(listFirst(c->reply));
//
//        reply = sdscatlen(reply,o->ptr,sdslen(o->ptr));
//        listDelNode(c->reply,listFirst(c->reply));
//    }
//    if (raise_error && reply[0] != '-') raise_error = 0;
//    redisProtocolToLuaType(lua,reply);
//    sdsfree(reply);

cleanup:
    fflush(0);
    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    for (j = 0; j < c->argc; j++) {
        decrRefCount(c->argv[j]);
        (*env)->ReleaseByteArrayElements(env, params[j], bytes[j], 0);
    }
    zfree(c->argv);
    zfree(params);
    zfree(bytes);

//    if (raise_error) {
//        /* If we are here we should have an error in the stack, in the
//         * form of a table with an "err" field. Extract the string to
//         * return the plain error. */
//        lua_pushstring(lua,"err");
//        lua_gettable(lua,-2);
//        return lua_error(lua);
//    }
//    return 1;
}