/* Hellonet module -- An example of blocking command implementation using
 * networking api
 */

#define REDISMODULE_EXPERIMENTAL_API
#include "../redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

const char *CRLF = "\r\n";

/* contextual data for hellonet module */
typedef struct HelloNetContext HelloNetContext;
/* free context of hellonet module */
static void HelloNetContext_Free(RedisModuleCtx *ctx, void *arg);

/* for long lived resources bookkeeping */
typedef struct HelloNetResource HelloNetResource;
/* close a given resource */
typedef void (*HelloNetResourceCloser)(RedisModuleCtx *ctx, HelloNetResource *resource);
/* free all resources chained together with given closer */
static void HelloNetResource_FreeAll(RedisModuleCtx *ctx, HelloNetResource *res, HelloNetResourceCloser closer);
/* free a certain resource with given id */
static void HelloNetResource_Free(RedisModuleCtx *ctx, HelloNetResource **pres, HelloNetResourceCloser closer, long long id);
/* closer for timer */
static void HelloNetTimer_Closer(RedisModuleCtx *ctx, HelloNetResource *resource);
/* closer for file event */
static void HelloNetFd_EventCloser(RedisModuleCtx *ctx, HelloNetResource *resource);
/* closer for open file with events monitored */
static void HelloNetFd_Closer(RedisModuleCtx *ctx, HelloNetResource *resource);
/* create a new resource and chain it to all resources */
static void HelloNetResource_New(HelloNetResource **pres, long long id);
/* create a new timer resource */
static void HelloNetTimer_New(RedisModuleCtx *ctx, long long timer);
/* free a timer resource */
static void HelloNetTimer_Free(RedisModuleCtx *ctx, long long timer);
/* create a new file resource */
static void HelloNetFd_New(RedisModuleCtx *ctx, int fd);
/* free a file resource */
static void HelloNetFd_Free(RedisModuleCtx *ctx, int fd);
/* free a closed file resource */
static void HelloNetFd_FreeClosed(RedisModuleCtx *ctx, int fd);

/* buffer structure to help reading from and writing to file descriptor */
typedef struct HelloNetBuffer HelloNetBuffer;
/* buffer flags */
enum {
  HELLONET_BUFFER_FLAG_WRAPPED = 1,
};
/* read data from fd and transfer it to buffer */
static int HelloNetBuffer_TransferTo(HelloNetBuffer *buffer, int fd);
/* read data from buffer and write it to fd */
static int HelloNetBuffer_TransferFrom(HelloNetBuffer *buffer, int fd);
/* initialize an empty buffer */
static void HelloNetBuffer_Init(HelloNetBuffer *buffer);
/* wrap existing data to buffer for reading */
static void HelloNetBuffer_Wrap(HelloNetBuffer *buffer, const char *init);
/* rewind buffer for next reading */
static void HelloNetBuffer_Rewind(HelloNetBuffer *buffer);
/* reset buffer for next writing */
static void HelloNetBuffer_Reset(HelloNetBuffer *buffer);
/* free buffer's internal data */
static void HelloNetBuffer_Free(HelloNetBuffer *buffer);

/* data required to serve keys request */
typedef struct HelloNetKeysRequest HelloNetKeysRequest;
/* free keys request data */
static void HelloNetKeysRequest_Free(HelloNetKeysRequest *request);

/* connector type for connecting to upstream redis server */
typedef void (*HelloNetKeys_Connector)(RedisModuleCtx *ctx, HelloNetKeysRequest *request);
/* tcp upstream redis connector */
static void HelloNetKeys_TcpConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request);
/* unix upstream redis connector */
static void HelloNetKeys_UnixConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request);
/* direct client upstream redis connector */
static void HelloNetKeys_ClientConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request);

/* buffered info response data from redis */
typedef struct HelloNetInfoResponse HelloNetInfoResponse;

/* create server with given setup */
typedef int (*HelloNetInfo_Server)(RedisModuleCtx *ctx, const char *address, int port, int backlog);
/* create a tcp server on bind address and port number with given backlog */
static int HelloNetInfo_TcpServer(RedisModuleCtx *ctx, const char *address, int port, int backlog);
/* create an ipv6 tcp server on bind address and port number with given backlog */
static int HelloNetInfo_Tcp6Server(RedisModuleCtx *ctx, const char *address, int port, int backlog);
/* create an unix server on path with given permission and backlog */
static int HelloNetInfo_UnixServer(RedisModuleCtx *ctx, const char *path, int perm, int backlog);
/* acceptor for new client */
typedef int (*HelloNetInfo_Acceptor)(RedisModuleCtx *ctx, int fd);
/* accept tcp client */
static int HelloNetInfo_TcpAcceptor(RedisModuleCtx *ctx, int fd);
/* accept unix client */
static int HelloNetInfo_UnixAcceptor(RedisModuleCtx *ctx, int fd);

/* module context */
struct HelloNetContext {
  HelloNetResource *fd;
  HelloNetResource *timer;
};

static void HelloNetContext_Free(RedisModuleCtx *ctx, void *arg) {
  HelloNetContext *net = arg;
  HelloNetResource_FreeAll(ctx, net->fd, HelloNetFd_Closer);
  HelloNetResource_FreeAll(ctx, net->timer, HelloNetTimer_Closer);
  RedisModule_Free(net);
}

/* resource tracking logic */
struct HelloNetResource {
  long long id;
  HelloNetResource *next;
};

static void HelloNetResource_FreeAll(RedisModuleCtx *ctx, HelloNetResource *res, HelloNetResourceCloser closer) {
  HelloNetResource *next;
  while (res != NULL) {
    next = res->next;
    closer(ctx, res);
    RedisModule_Free(res);
    res = next;
  }
}

static void HelloNetResource_Free(RedisModuleCtx *ctx, HelloNetResource **pres, HelloNetResourceCloser closer, long long id) {
  HelloNetResource *res = *pres;
  while (res != NULL) {
    if (res->id == id) {
      *pres = res->next;
      break;
    } else {
      pres = &res->next;
    }
  }
  if (res != NULL) {
    closer(ctx, res);
    RedisModule_Free(res);
  }
}

static void HelloNetTimer_Closer(RedisModuleCtx *ctx, HelloNetResource *resource) {
  RedisModule_DeleteTimeEvent(ctx, resource->id);
}

static void HelloNetFd_EventCloser(RedisModuleCtx *ctx, HelloNetResource *resource) {
  RedisModule_DeleteFileEvent(ctx, resource->id, REDISMODULE_FILE_READABLE | REDISMODULE_FILE_WRITABLE);
}

static void HelloNetFd_Closer(RedisModuleCtx *ctx, HelloNetResource *resource) {
  HelloNetFd_EventCloser(ctx, resource);
  close(resource->id);
}

static void HelloNetResource_New(HelloNetResource **pres, long long id) {
  HelloNetResource *res = RedisModule_Alloc(sizeof(*res));
  res->id = id;
  res->next = *pres;
  *pres = res;
}

static void HelloNetTimer_New(RedisModuleCtx *ctx, long long timer) {
  HelloNetContext *net = RedisModule_GetAttachment(ctx, NULL, 0);
  HelloNetResource_New(&net->timer, timer);
}
static void HelloNetTimer_Free(RedisModuleCtx *ctx, long long timer) {
  HelloNetContext *net = RedisModule_GetAttachment(ctx, NULL, 0);
  HelloNetResource_Free(ctx, &net->timer, HelloNetTimer_Closer, timer);
}

static void HelloNetFd_New(RedisModuleCtx *ctx, int fd) {
  HelloNetContext *net = RedisModule_GetAttachment(ctx, NULL, 0);
  HelloNetResource_New(&net->fd, fd);
}

static void HelloNetFd_Free(RedisModuleCtx *ctx, int fd) {
  HelloNetContext *net = RedisModule_GetAttachment(ctx, NULL, 0);
  HelloNetResource_Free(ctx, &net->fd, HelloNetFd_Closer, fd);
}

static void HelloNetFd_FreeClosed(RedisModuleCtx *ctx, int fd) {
  HelloNetContext *net = RedisModule_GetAttachment(ctx, NULL, 0);
  HelloNetResource_Free(ctx, &net->fd, HelloNetFd_EventCloser, fd);
}

/* buffer logic to transfer from and to file descriptors */
struct HelloNetBuffer {
  int flags;
  int rd;
  int wr;
  int capacity;
  char *buffer;
};

static int HelloNetBuffer_TransferTo(HelloNetBuffer *buffer, int fd) {
  int available, rd;
  while (1) {
    if ((available = buffer->capacity - buffer->wr) < 128) {
      buffer->capacity += 4096;
      /* leave one byte for \0 terminator */
      buffer->buffer = RedisModule_Realloc(buffer->buffer,buffer->capacity+1);
      available += 4096;
    }
    if ((rd = read(fd,buffer->buffer+buffer->wr,available)) > 0) {
      buffer->wr += rd;
    } else {
      break;
    }
  }
  buffer->buffer[buffer->wr] = '\0';
  return rd < 0 && errno != EINTR && errno != EAGAIN ? REDISMODULE_ERR : REDISMODULE_OK;
}

static int HelloNetBuffer_TransferFrom(HelloNetBuffer *buffer, int fd) {
  int wr = write(fd,buffer->buffer+buffer->rd,buffer->wr-buffer->rd);
  if (wr > 0) {
    buffer->rd += wr;
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    return REDISMODULE_ERR;
  }
  return REDISMODULE_OK;
}

static void HelloNetBuffer_Init(HelloNetBuffer *buffer) {
  memset(buffer, 0, sizeof(*buffer));
}

static void HelloNetBuffer_Wrap(HelloNetBuffer *buffer, const char *init) {
  buffer->rd = 0;
  buffer->capacity = buffer->wr = strlen(init);
  buffer->buffer = (char *) init;
  buffer->flags = HELLONET_BUFFER_FLAG_WRAPPED;
}

static void HelloNetBuffer_Rewind(HelloNetBuffer *buffer) {
  buffer->rd = 0;
}

static void HelloNetBuffer_Reset(HelloNetBuffer *buffer) {
  if ((buffer->flags & HELLONET_BUFFER_FLAG_WRAPPED) != 0) {
    HelloNetBuffer_Init(buffer);
  } else {
    buffer->rd = buffer->wr = 0;
  }
}

static void HelloNetBuffer_Free(HelloNetBuffer *buffer) {
  if ((buffer->flags & HELLONET_BUFFER_FLAG_WRAPPED) == 0) {
    RedisModule_Free(buffer->buffer);
  }
  HelloNetBuffer_Init(buffer);
}

/* bookkeeping data for hellonet.tcp/unix/client request */
struct HelloNetKeysRequest {
  HelloNetKeys_Connector connector;
  const char *error;
  char *address;
  char client[32];
  size_t length;
  int left;
  HelloNetBuffer buffer;
};

static void HelloNetKeysRequest_Free(HelloNetKeysRequest *request) {
  RedisModule_Free(request->address);
  HelloNetBuffer_Free(&request->buffer);
  RedisModule_Free(request);
}

static int HelloNetKeys_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  HelloNetKeysRequest *request = RedisModule_GetBlockedClientPrivateData(ctx);
  HelloNetBuffer *buffer = &request->buffer;
  const char *crlf;
  int length;
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  if (request->error) {
    RedisModule_ReplyWithError(ctx,request->error);
  } else {
    HelloNetBuffer_Rewind(buffer);
    while ((crlf = strstr(buffer->buffer+buffer->rd,CRLF)) != NULL) {
      const char c = buffer->buffer[buffer->rd];
      if (c == '*') {
        /* array size */
        RedisModule_ReplyWithArray(ctx,strtol(buffer->buffer+buffer->rd+1,NULL,10));
      } else if (c == '$') {
        /* array element */
        length = strtol(buffer->buffer+buffer->rd+1,NULL,10);
        RedisModule_ReplyWithStringBuffer(ctx,crlf+2,length);
        crlf = strstr(crlf+2,CRLF);
      }
      buffer->rd = (crlf - buffer->buffer) + 2;
    }
  }
  return REDISMODULE_OK;
}

static int HelloNetKeys_Timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  char fmtbuff[32];
  size_t len = snprintf(fmtbuff,sizeof(fmtbuff),"%llu",RedisModule_GetClientId(ctx));
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  RedisModule_Detach(ctx,fmtbuff,len);
  return RedisModule_ReplyWithError(ctx,"ERR Request timedout");
}

static void HelloNetKeys_FreeRequest(void *privdata) {
  HelloNetKeysRequest_Free(privdata);
}

static void HelloNetKeys_Respond(RedisModuleCtx *ctx, HelloNetKeysRequest *request) {
  RedisModuleBlockedClient *blocked = RedisModule_GetAttachment(ctx,request->client,request->length);
  if (blocked != NULL) {
    RedisModule_Detach(ctx,request->client,request->length);
    RedisModule_UnblockClient(blocked,request);
  } else {
    HelloNetKeysRequest_Free(request);
  }
}

static void HelloNetKeys_Error(RedisModuleCtx *ctx, HelloNetKeysRequest *request, const char *error) {
  request->error = error;
  HelloNetKeys_Respond(ctx, request);
}

static void HelloNetKeys_ParseResponse(RedisModuleCtx *ctx, int fd, HelloNetKeysRequest *request) {
  const char *crlf;
  HelloNetBuffer *buffer = &request->buffer;
  while ((crlf = strstr(buffer->buffer+buffer->rd,CRLF)) != NULL) {
    const char c = buffer->buffer[buffer->rd];
    if (c == '*') {
      /* array size */
      request->left = strtol(buffer->buffer+buffer->rd+1,NULL,10);
    } else if (c == '$') {
      /* array element */
      if ((crlf = strstr(crlf+2,CRLF)) == NULL) {
        return;
      }
      request->left--;
    }
    buffer->rd = (crlf - buffer->buffer) + 2;
  }

  if (request->left == 0) {
    if (request->connector == HelloNetKeys_ClientConnector) {
      RedisModule_FreeClient(ctx, fd);
    } else {
      close(fd);
    }
    HelloNetFd_FreeClosed(ctx, fd);
    HelloNetKeys_Respond(ctx, request);
  }
}

static void HelloNetKeys_ReadResponse(RedisModuleCtx *ctx, int fd, void *clientData, int mask) {
  HelloNetKeysRequest *request = clientData;
  HelloNetBuffer *buffer = &request->buffer;
  REDISMODULE_NOT_USED(mask);
  HelloNetBuffer_Reset(buffer);

  if (HelloNetBuffer_TransferTo(buffer, fd) != REDISMODULE_OK) {
    HelloNetFd_Free(ctx, fd);
    HelloNetKeys_Error(ctx,request,"ERR Could not read response from upstream");
  } else {
    HelloNetKeys_ParseResponse(ctx, fd, request);
  }
}

static void HelloNetKeys_WriteRequest(RedisModuleCtx *ctx, int fd, void *clientData, int mask) {
  HelloNetKeysRequest *request = clientData;
  HelloNetBuffer *buffer = &request->buffer;
  REDISMODULE_NOT_USED(mask);
  if (HelloNetBuffer_TransferFrom(buffer, fd) != REDISMODULE_OK) {
    HelloNetKeys_Error(ctx,request,"ERR Could not write keys request to upstream");
    HelloNetFd_Free(ctx, fd);
  } else if (buffer->rd == buffer->wr) {
    RedisModule_CreateFileEvent(ctx,fd,REDISMODULE_FILE_READABLE,HelloNetKeys_ReadResponse,request);
    RedisModule_DeleteFileEvent(ctx,fd,REDISMODULE_FILE_WRITABLE);
  }
}

static void HelloNetKeys_TcpConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request) {
  int fd = 0, port = 6379;
  char *c = strchr(request->address, ':');
  if (c != NULL) {
    *c = '\0';
    port = strtol(c + 1, NULL, 10);
  }
  if (RedisModule_TcpNonBlockConnect(ctx, request->address, port, &fd) != REDISMODULE_OK) {
    HelloNetKeys_Error(ctx,request,"ERR could not connect to redis server via tcp socket");
  } else {
    char peerBuffer[256], localBuffer[256];
    int peerPort, localPort;
    if (RedisModule_DisableTcpNoDelay(ctx, fd) != REDISMODULE_OK) {
      RedisModule_Log(ctx, "warning", "Could not disable tcp no delay on fd %d", fd);
    }
    if (RedisModule_EnableTcpNoDelay(ctx, fd) != REDISMODULE_OK) {
      RedisModule_Log(ctx, "warning", "Could not enable tcp no delay on fd %d", fd);
    }
    if (RedisModule_TcpKeepAlive(ctx, fd, 120) != REDISMODULE_OK) {
      RedisModule_Log(ctx, "warning", "Could not enable tcp keepalive on fd %d", fd);
    }
    if (RedisModule_CreateFileEvent(ctx, fd, REDISMODULE_FILE_WRITABLE, HelloNetKeys_WriteRequest, request) == REDISMODULE_OK) {
      RedisModule_PeerName(ctx, fd, peerBuffer, sizeof(peerBuffer), &peerPort);
      RedisModule_SockName(ctx, fd, localBuffer, sizeof(localBuffer), &localPort);
      RedisModule_Log(ctx, "notice", "TCP connection established to socket %s:%d from %s:%d", peerBuffer, peerPort, localBuffer, localPort);
      HelloNetFd_New(ctx, fd);
    } else {
      HelloNetKeys_Error(ctx,request,"ERR could not watch for tcp connect finish event");
    }
  }
}

static void HelloNetKeys_UnixConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request) {
  int fd = 0;
  if (RedisModule_UnixNonBlockConnect(ctx, request->address, &fd) != REDISMODULE_OK) {
    HelloNetKeys_Error(ctx,request,"ERR could not connect to redis server via unix socket");
  } else {
    if (RedisModule_CreateFileEvent(ctx, fd, REDISMODULE_FILE_WRITABLE, HelloNetKeys_WriteRequest, request) == REDISMODULE_OK) {
      RedisModule_Log(ctx, "notice", "Unix connection established to %s", request->address);
      HelloNetFd_New(ctx, fd);
    } else {
      HelloNetKeys_Error(ctx,request,"ERR could not watch for unix connect finish event");
    }
  }
}

static void HelloNetKeys_ClientConnector(RedisModuleCtx *ctx, HelloNetKeysRequest *request) {
  int fd = 0;
  if (RedisModule_CreateClient(ctx, &fd) != REDISMODULE_OK) {
    HelloNetKeys_Error(ctx,request,"ERR could not create client to redis server");
  } else {
    RedisModule_EnableNonBlock(ctx, fd);
    if (RedisModule_CreateFileEvent(ctx, fd, REDISMODULE_FILE_WRITABLE, HelloNetKeys_WriteRequest, request) == REDISMODULE_OK) {
      HelloNetFd_New(ctx, fd);
    } else {
      HelloNetKeys_Error(ctx,request,"ERR could not watch client fd");
    }
  }
}

static int HelloNetKeys_Delayed(RedisModuleCtx *ctx, long long id, void *clientData) {
  HelloNetKeysRequest *request = clientData;
  request->connector(ctx, request);
  HelloNetTimer_Free(ctx, id);
  return REDISMODULE_TIME_NOMORE;
}

/* hellonet.tcp/unix/client <address> <delay> <timeout> -- Block for <count> seconds,
 * then reply with all keys. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout. */
static int HelloNetKeys_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, HelloNetKeys_Connector connector) {
  size_t len;
  long long delay, timeout, timer;
  const char *address, *err = NULL;
  RedisModuleBlockedClient *bc = NULL;
  HelloNetKeysRequest *request = NULL;
  if (argc != 4) return RedisModule_WrongArity(ctx);
  if (RedisModule_StringToLongLong(argv[2],&delay) != REDISMODULE_OK) {
    err = "ERR invalid delay";
    goto error;
  }

  if (RedisModule_StringToLongLong(argv[3],&timeout) != REDISMODULE_OK) {
    err = "ERR invalid timeout";
    goto error;
  }
  address = RedisModule_StringPtrLen(argv[1], &len);
  request = RedisModule_Calloc(1, sizeof(*request));
  request->connector = connector;
  request->address = RedisModule_Alloc(len + 1);
  memcpy(request->address, address, len + 1);
  request->length = snprintf(request->client, sizeof(request->client), "%llu", RedisModule_GetClientId(ctx));;
  HelloNetBuffer_Wrap(&request->buffer, "*2\r\n$4\r\nkeys\r\n$1\r\n*\r\n");
  RedisModule_Attach(ctx, request->client, request->length,
      RedisModule_BlockClient(ctx,HelloNetKeys_Reply,HelloNetKeys_Timeout,HelloNetKeys_FreeRequest,timeout), NULL);
  if (RedisModule_CreateTimeEvent(ctx, delay, HelloNetKeys_Delayed, request, NULL, &timer) == REDISMODULE_OK) {
    HelloNetTimer_New(ctx, timer);
    return REDISMODULE_OK;
  }
  err = "ERR could not create delay timer";
error:
  if (bc != NULL) {
    RedisModule_AbortBlock(bc);
  }
  if (request != NULL) {
    HelloNetKeysRequest_Free(request);
  }
  return RedisModule_ReplyWithError(ctx,err);
}

/* hellonet.tcpserver/tcp6server/unixserver response */
struct HelloNetInfoResponse {
  HelloNetBuffer buffer;
  RedisModuleCallReply *reply;
};

static int HelloNetTcp_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetKeys_RedisCommand(ctx, argv, argc, HelloNetKeys_TcpConnector);
}

static int HelloNetUnix_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetKeys_RedisCommand(ctx, argv, argc, HelloNetKeys_UnixConnector);
}

static int HelloNetClient_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetKeys_RedisCommand(ctx, argv, argc, HelloNetKeys_ClientConnector);
}

static void HelloNetInfo_Echo(RedisModuleCtx *ctx, int fd, void *clientData, int mask) {
  char address[256];
  HelloNetInfoResponse *response = clientData;
  HelloNetBuffer *buffer = &response->buffer;
  int port;
  REDISMODULE_NOT_USED(mask);
  if (HelloNetBuffer_TransferFrom(buffer, fd) != REDISMODULE_OK || buffer->rd == buffer->wr) {
    goto cleanup;
  }
  return;
cleanup:
  REDISMODULE_NOT_USED(mask);
  RedisModule_PeerName(ctx, fd, address, sizeof(address), &port);
  HelloNetFd_Free(ctx, fd);
  RedisModule_Log(ctx, "notice", "Closed connection from %s:%d", address, port);
  RedisModule_FreeCallReply(response->reply);
  HelloNetBuffer_Free(&response->buffer);
  RedisModule_Free(response);
}

static int HelloNetInfo_TcpAcceptor(RedisModuleCtx *ctx, int fd) {
  int client, port;
  char ip[128];
  if (RedisModule_TcpAccept(ctx, fd, ip, sizeof(ip), &port, &client) != REDISMODULE_OK) {
    return 0;
  } else {
    RedisModule_Log(ctx, "notice", "Accepted tcp connection from %s:%d", ip, port);
    return client;
  }
}

static int HelloNetInfo_UnixAcceptor(RedisModuleCtx *ctx, int fd) {
  int client;
  if (RedisModule_UnixAccept(ctx, fd, &client) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "notice", "Accepted unix connection");
    return 0;
  } else {
    return client;
  }
}

static void HelloNetInfo_Accept(RedisModuleCtx *ctx, int fd, void *clientData, int mask) {
  HelloNetInfoResponse *response;
  HelloNetInfo_Acceptor acceptor = clientData;
  int client = acceptor(ctx, fd);
  size_t len;
  REDISMODULE_NOT_USED(mask);
  if (client <= 0) {
    return;
  }
  response = RedisModule_Alloc(sizeof(*response));
  response->reply = RedisModule_Call(ctx, "info", "");
  HelloNetBuffer_Wrap(&response->buffer, RedisModule_CallReplyStringPtr(response->reply, &len));
  if (RedisModule_CreateFileEvent(ctx, client, REDISMODULE_FILE_WRITABLE, HelloNetInfo_Echo, response) == REDISMODULE_OK) {
    HelloNetFd_New(ctx, client);
  } else {
    close(client);
  }
}

static int HelloNetInfo_TcpServer(RedisModuleCtx *ctx, const char *address, int port, int backlog) {
  int fd = 0;
  RedisModule_TcpServer(ctx, port, strlen(address) == 0 ? NULL : address, backlog, &fd);
  return fd;
}

static int HelloNetInfo_Tcp6Server(RedisModuleCtx *ctx, const char *address, int port, int backlog) {
  int fd = 0;
  RedisModule_Tcp6Server(ctx, port, strlen(address) == 0 ? NULL : address, backlog, &fd);
  return fd;
}

static int HelloNetInfo_UnixServer(RedisModuleCtx *ctx, const char *path, int perm, int backlog) {
  int fd = 0;
  RedisModule_UnixServer(ctx, path, perm, backlog, &fd);
  return fd;
}

/* hellonet.tcpserver/tcp6server/unixserver <address> <port> <backlog>
 * create server on given address and echo redis info to connected clients */
static int HelloNetServer_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv,
    int argc, HelloNetInfo_Server server, HelloNetInfo_Acceptor acceptor) {
  size_t len;
  const char *address, *err = NULL;
  long long port, backlog;
  int fd = 0;
  if (argc != 4) return RedisModule_WrongArity(ctx);
  if (RedisModule_StringToLongLong(argv[2],&port) != REDISMODULE_OK) {
    err = "ERR invalid backlog";
    goto error;
  }
  if (RedisModule_StringToLongLong(argv[3],&backlog) != REDISMODULE_OK) {
    err = "ERR invalid backlog";
    goto error;
  }
  address = RedisModule_StringPtrLen(argv[1], &len);
  if ((fd = server(ctx, address, port, backlog)) <= 0) {
    err = "ERR could not create server";
    goto error;
  }
  if (RedisModule_CreateFileEvent(ctx, fd, REDISMODULE_FILE_READABLE, HelloNetInfo_Accept, acceptor) == REDISMODULE_OK) {
    HelloNetFd_New(ctx, fd);
    RedisModule_ReplyWithLongLong(ctx, fd);
    RedisModule_Log(ctx, "notice", "Info server is now ready to accept connections at %s:%lld with backlog of %lld", address, port, backlog);
    return REDISMODULE_OK;
  }
  err = "ERR could not wait for new connection";
error:
  if (fd != 0) {
    close(fd);
  }
  return RedisModule_ReplyWithError(ctx,err);
}

static int HelloNetTcpServer_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetServer_RedisCommand(ctx, argv, argc, HelloNetInfo_TcpServer, HelloNetInfo_TcpAcceptor);
}

static int HelloNetTcp6Server_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetServer_RedisCommand(ctx, argv, argc, HelloNetInfo_Tcp6Server, HelloNetInfo_TcpAcceptor);
}

static int HelloNetUnixServer_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return HelloNetServer_RedisCommand(ctx, argv, argc, HelloNetInfo_UnixServer, HelloNetInfo_UnixAcceptor);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);

  if (RedisModule_Init(ctx,"hellonet",1,REDISMODULE_APIVER_1)
      == REDISMODULE_ERR) return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx,"hellonet.tcp",
        HelloNetTcp_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;
  if (RedisModule_CreateCommand(ctx,"hellonet.unix",
        HelloNetUnix_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;
  if (RedisModule_CreateCommand(ctx,"hellonet.client",
        HelloNetClient_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx,"hellonet.tcpserver",
        HelloNetTcpServer_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;
  if (RedisModule_CreateCommand(ctx,"hellonet.tcp6server",
        HelloNetTcp6Server_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;
  if (RedisModule_CreateCommand(ctx,"hellonet.unixserver",
        HelloNetUnixServer_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  RedisModule_Attach(ctx, NULL, 0, RedisModule_Calloc(1, sizeof(HelloNetContext)), HelloNetContext_Free);

  return REDISMODULE_OK;
}
