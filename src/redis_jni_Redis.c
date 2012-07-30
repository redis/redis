#include <stdio.h>
#include "sds.h"
#include "redis.h"
#include "redis_jni_Redis.h"

redisClient *jniClient = 0;

void beforeSleep(struct aeEventLoop *eventLoop);

JNIEXPORT void JNICALL Java_redis_jni_Redis_start(JNIEnv *env, jclass class, jstring file) {
  jboolean isCopy;
  int argc = 2;
  char **argv = zmalloc(sizeof(*argv)*2);
    long long start;

  if (!jniClient) {
  // Start redis
  argv[0] = "";
  argv[1] = (*env)->GetStringUTFChars(env, file, &isCopy);
  printf("Config: %s\n", argv[1]);

  zmalloc_enable_thread_safeness();
  initServerConfig();
  resetServerSaveParams();
  loadServerConfig(argv[1]);
  initServer();
  redisLog(REDIS_NOTICE,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
  start = ustime();
  if (server.appendonly) {
    if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
      redisLog(REDIS_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
      if (rdbLoad(server.dbfilename) == REDIS_OK) {
        redisLog(REDIS_NOTICE,"DB loaded from disk: %.3f seconds", (float)(ustime()-start)/1000000);
      } else if (errno != ENOENT) {
         redisLog(REDIS_WARNING,"Fatal error loading the DB. Exiting.");
         exit(1);
      }
    }
    if (server.ipfd > 0)
      redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    if (server.sofd > 0)
      redisLog(REDIS_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
    aeSetBeforeSleepProc(server.el,beforeSleep);
    // Pretend to be the in-proc LUA client
    jniClient = createClient(-1);
    jniClient->flags |= REDIS_LUA_CLIENT;
    selectDb(jniClient, 0);
    redisLog(REDIS_NOTICE, "Redis client initialized\n");
  }
}

JNIEXPORT void JNICALL Java_redis_jni_Redis_eventloop(JNIEnv *env, jclass class) {
    redisLog(REDIS_NOTICE, "Starting Redis eventloop\n");
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
}

JNIEXPORT jbyteArray JNICALL Java_redis_jni_Redis_command(JNIEnv *env, jclass class, jobjectArray paramArray) {
    int j, argc;
    struct redisCommand *cmd;
    robj **argv;
    jobject *params;
    jbyte **bytes;
    redisClient *c = jniClient;
    jboolean isCopy;
    sds reply;
    int len = 0;
    int replylen = 0;
    jbyteArray result;

    argc = (*env)->GetArrayLength(env, paramArray);

     /* Build the arguments vector */
    argv = zmalloc(sizeof(robj*)*argc);
    params = zmalloc(sizeof(robj*)*argc);
    bytes = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        params[j] = (*env)->GetObjectArrayElement(env, paramArray, j);
        bytes[j] = (*env)->GetByteArrayElements(env, params[j], &isCopy);
        argv[j] = createStringObject(bytes[j], (*env)->GetArrayLength(env, params[j]));
    }

    /* Setup our fake client for command execution */
    c->argv = argv;
    c->argc = argc;

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);

    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        if (cmd)
            redisLog(REDIS_ERR, "Wrong number of args calling Redis command\n");
        else
            redisLog(REDIS_ERR, "Unknown Redis command called\n");
        goto cleanup;
    }

    if (cmd->flags & REDIS_CMD_NOSCRIPT) {
        redisLog(REDIS_ERR, "This Redis command is not allowed from scripts");
        goto cleanup;
    }

    if (cmd->flags & REDIS_CMD_WRITE && server.lua_random_dirty) {
        redisLog(REDIS_ERR, "Write commands not allowed after non deterministic commands\n");
        goto cleanup;
    }

    if (cmd->flags & REDIS_CMD_RANDOM) server.lua_random_dirty = 1;

    /* Run the command */
    cmd->proc(c);

    reply = sdsempty();
    if (c->bufpos) {
        reply = sdscatlen(reply,c->buf,c->bufpos);
        len += c->bufpos;
        c->bufpos = 0;
    }
    while(listLength(c->reply)) {
        robj *o = listNodeValue(listFirst(c->reply));
        replylen = sdslen(o->ptr);
        reply = sdscatlen(reply, o->ptr, replylen);
        len += replylen;
        listDelNode(c->reply, listFirst(c->reply));
    }

cleanup:
    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    for (j = 0; j < c->argc; j++) {
        decrRefCount(c->argv[j]);
        (*env)->ReleaseByteArrayElements(env, params[j], bytes[j], 0);
    }
    zfree(c->argv);
    zfree(params);
    zfree(bytes);

    if (reply) {
      result = (*env)->NewByteArray(env, len);
      (*env)->SetByteArrayRegion(env, result, 0, len, reply);
      return result;
    } else {
      return (*env)->NewByteArray(env, 0);
    }
}