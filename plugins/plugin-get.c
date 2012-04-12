#include "redis.h"

extern void getCommand(redisClient *c);

struct redisCommand commands[] = {
  {"samget",getCommand,2,"r",0,NULL,1,1,1,0,0}
};

struct redisCommand* registerCommands(int *numcommands) {
  *numcommands = sizeof(commands)/sizeof(struct redisCommand);
  return commands;
}
