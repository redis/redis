
#include <stdio.h>
#include <string.h>

/*
 * List command groups.
 */

#define GROUPS \
  G(UNKNOWN, "unknown") \
  G(SET, "set") \
  G(LIST, "list") \
  G(HASH, "hash") \
  G(GENERIC, "generic") \
  G(PUBSUB, "pubsub") \
  G(STRING, "string") \
  G(SERVER, "server") \
  G(CONNECTION, "connection") \
  G(TRANSACTIONS, "transactions") \
  G(SORTED_SET, "sorted_set")

/*
 * Command group types.
 */

typedef enum {
  #define G(GROUP, _) COMMAND_GROUP_##GROUP,
  GROUPS
  #undef G
  COMMAND_GROUP_LENGTH
} command_group_type_t;

/*
 * Command group type names.
 */

static char *command_group_type_names[] = {
  #define G(_, STR) STR,
  GROUPS
  #undef G
};

/*
 * Command help struct.
 */

struct command_help {
  char *name;
  char *params;
  char *summary;
  command_group_type_t group;
  char *since;
} command_help[] = {
  __COMMANDS__
};

/*
 * Output command help to stdout.
 */

static void
output_command_help(struct command_help *help) {
  printf("\n  \x1b[1m%s\x1b[0m \x1b[90m%s\x1b[0m\n", help->name, help->params);
  printf("  \x1b[33msummary:\x1b[0m %s\n", help->summary);
  printf("  \x1b[33msince:\x1b[0m %s\n", help->since);
  printf("  \x1b[33mgroup:\x1b[0m %s\n", command_group_type_names[help->group]);
}

/*
 * Return command group type by name string.
 */

static command_group_type_t
command_group_type_by_name(const char *name) {
  for (int i = 0; i < COMMAND_GROUP_LENGTH; ++i) {
    const char *group = command_group_type_names[i];
    if (0 == strcasecmp(name, group)) return i;
  }
  return 0;
}

/*
 * Output group names.
 */

static void
output_group_help() {
	for (int i = 0; i < COMMAND_GROUP_LENGTH; ++i) {
		if (COMMAND_GROUP_UNKNOWN == i) continue;
		const char *group = command_group_type_names[i];
		printf("  \x1b[90m-\x1b[0m %s\n", group);
	}
}

/*
 * Output all command help, filtering by group or command name.
 */

static void
output_help(int argc, const char **argv) {
  int len = sizeof(command_help) / sizeof(struct command_help);

	if (argc && 0 == strcasecmp("groups", argv[0])) {
		output_group_help();
		return;
	}

	command_group_type_t group = argc
    ? command_group_type_by_name(argv[0])
    : COMMAND_GROUP_UNKNOWN;

  for (int i = 0; i < len; ++i) {
    struct command_help help = command_help[i];
    if (argc && !group && 0 != strcasecmp(help.name, argv[0])) continue;
    if (group && group != help.group) continue;
    output_command_help(&help);
  }
  puts("");
}