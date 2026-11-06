#include <stddef.h>
#include "cli_commands.h"

/* Definitions to configure commands.c to generate the above structs. */
#define MAKE_CMD(name,summary,complexity,since,doc_flags,replaced,deprecated,group,group_enum,history,num_history,tips,num_tips,function,arity,flags,acl,key_specs,key_specs_num,get_keys,numargs) name,summary,group,since,numargs
#define MAKE_ARG(name,type,key_spec_index,token,summary,since,flags,numsubargs,deprecated_since) name,type,token,since,flags,numsubargs
#define COMMAND_ARG cliCommandArg
#define COMMAND_STRUCT commandDocs
#define SKIP_CMD_HISTORY_TABLE
#define SKIP_CMD_TIPS_TABLE
#define SKIP_CMD_KEY_SPECS_TABLE

#include "commands.def"
