#!/usr/bin/env python3
import glob
import json
import os

ARG_TYPES = {
    "string": "ARG_TYPE_STRING",
    "integer": "ARG_TYPE_INTEGER",
    "double": "ARG_TYPE_DOUBLE",
    "key": "ARG_TYPE_KEY",
    "pattern": "ARG_TYPE_PATTERN",
    "unix-time": "ARG_TYPE_UNIX_TIME",
    "pure-token": "ARG_TYPE_PURE_TOKEN",
    "oneof": "ARG_TYPE_ONEOF",
    "block": "ARG_TYPE_BLOCK",
}

def get_optional_desc_string(desc, field, force_uppercase=False):
    v = desc.get(field, None)
    if v and force_uppercase:
        v = v.upper()
    ret = "\"%s\"" % v if v else "NULL"
    return ret.replace("\n", "\\n")


# Globals
subcommands = {}  # container_name -> dict(subcommand_name -> Subcommand) - Only subcommands
commands = {}  # command_name -> Command - Only commands

class Argument(object):
    def __init__(self, parent_name, desc):
        self.desc = desc
        self.name = self.desc["name"].lower()
        self.type = self.desc["type"]
        self.parent_name = parent_name
        self.subargs = []
        self.subargs_name = None
        if self.type in ["oneof", "block"]:
            for subdesc in self.desc["arguments"]:
                self.subargs.append(Argument(self.fullname(), subdesc))

    def fullname(self):
        return ("%s %s" % (self.parent_name, self.name)).replace("-", "_")

    def struct_name(self):
        return "%s_Arg" % (self.fullname().replace(" ", "_"))

    def subarg_table_name(self):
        assert self.subargs
        return "%s_Subargs" % (self.fullname().replace(" ", "_"))

    def struct_code(self):
        """
        Output example:
        "expiration",ARG_TYPE_ONEOF,NULL,NULL,NULL,0,1,0,.value.subargs=SET_expiration_Subargs
        """

        s = "\"%s\",%s,%s,%s,%d,%d,%d" % (
            self.name,
            ARG_TYPES[self.type],
            get_optional_desc_string(self.desc, "token", force_uppercase=True),
            get_optional_desc_string(self.desc, "since"),
            self.desc.get("optional", 0) and 1,
            self.desc.get("multiple", 0) and 1,
            self.desc.get("multiple_token", 0) and 1,
        )
        if self.subargs:
            s += ",.subargs=%s,.numsubargs=%d" % (self.subarg_table_name(), len(self.subargs))
        return s

    def write_internal_structs(self, f):
        if self.subargs:
            for subarg in self.subargs:
                subarg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct commandArg %s[] = {\n" % self.subarg_table_name())
            for subarg in self.subargs:
                f.write("{%s},\n" % subarg.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")


class Command(object):
    def __init__(self, name, desc):
        self.name = name.upper()
        self.desc = desc
        self.group = self.desc["group"]
        self.subcommands = []
        self.args = []
        for arg_desc in self.desc.get("arguments", []):
            self.args.append(Argument(self.fullname(), arg_desc))

    def fullname(self):
        return self.name.replace("-", "_").replace(":", "")

    def subcommand_table_name(self):
        assert self.subcommands
        return "%s_Subcommands" % self.name

    def arg_table_name(self):
        return "%s_Args" % (self.fullname().replace(" ", "_"))

    def struct_name(self):
        return "%s_Command" % (self.fullname().replace(" ", "_"))

    def struct_code(self):
        """
        Output example:
        "set","NULL","Set the string value of a key","1.0.0","generic",.args=SET_Args,.numargs=5
        """

        s = "\"%s\",%s,%s,\"%s\",%s," % (
            self.name.lower(),
            "NULL",
            get_optional_desc_string(self.desc, "summary"),
            self.group,
            get_optional_desc_string(self.desc, "since"),
        )

        if self.subcommands:
            s += ".subcommands=%s," % self.subcommand_table_name()

        if self.args:
            s += ".args=%s," % self.arg_table_name()
            s += ".numargs=%d," % len(self.args)

        return s[:-1]

    def write_internal_structs(self, f):
        if self.subcommands:
            subcommand_list = sorted(self.subcommands, key=lambda cmd: cmd.name)
            for subcommand in subcommand_list:
                subcommand.write_internal_structs(f)

            f.write("/* %s command table */\n" % self.fullname())
            f.write("struct commandDocs %s[] = {\n" % self.subcommand_table_name())
            for subcommand in subcommand_list:
                f.write("{%s},\n" % subcommand.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")

        f.write("/********** %s ********************/\n\n" % self.fullname())

        if self.args:
            for arg in self.args:
                arg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct commandArg %s[] = {\n" % self.arg_table_name())
            for arg in self.args:
                f.write("{%s},\n" % arg.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")


class Subcommand(Command):
    def __init__(self, name, desc):
        self.container_name = desc["container"].upper()
        super(Subcommand, self).__init__(name, desc)

    def fullname(self):
        return "%s %s" % (self.container_name, self.name.replace("-", "_").replace(":", ""))


def create_command(name, desc):
    if desc.get("container"):
        cmd = Subcommand(name.upper(), desc)
        subcommands.setdefault(desc["container"].upper(), {})[name] = cmd
    else:
        cmd = Command(name.upper(), desc)
        commands[name.upper()] = cmd


# MAIN

# Figure out where the sources are
srcdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../src")

# Create all command objects
print("Processing json files...")
for filename in glob.glob('%s/commands/*.json' % srcdir):
    with open(filename, "r") as f:
        try:
            d = json.load(f)
            for name, desc in d.items():
                create_command(name, desc)
        except json.decoder.JSONDecodeError as err:
            print("Error processing %s: %s" % (filename, err))
            exit(1)

# Link subcommands to containers
print("Linking container command to subcommands...")
for command in commands.values():
    assert command.group
    if command.name not in subcommands:
        continue
    for subcommand in subcommands[command.name].values():
        assert not subcommand.group or subcommand.group == command.group
        subcommand.group = command.group
        command.subcommands.append(subcommand)

print("Generating cli_commands.c...")
with open("%s/cli_commands.c" % srcdir, "w") as f:
    f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
    f.write(
"""
/* Fallback command arg tables for use with pre-7.0 servers
 * that don't support COMMAND DOCS.
 */\n
"""
    )

    command_list = sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name))
    for command in command_list:
        command.write_internal_structs(f)

    f.write("/* Main command table */\n")
    f.write("struct commandDocs cliCommandDocs[] = {\n")
    curr_group = None
    for command in command_list:
        if curr_group != command.group:
            curr_group = command.group
            f.write("/* %s */\n" % curr_group)
        f.write("{%s},\n" % command.struct_code())
    f.write("{0}\n")
    f.write("};\n")

print("All done, exiting.")
