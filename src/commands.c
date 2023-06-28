#include "commands.h"
#include "server.h"

#define MAKE_CMD(name,summary,complexity,since,doc_flags,replaced,deprecated,group,group_enum,history,num_history,tips,num_tips,function,arity,flags,acl,key_specs,key_specs_num,get_keys,numargs) name,summary,complexity,since,doc_flags,replaced,deprecated,group_enum,history,num_history,tips,num_tips,function,arity,flags,acl,key_specs,key_specs_num,get_keys,numargs
#define MAKE_ARG(name,type,key_spec_index,token,summary,since,flags,numsubargs,deprecated_since) name,type,key_spec_index,token,summary,since,flags,deprecated_since,numsubargs
#define COMMAND_STRUCT redisCommand
#define COMMAND_ARG redisCommandArg

#ifdef LOG_REQ_RES
#include "commands_with_reply_schema.def"
#else
#include "commands.def"
#endif
