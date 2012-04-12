#include "redis.h"

extern void getCommand(redisClient *c);

void hello(redisClient*);

struct redisCommand commands[] = {
  {"hello",hello,1,"r",0,NULL,1,1,1,0,0}
};

struct redisCommand* registerCommands(int *numcommands) {
  *numcommands = sizeof(commands)/sizeof(struct redisCommand);
  return commands;
}

void hello(redisClient *c) {
  addReplyBulkCString(c, "world!");
}
