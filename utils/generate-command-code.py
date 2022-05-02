#!/usr/bin/env python3

import os
import glob
import json

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

GROUPS = {
    "generic": "COMMAND_GROUP_GENERIC",
    "string": "COMMAND_GROUP_STRING",
    "list": "COMMAND_GROUP_LIST",
    "set": "COMMAND_GROUP_SET",
    "sorted_set": "COMMAND_GROUP_SORTED_SET",
    "hash": "COMMAND_GROUP_HASH",
    "pubsub": "COMMAND_GROUP_PUBSUB",
    "transactions": "COMMAND_GROUP_TRANSACTIONS",
    "connection": "COMMAND_GROUP_CONNECTION",
    "server": "COMMAND_GROUP_SERVER",
    "scripting": "COMMAND_GROUP_SCRIPTING",
    "hyperloglog": "COMMAND_GROUP_HYPERLOGLOG",
    "cluster": "COMMAND_GROUP_CLUSTER",
    "sentinel": "COMMAND_GROUP_SENTINEL",
    "geo": "COMMAND_GROUP_GEO",
    "stream": "COMMAND_GROUP_STREAM",
    "bitmap": "COMMAND_GROUP_BITMAP",
}

RESP2_TYPES = {
    "simple-string": "RESP2_SIMPLE_STRING",
    "error": "RESP2_ERROR",
    "integer": "RESP2_INTEGER",
    "bulk-string": "RESP2_BULK_STRING",
    "null-bulk-string": "RESP2_NULL_BULK_STRING",
    "array": "RESP2_ARRAY",
    "null-array": "RESP2_NULL_ARRAY",
}

RESP3_TYPES = {
    "simple-string": "RESP3_SIMPLE_STRING",
    "error": "RESP3_ERROR",
    "integer": "RESP3_INTEGER",
    "double": "RESP3_DOUBLE",
    "bulk-string": "RESP3_BULK_STRING",
    "array": "RESP3_ARRAY",
    "map": "RESP3_MAP",
    "set": "RESP3_SET",
    "bool": "RESP3_BOOL",
    "null": "RESP3_NULL",
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


class KeySpec(object):
    def __init__(self, spec):
        self.spec = spec

    def struct_code(self):
        def _flags_code():
            s = ""
            for flag in self.spec.get("flags", []):
                s += "CMD_KEY_%s|" % flag
            return s[:-1] if s else 0

        def _begin_search_code():
            if self.spec["begin_search"].get("index"):
                return "KSPEC_BS_INDEX,.bs.index={%d}" % (
                    self.spec["begin_search"]["index"]["pos"]
                )
            elif self.spec["begin_search"].get("keyword"):
                return "KSPEC_BS_KEYWORD,.bs.keyword={\"%s\",%d}" % (
                    self.spec["begin_search"]["keyword"]["keyword"],
                    self.spec["begin_search"]["keyword"]["startfrom"],
                )
            elif "unknown" in self.spec["begin_search"]:
                return "KSPEC_BS_UNKNOWN,{{0}}"
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
                return "KSPEC_FK_KEYNUM,.fk.keynum={%d,%d,%d}" % (
                    self.spec["find_keys"]["keynum"]["keynumidx"],
                    self.spec["find_keys"]["keynum"]["firstkey"],
                    self.spec["find_keys"]["keynum"]["step"]
                )
            elif "unknown" in self.spec["find_keys"]:
                return "KSPEC_FK_UNKNOWN,{{0}}"
            else:
                print("Invalid find_keys! value=%s" % self.spec["find_keys"])
                exit(1)

        return "%s,%s,%s,%s" % (
            get_optional_desc_string(self.spec, "notes"),
            _flags_code(),
            _begin_search_code(),
            _find_keys_code()
        )


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
        "expiration",ARG_TYPE_ONEOF,NULL,NULL,NULL,CMD_ARG_OPTIONAL,.value.subargs=SET_expiration_Subargs
        """

        def _flags_code():
            s = ""
            if self.desc.get("optional", False):
                s += "CMD_ARG_OPTIONAL|"
            if self.desc.get("multiple", False):
                s += "CMD_ARG_MULTIPLE|"
            if self.desc.get("multiple_token", False):
                assert self.desc.get("multiple", False)  # Sanity
                s += "CMD_ARG_MULTIPLE_TOKEN|"
            return s[:-1] if s else "CMD_ARG_NONE"

        s = "\"%s\",%s,%d,%s,%s,%s,%s" % (
            self.name,
            ARG_TYPES[self.type],
            self.desc.get("key_spec_index", -1),
            get_optional_desc_string(self.desc, "token", force_uppercase=True),
            get_optional_desc_string(self.desc, "summary"),
            get_optional_desc_string(self.desc, "since"),
            _flags_code(),
        )
        if "deprecated_since" in self.desc:
            s += ",.deprecated_since=\"%s\"" % self.desc["deprecated_since"]
        if self.subargs:
            s += ",.subargs=%s" % self.subarg_table_name()

        return s

    def write_internal_structs(self, f):
        if self.subargs:
            for subarg in self.subargs:
                subarg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct redisCommandArg %s[] = {\n" % self.subarg_table_name())
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

    def return_types_table_name(self):
        return "%s_ReturnInfo" % self.fullname().replace(" ", "_")

    def subcommand_table_name(self):
        assert self.subcommands
        return "%s_Subcommands" % self.name

    def history_table_name(self):
        return "%s_History" % (self.fullname().replace(" ", "_"))

    def tips_table_name(self):
        return "%s_tips" % (self.fullname().replace(" ", "_"))

    def arg_table_name(self):
        return "%s_Args" % (self.fullname().replace(" ", "_"))

    def struct_name(self):
        return "%s_Command" % (self.fullname().replace(" ", "_"))

    def history_code(self):
        if not self.desc.get("history"):
            return ""
        s = ""
        for tupl in self.desc["history"]:
            s += "{\"%s\",\"%s\"},\n" % (tupl[0], tupl[1])
        s += "{0}"
        return s

    def tips_code(self):
        if not self.desc.get("command_tips"):
            return ""
        s = ""
        for hint in self.desc["command_tips"]:
            s += "\"%s\",\n" % hint.lower()
        s += "NULL"
        return s

    def struct_code(self):
        """
        Output example:
        "set","Set the string value of a key","O(1)","1.0.0",CMD_DOC_NONE,NULL,NULL,COMMAND_GROUP_STRING,SET_History,SET_tips,setCommand,-3,"write denyoom @string",{{"write read",KSPEC_BS_INDEX,.bs.index={1},KSPEC_FK_RANGE,.fk.range={0,1,0}}},.args=SET_Args
        """

        def _flags_code():
            s = ""
            for flag in self.desc.get("command_flags", []):
                s += "CMD_%s|" % flag
            return s[:-1] if s else 0

        def _acl_categories_code():
            s = ""
            for cat in self.desc.get("acl_categories", []):
                s += "ACL_CATEGORY_%s|" % cat
            return s[:-1] if s else 0

        def _doc_flags_code():
            s = ""
            for flag in self.desc.get("doc_flags", []):
                s += "CMD_DOC_%s|" % flag
            return s[:-1] if s else "CMD_DOC_NONE"

        def _key_specs_code():
            s = ""
            for spec in self.desc.get("key_specs", []):
                s += "{%s}," % KeySpec(spec).struct_code()
            return s[:-1]

        s = "\"%s\",%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%s," % (
            self.name.lower(),
            get_optional_desc_string(self.desc, "summary"),
            get_optional_desc_string(self.desc, "complexity"),
            get_optional_desc_string(self.desc, "since"),
            _doc_flags_code(),
            get_optional_desc_string(self.desc, "replaced_by"),
            get_optional_desc_string(self.desc, "deprecated_since"),
            GROUPS[self.group],
            self.history_table_name(),
            self.tips_table_name(),
            self.desc.get("function", "NULL"),
            self.desc["arity"],
            _flags_code(),
            _acl_categories_code()
        )

        specs = _key_specs_code()
        if specs:
            s += "{%s}," % specs

        if self.desc.get("get_keys_function"):
            s += "%s," % self.desc["get_keys_function"]

        if self.subcommands:
            s += ".subcommands=%s," % self.subcommand_table_name()

        if self.args:
            s += ".args=%s," % self.arg_table_name()

        return s[:-1]

    def write_internal_structs(self, f):
        if self.subcommands:
            subcommand_list = sorted(self.subcommands, key=lambda cmd: cmd.name)
            for subcommand in subcommand_list:
                subcommand.write_internal_structs(f)

            f.write("/* %s command table */\n" % self.fullname())
            f.write("struct redisCommand %s[] = {\n" % self.subcommand_table_name())
            for subcommand in subcommand_list:
                f.write("{%s},\n" % subcommand.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")

        f.write("/********** %s ********************/\n\n" % self.fullname())

        f.write("/* %s history */\n" % self.fullname())
        code = self.history_code()
        if code:
            f.write("commandHistory %s[] = {\n" % self.history_table_name())
            f.write("%s\n" % code)
            f.write("};\n\n")
        else:
            f.write("#define %s NULL\n\n" % self.history_table_name())

        f.write("/* %s tips */\n" % self.fullname())
        code = self.tips_code()
        if code:
            f.write("const char *%s[] = {\n" % self.tips_table_name())
            f.write("%s\n" % code)
            f.write("};\n\n")
        else:
            f.write("#define %s NULL\n\n" % self.tips_table_name())

        if self.args:
            for arg in self.args:
                arg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct redisCommandArg %s[] = {\n" % self.arg_table_name())
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

print("Generating commands.c...")
with open("%s/commands.c" % srcdir, "w") as f:
    f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
    f.write("#include \"server.h\"\n")
    f.write(
"""
/* We have fabulous commands from
 * the fantastic
 * Redis Command Table! */\n
"""
    )

    command_list = sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name))
    for command in command_list:
        command.write_internal_structs(f)

    f.write("/* Main command table */\n")
    f.write("struct redisCommand redisCommandTable[] = {\n")
    curr_group = None
    for command in command_list:
        if curr_group != command.group:
            curr_group = command.group
            f.write("/* %s */\n" % curr_group)
        f.write("{%s},\n" % command.struct_code())
    f.write("{0}\n")
    f.write("};\n")

print("All done, exiting.")
