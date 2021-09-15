#!/usr/bin/env python

import os
import glob
import json
import argparse

# Note: This script should be run from the src/ dir: ../utils/generate-command-code.py

GROUPS = [
    "generic",
    "string",
    "list",
    "set",
    "sorted_set",
    "hash",
    "pubsub",
    "transactions",
    "connection",
    "server",
    "scripting",
    "hyperloglog",
    "cluster",
    "geo",
    "stream",
    "bitmap"
]

# Globals

subcommands = {}  # container_name -> dict(subcommand_name -> Subcommand) - Only subcommands
commands = {}  # command_name -> Command - Only commands and container commands
container_commands = {}  # container_command_name -> ContainerCommand - Only container commands


class KeySpec(object):
    def __init__(self, spec):
        self.spec = spec

    def code(self):
        def _flags_code():
            s = ""
            for flag in self.spec.get("flags", []):
                s += "%s " % flag
            return s[:-1]

        def _begin_search_code():
            if self.spec["begin_search"].get("index"):
                return "KSPEC_BS_INDEX,.bs.index={%d}" % (
                    self.spec["begin_search"]["index"]["pos"]
                )
            elif self.spec["begin_search"].get("keyword"):
                return "KSPEC_BS_INDEX,.bs.keyword={\"%s\",%d}" % (
                    self.spec["begin_search"]["keyword"]["keyword"],
                    self.spec["begin_search"]["keyword"]["startfrom"],
                )
            else:
                print("Invalid begin_search! value=%s" % self.spec["begin_search"])
                exit(1)

        def _find_keys_code():
            if self.spec["find_keys"].get("range"):
                return "KSPEC_FK_RANGE,.fk.range={%d,%d,%d}" % (
                    self.spec["find_keys"]["range"]["lastkey"],
                    self.spec["find_keys"]["range"]["step"],
                    self.spec["find_keys"]["range"]["limit"]
                )
            elif self.spec["find_keys"].get("keynum"):
                return "KSPEC_FK_RANGE,.fk.keynum={%d,%d,%d}" % (
                    self.spec["find_keys"]["keynum"]["keynumidx"],
                    self.spec["find_keys"]["keynum"]["firstkey"],
                    self.spec["find_keys"]["keynum"]["step"]
                )
            else:
                print("Invalid find_keys! value=%s" % self.spec["find_keys"])
                exit(1)

        return "\"%s\",%s,%s" % (
            _flags_code(),
            _begin_search_code(),
            _find_keys_code()
        )

"""
name: String. The name of the argument.
description: String. Short description of the argument (optional)
type: String. The type of argument. Possible values:
"string": a string-valued argument
"integer": an integer-valued argument
"double": a floating-point argument
"key": a string-valued argument representing a key in the datastore
"pattern": a string representing a glob-style pattern
"unix_time": integer-valued argument is a Unix timestamp value in seconds
"oneof": multiple options that mutually exclude each other. in this case the field "value" is a list of arguments
"block": not an individual argument, but a block of multiple arguments. in this case, the field "value" is a list of arguments
value: String or List. Either the name to display or a list of dictionaries (each is an argument, so arguments can be nested)
token: String. Name of the preceding token if exists (optional)
optional: Boolean. True if this argument is optional. (optional)
multiple: Boolean. True if this argument can be repeated multiple times. (optional)
since: String. The first version introduced this argument. (optional)"""

class Argument(object):
    def __init__(self, desc):
        self.desc = desc
        self.subargs = []
        self.subargs_name = None
        if self.desc["type"] in ["oneof", "block"]:
            for subdesc in self.desc["value"]:
                self.subargs.append(Argument(subdesc))

    def code(self):
        return "%s,%s,%s,%s,%s,%d,%d,%s,%s" % (
            "%s" % self.desc["name"],
            "%s" %self.desc["description"],
            "%s" %self.desc["type"],
            "%s" % self.desc.get("value"),
        )


class Command(object):
    def __init__(self, name, desc):
        self.name = name.upper()
        self.desc = desc
        self.group = self.desc["group"]

    def fullname(self):
        return self.name

    def return_types_table_name(self, resp):
        return "%s_ReturnTypesRESP%d" % (self.fullname().replace(" ", "_"), resp)

    def return_types_code(self, resp):
        if not self.desc.get("return_types"):
            return ""
        s = ""
        for tupl in self.desc["return_types"][str(resp)]:
            print tupl
            s += "{\"%s\",\"%s\"},\n" % (tupl[0], tupl[1])
        s += "{0}"
        return s

    def history_table_name(self):
        return "%s_History" % (self.fullname().replace(" ", "_"))

    def history_code(self):
        if not self.desc.get("history"):
            return ""
        s = ""
        for tupl in self.desc["history"]:
            print tupl
            s += "{\"%s\",\"%s\"},\n" % (tupl[0], tupl[1])
        s += "{0}"
        return s

    def command_table_code(self):
        """
        "SET","<summary>","1.0.0","string","<return summary>",SET_ReturnTypesRESP2,SET_ReturnTypesRESP3,SET_History,setCommand,-3,"write use-memory @string @write @slow",{{"write",KSPEC_BS_INDEX,.bs.index={1},KSPEC_FK_RANGE,.fk.range={0,1,0}}}
        """
        def _flags_code():
            s = ""
            for flag in self.desc.get("command_flags", []):
                s += "%s " % flag
            for cat in self.desc.get("acl_categories", []):
                s += "@%s " % cat
            return s[:-1]

        def _key_specs_code():
            s = ""
            for spec in self.desc.get("key_specs", []):
                s += "{%s}," % KeySpec(spec).code()
            return s[:-1]

        s = "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",%s,%s,%s,%s,%d,\"%s\"," % (
            self.name,
            self.desc.get("summary", "NA"),
            self.desc.get("since", "NA"),
            self.desc["group"],
            self.desc.get("return_summary", "NA").replace("\n", "\\n"),
            self.return_types_table_name(2),
            self.return_types_table_name(3),
            self.history_table_name(),
            self.desc.get("function", "NULL"),
            self.desc["arity"],
            _flags_code()
        )

        specs = _key_specs_code()
        if specs:
            s += "{%s}," % specs

        if self.desc.get("get_keys_function"):
            s += "%s," % self.desc["get_keys_function"]

        return s[:-1]

    def help_code(self):
        def _arg_syntax_code(arg):
            s = ""
            if arg.get("optional"):
                s += "["
            if not arg.get("type"):
                assert arg.get("token")
                s += arg["token"]
            else:
                if arg.get("token"):
                    s += "%s " % arg["token"]

                if arg["type"] == "oneof":
                    tmp_s = "|".join(_arg_syntax_code(_arg) for _arg in arg["value"])
                    s += "(%s)" % tmp_s
                elif arg["type"] == "block":
                    s += " ".join(_arg_syntax_code(_arg) for _arg in arg["value"])
                else:
                    s += arg["value"]
                    if arg.get("multiple"):
                        tmp_arg = dict(arg)
                        tmp_arg.pop("multiple")
                        tmp_arg.pop("optional", None)
                        tmp_arg.pop("token", None)
                        s += " [%s ...]" % _arg_syntax_code(tmp_arg)
            if arg.get("optional"):
                s += "]"
            return s
                
        return "\"%s\",\n\"%s\",\n\"%s\",\n%d,\n\"%s\"" % (
            self.fullname(),
            " ".join(_arg_syntax_code(_arg) for _arg in self.desc.get("arguments", [])),
            self.desc["summary"],
            GROUPS.index(self.group),
            self.desc["since"]
        )
    

    def __str__(self):
        return self.code()


class ContainerCommand(Command):
    def __init__(self, name, desc):
        super(ContainerCommand, self).__init__(name, desc)
        self.subcommands = []

    def subcommands_table_name(self):
        assert self.subcommands
        return "%s_Subcommands" % self.name

    def command_table_code(self):
        return super(ContainerCommand, self).command_table_code() + ",.subcommands=%s" % self.subcommands_table_name()

    def help_code(self):
        # COMMAND is the only container command that is not a pure container...
        return super(ContainerCommand, self).help_code() if self.name == "COMMAND" else None


class Subcommand(Command):
    def __init__(self, name, desc):
        super(Subcommand, self).__init__(name, desc)
        self.container_name = self.desc["container"].upper()

    def fullname(self):
        return "%s %s" % (self.container_name, name)


def create_command(name, desc):
    if desc.get("container"):
        cmd = Subcommand(name, desc)
        subcommands.setdefault(desc["container"].upper(), {})[name] = cmd
    else:
        if desc.get("subcommands"):
            cmd = ContainerCommand(name, desc)
            container_commands[name.upper()] = cmd
        else:
            cmd = Command(name.upper(), desc)
        commands[name.upper()] = cmd


def gen_commands_c():
    def write_internal_structures(f, command):
        f.write("/* %s RESP2 return types */\n" % command.fullname())
        f.write("commandReturnType %s[] = {\n" % command.return_types_table_name(2))
        f.write("%s\n" % command.return_types_code(2))
        f.write("};\n\n")

        f.write("/* %s RESP3 return types */\n" % command.fullname())
        f.write("commandReturnType %s[] = {\n" % command.return_types_table_name(3))
        f.write("%s\n" % command.return_types_code(3))
        f.write("};\n\n")

        f.write("/* %s history */\n" % command.fullname())
        f.write("commandReturnType %s[] = {\n" % command.history_table_name())
        f.write("%s\n" % command.history_code())
        f.write("};\n\n")

    def write_command_table(f, container_name, table_name, command_list):
        f.write("/* %s command table */\n" % (container_name or "Main"))
        f.write("struct redisCommand %s[] = {\n" % table_name)
        curr_group = None
        for command in command_list:
            #print command.fullname()
            #print command.help_code()
            if container_name is None and curr_group != command.group:
                curr_group = command.group
                f.write("/* %s */\n" % curr_group)
            f.write("{%s},\n" % command.command_table_code())
        f.write("{0}\n")
        f.write("};\n\n")


    print("Generating commands.c...")
    with open("commands.c","w") as f:
        f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
        f.write("#include \"server.h\"\n")
        f.write(
"""
/* Our command table.
 *
 * Every entry is composed of the following fields:
 *
 * name:        A string representing the command name.
 *
 * function:    Pointer to the C function implementing the command.
 *
 * arity:       Number of arguments, it is possible to use -N to say >= N
 *
 * sflags:      Command flags as string. See below for a table of flags.
 *
 * flags:       Flags as bitmask. Computed by Redis using the 'sflags' field.
 *
 * get_keys_proc: An optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *
 * first_key_index: First argument that is a key
 *
 * last_key_index: Last argument that is a key
 *
 * key_step:    Step to get all the keys from first to last argument.
 *              For instance in MSET the step is two since arguments
 *              are key,val,key,val,...
 *
 * microseconds: Microseconds of total execution time for this command.
 *
 * calls:       Total number of calls of this command.
 *
 * id:          Command bit identifier for ACLs or other goals.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using space separated strings, that are turned
 * into actual flags by the populateCommandTable() function.
 *
 * This is the meaning of the flags:
 *
 * write:       Write command (may modify the key space).
 *
 * read-only:   Commands just reading from keys without changing the content.
 *              Note that commands that don't read from the keyspace such as
 *              TIME, SELECT, INFO, administrative commands, and connection
 *              or transaction related commands (multi, exec, discard, ...)
 *              are not flagged as read-only commands, since they affect the
 *              server or the connection in other ways.
 *
 * use-memory:  May increase memory usage once called. Don't allow if out
 *              of memory.
 *
 * admin:       Administrative command, like SAVE or SHUTDOWN.
 *
 * pub-sub:     Pub/Sub related command.
 *
 * no-script:   Command not allowed in scripts.
 *
 * random:      Random command. Command is not deterministic, that is, the same
 *              command with the same arguments, with the same key space, may
 *              have different results. For instance SPOP and RANDOMKEY are
 *              two random commands.
 *
 * to-sort:     Sort command output array if called from script, so that the
 *              output is deterministic. When this flag is used (not always
 *              possible), then the "random" flag is not needed.
 *
 * ok-loading:  Allow the command while loading the database.
 *
 * ok-stale:    Allow the command while a slave has stale data but is not
 *              allowed to serve this data. Normally no command is accepted
 *              in this condition but just a few.
 *
 * no-monitor:  Do not automatically propagate the command on MONITOR.
 *
 * no-slowlog:  Do not automatically propagate the command to the slowlog.
 *
 * cluster-asking: Perform an implicit ASKING for this command, so the
 *              command will be accepted in cluster mode if the slot is marked
 *              as 'importing'.
 *
 * fast:        Fast command: O(1) or O(log(N)) command that should never
 *              delay its execution as long as the kernel scheduler is giving
 *              us time. Note that commands that may trigger a DEL as a side
 *              effect (like SET) are not fast commands.
 * 
 * may-replicate: Command may produce replication traffic, but should be 
 *                allowed under circumstances where write commands are disallowed. 
 *                Examples include PUBLISH, which replicates pubsub messages,and 
 *                EVAL, which may execute write commands, which are replicated, 
 *                or may just execute read commands. A command can not be marked 
 *                both "write" and "may-replicate"
 *
 * sentinel: This command is present in sentinel mode too.
 *
 * sentinel-only: This command is present only when in sentinel mode.
 *
 * The following additional flags are only used in order to put commands
 * in a specific ACL category. Commands can have multiple ACL categories.
 * See redis.conf for the exact meaning of each.
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo.
 *
 * Note that:
 *
 * 1) The read-only flag implies the @read ACL category.
 * 2) The write flag implies the @write ACL category.
 * 3) The fast flag implies the @fast ACL category.
 * 4) The admin flag implies the @admin and @dangerous ACL category.
 * 5) The pub-sub flag implies the @pubsub ACL category.
 * 6) The lack of fast flag implies the @slow ACL category.
 * 7) The non obvious "keyspace" category includes the commands
 *    that interact with keys without having anything to do with
 *    specific data structures, such as: DEL, RENAME, MOVE, SELECT,
 *    TYPE, EXPIRE*, PEXPIRE*, TTL, PTTL, ...
 */\n
"""
        )
        # Write help structires (history, arguments, etc.)
        for command in sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name)):
            write_internal_structures(f, command)
            if isinstance(command, ContainerCommand):
                for subcmd in sorted(container.subcommands, key=lambda cmd: cmd.name):
                    write_internal_structures(f, subcmd)
        # Write all subcommand tables
        for container in sorted(container_commands.values(), key=lambda cmd: (cmd.group, cmd.name)):
            write_command_table(f, container.name, container.subcommands_table_name(), sorted(container.subcommands, key=lambda cmd: cmd.name))
        # Write main command table
        write_command_table(f, None, "redisCommandTable", sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name)))


def gen_help_h():
    print("Generating help.h...")
    with open("help.h","w") as f:
        f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
        f.write("#ifndef __REDIS_HELP_H\n")
        f.write("#define __REDIS_HELP_H\n\n")
        f.write("static char *commandGroups[] = {\n")
        for group in GROUPS:
            f.write("    \"%s\",\n" % group)
        f.write("};\n")
        f.write(
"""
struct commandHelp {
    char *name;
    char *params;
    char *summary;
    int group;
    char *since;
} commandHelp[] = {
"""
        )
        for command in sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name)):
            help_code = command.help_code()
            if help_code:
                f.write("{%s},\n" % help_code)
            if isinstance(command, ContainerCommand):
                for subcmd in sorted(container.subcommands, key=lambda cmd: cmd.name):
                    f.write("{%s},\n" % subcmd.help_code())
        f.write("};\n\n")
        f.write("#endif\n\n")

parser = argparse.ArgumentParser()
parser.add_argument("--gen-commands-c", help="Generate src/commands.c", action="store_true")
parser.add_argument("--gen-help-h", help="Generate src/help.h", action="store_true")
args = parser.parse_args()

if not (args.gen_commands_c or args.gen_help_h):
    exit(0)

# Create all command objects
print("Processing json files...")
for filename in glob.glob('commands/*.json'):
    print(filename)
    if filename != "commands/set.json":
        continue
    with open(filename,"r") as f:
        d = json.load(f)
        for name, desc in d.items():
            create_command(name, desc)

# Link subcommands to containers
print("Linking container command to subcommands...")
for container in container_commands.values():
    for subcommand in subcommands[container.name].values():
        container.subcommands.append(subcommand)


if args.gen_commands_c:
    gen_commands_c()

if args.gen_help_h:
    gen_help_h()

print("All done, exiting.")

