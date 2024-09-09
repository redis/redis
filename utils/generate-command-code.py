#!/usr/bin/env python3
import glob
import json
import os
import argparse

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


def get_optional_desc_string(desc, field, force_uppercase=False):
    v = desc.get(field, None)
    if v and force_uppercase:
        v = v.upper()
    ret = "\"%s\"" % v if v else "NULL"
    return ret.replace("\n", "\\n")


def check_command_args_key_specs(args, command_key_specs_index_set, command_arg_key_specs_index_set):
    if not args:
        return True

    for arg in args:
        if arg.key_spec_index is not None:
            assert isinstance(arg.key_spec_index, int)

            if arg.key_spec_index not in command_key_specs_index_set:
                print("command: %s arg: %s key_spec_index error" % (command.fullname(), arg.name))
                return False

            command_arg_key_specs_index_set.add(arg.key_spec_index)

        if not check_command_args_key_specs(arg.subargs, command_key_specs_index_set, command_arg_key_specs_index_set):
            return False

    return True

def check_command_key_specs(command):
    if not command.key_specs:
        return True

    assert isinstance(command.key_specs, list)

    for cmd_key_spec in command.key_specs:
        if "flags" not in cmd_key_spec:
            print("command: %s key_specs missing flags" % command.fullname())
            return False

        if "NOT_KEY" in cmd_key_spec["flags"]:
            # Like SUNSUBSCRIBE / SPUBLISH / SSUBSCRIBE
            return True

    command_key_specs_index_set = set(range(len(command.key_specs)))
    command_arg_key_specs_index_set = set()

    # Collect key_spec used for each arg, including arg.subarg
    if not check_command_args_key_specs(command.args, command_key_specs_index_set, command_arg_key_specs_index_set):
        return False

    # Check if we have key_specs not used
    if command_key_specs_index_set != command_arg_key_specs_index_set:
        print("command: %s may have unused key_spec" % command.fullname())
        return False

    return True


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


def verify_no_dup_names(container_fullname, args):
    name_list = [arg.name for arg in args]
    name_set = set(name_list)
    if len(name_list) != len(name_set):
        print("{}: Dup argument names: {}".format(container_fullname, name_list))
        exit(1)


class Argument(object):
    def __init__(self, parent_name, desc):
        self.parent_name = parent_name
        self.desc = desc
        self.name = self.desc["name"].lower()
        if "_" in self.name:
            print("{}: name ({}) should not contain underscores".format(self.fullname(), self.name))
            exit(1)
        self.type = self.desc["type"]
        self.key_spec_index = self.desc.get("key_spec_index", None)
        self.subargs = []
        if self.type in ["oneof", "block"]:
            self.display = None
            for subdesc in self.desc["arguments"]:
                self.subargs.append(Argument(self.fullname(), subdesc))
            if len(self.subargs) < 2:
                print("{}: oneof or block arg contains less than two subargs".format(self.fullname()))
                exit(1)
            verify_no_dup_names(self.fullname(), self.subargs)
        else:
            self.display = self.desc.get("display")

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
        MAKE_ARG("expiration",ARG_TYPE_ONEOF,-1,NULL,NULL,NULL,CMD_ARG_OPTIONAL,5,NULL),.subargs=GETEX_expiration_Subargs
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

        s = "MAKE_ARG(\"%s\",%s,%d,%s,%s,%s,%s,%d,%s)" % (
            self.name,
            ARG_TYPES[self.type],
            self.desc.get("key_spec_index", -1),
            get_optional_desc_string(self.desc, "token", force_uppercase=True),
            get_optional_desc_string(self.desc, "summary"),
            get_optional_desc_string(self.desc, "since"),
            _flags_code(),
            len(self.subargs),
            get_optional_desc_string(self.desc, "deprecated_since"),
        )
        if "display" in self.desc:
            s += ",.display_text=\"%s\"" % self.desc["display"].lower()
        if self.subargs:
            s += ",.subargs=%s" % self.subarg_table_name()

        return s

    def write_internal_structs(self, f):
        if self.subargs:
            for subarg in self.subargs:
                subarg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct COMMAND_ARG %s[] = {\n" % self.subarg_table_name())
            for subarg in self.subargs:
                f.write("{%s},\n" % subarg.struct_code())
            f.write("};\n\n")


def to_c_name(str):
    return str.replace(":", "").replace(".", "_").replace("$", "_")\
        .replace("^", "_").replace("*", "_").replace("-", "_") \
        .replace("\\", "_").replace("+", "_")


class ReplySchema(object):
    def __init__(self, name, desc):
        self.name = to_c_name(name)
        self.schema = {}
        if desc.get("type") == "object":
            if desc.get("properties") and desc.get("additionalProperties") is None:
                print("%s: Any object that has properties should have the additionalProperties field" % self.name)
                exit(1)
        elif desc.get("type") == "array":
            if desc.get("items") and isinstance(desc["items"], list) and any([desc.get(k) is None for k in ["minItems", "maxItems"]]):
                print("%s: Any array that has items should have the minItems and maxItems fields" % self.name)
                exit(1)
        for k, v in desc.items():
            if isinstance(v, dict):
                self.schema[k] = ReplySchema("%s_%s" % (self.name, k), v)
            elif isinstance(v, list):
                self.schema[k] = []
                for i, subdesc in enumerate(v):
                    self.schema[k].append(ReplySchema("%s_%s_%i" % (self.name, k,i), subdesc))
            else:
                self.schema[k] = v
    
    def write(self, f):
        def struct_code(name, k, v):
            if isinstance(v, ReplySchema):
                t = "JSON_TYPE_OBJECT"
                vstr = ".value.object=&%s" % name
            elif isinstance(v, list):
                t = "JSON_TYPE_ARRAY"
                vstr = ".value.array={.objects=%s,.length=%d}" % (name, len(v))
            elif isinstance(v, bool):
                t = "JSON_TYPE_BOOLEAN"
                vstr = ".value.boolean=%d" % int(v)
            elif isinstance(v, str):
                t = "JSON_TYPE_STRING"
                vstr = ".value.string=\"%s\"" % v
            elif isinstance(v, int):
                t = "JSON_TYPE_INTEGER"
                vstr = ".value.integer=%d" % v
            
            return "%s,%s,%s" % (t, json.dumps(k), vstr)

        for k, v in self.schema.items():
            if isinstance(v, ReplySchema):
                v.write(f)
            elif isinstance(v, list):
                for i, schema in enumerate(v):
                    schema.write(f)
                name = to_c_name("%s_%s" % (self.name, k))
                f.write("/* %s array reply schema */\n" % name)
                f.write("struct jsonObject *%s[] = {\n" % name)
                for i, schema in enumerate(v):
                    f.write("&%s,\n" % schema.name)
                f.write("};\n\n")
            
        f.write("/* %s reply schema */\n" % self.name)
        f.write("struct jsonObjectElement %s_elements[] = {\n" % self.name)
        for k, v in self.schema.items():
            name = to_c_name("%s_%s" % (self.name, k))
            f.write("{%s},\n" % struct_code(name, k, v))
        f.write("};\n\n")
        f.write("struct jsonObject %s = {%s_elements,.length=%d};\n\n" % (self.name, self.name, len(self.schema)))


class Command(object):
    def __init__(self, name, desc):
        self.name = name.upper()
        self.desc = desc
        self.group = self.desc["group"]
        self.key_specs = self.desc.get("key_specs", [])
        self.subcommands = []
        self.args = []
        for arg_desc in self.desc.get("arguments", []):
            self.args.append(Argument(self.fullname(), arg_desc))
        verify_no_dup_names(self.fullname(), self.args)
        self.reply_schema = None
        if "reply_schema" in self.desc:
            self.reply_schema = ReplySchema(self.reply_schema_name(), self.desc["reply_schema"])

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
        return "%s_Tips" % (self.fullname().replace(" ", "_"))

    def arg_table_name(self):
        return "%s_Args" % (self.fullname().replace(" ", "_"))

    def key_specs_table_name(self):
        return "%s_Keyspecs" % (self.fullname().replace(" ", "_"))

    def reply_schema_name(self):
        return "%s_ReplySchema" % (self.fullname().replace(" ", "_"))

    def struct_name(self):
        return "%s_Command" % (self.fullname().replace(" ", "_"))

    def history_code(self):
        if not self.desc.get("history"):
            return ""
        s = ""
        for tupl in self.desc["history"]:
            s += "{\"%s\",\"%s\"},\n" % (tupl[0], tupl[1])
        return s

    def num_history(self):
        if not self.desc.get("history"):
            return 0
        return len(self.desc["history"])

    def tips_code(self):
        if not self.desc.get("command_tips"):
            return ""
        s = ""
        for hint in self.desc["command_tips"]:
            s += "\"%s\",\n" % hint.lower()
        return s

    def num_tips(self):
        if not self.desc.get("command_tips"):
            return 0
        return len(self.desc["command_tips"])

    def key_specs_code(self):
        s = ""
        for spec in self.key_specs:
            s += "{%s}," % KeySpec(spec).struct_code()
        return s[:-1]


    def struct_code(self):
        """
        Output example:
        MAKE_CMD("set","Set the string value of a key","O(1)","1.0.0",CMD_DOC_NONE,NULL,NULL,"string",COMMAND_GROUP_STRING,SET_History,4,SET_Tips,0,setCommand,-3,CMD_WRITE|CMD_DENYOOM,ACL_CATEGORY_STRING,SET_Keyspecs,1,setGetKeys,5),.args=SET_Args
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

        s = "MAKE_CMD(\"%s\",%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%d,%s,%d,%s,%s,%s,%d,%s,%d)," % (
            self.name.lower(),
            get_optional_desc_string(self.desc, "summary"),
            get_optional_desc_string(self.desc, "complexity"),
            get_optional_desc_string(self.desc, "since"),
            _doc_flags_code(),
            get_optional_desc_string(self.desc, "replaced_by"),
            get_optional_desc_string(self.desc, "deprecated_since"),
            "\"%s\"" % self.group,
            GROUPS[self.group],
            self.history_table_name(),
            self.num_history(),
            self.tips_table_name(),
            self.num_tips(),
            self.desc.get("function", "NULL"),
            self.desc["arity"],
            _flags_code(),
            _acl_categories_code(),
            self.key_specs_table_name(),
            len(self.key_specs),
            self.desc.get("get_keys_function", "NULL"),
            len(self.args),
        )

        if self.subcommands:
            s += ".subcommands=%s," % self.subcommand_table_name()

        if self.args:
            s += ".args=%s," % self.arg_table_name()

        if self.reply_schema and args.with_reply_schema:
            s += ".reply_schema=&%s," % self.reply_schema_name()

        return s[:-1]

    def write_internal_structs(self, f):
        if self.subcommands:
            subcommand_list = sorted(self.subcommands, key=lambda cmd: cmd.name)
            for subcommand in subcommand_list:
                subcommand.write_internal_structs(f)

            f.write("/* %s command table */\n" % self.fullname())
            f.write("struct COMMAND_STRUCT %s[] = {\n" % self.subcommand_table_name())
            for subcommand in subcommand_list:
                f.write("{%s},\n" % subcommand.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")

        f.write("/********** %s ********************/\n\n" % self.fullname())

        f.write("#ifndef SKIP_CMD_HISTORY_TABLE\n")
        f.write("/* %s history */\n" % self.fullname())
        code = self.history_code()
        if code:
            f.write("commandHistory %s[] = {\n" % self.history_table_name())
            f.write("%s" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.history_table_name())
        f.write("#endif\n\n")

        f.write("#ifndef SKIP_CMD_TIPS_TABLE\n")
        f.write("/* %s tips */\n" % self.fullname())
        code = self.tips_code()
        if code:
            f.write("const char *%s[] = {\n" % self.tips_table_name())
            f.write("%s" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.tips_table_name())
        f.write("#endif\n\n")

        f.write("#ifndef SKIP_CMD_KEY_SPECS_TABLE\n")
        f.write("/* %s key specs */\n" % self.fullname())
        code = self.key_specs_code()
        if code:
            f.write("keySpec %s[%d] = {\n" % (self.key_specs_table_name(), len(self.key_specs)))
            f.write("%s\n" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.key_specs_table_name())
        f.write("#endif\n\n")

        if self.args:
            for arg in self.args:
                arg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct COMMAND_ARG %s[] = {\n" % self.arg_table_name())
            for arg in self.args:
                f.write("{%s},\n" % arg.struct_code())
            f.write("};\n\n")

        if self.reply_schema and args.with_reply_schema:
            self.reply_schema.write(f)


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

parser = argparse.ArgumentParser()
parser.add_argument('--with-reply-schema', action='store_true')
args = parser.parse_args()

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

check_command_error_counter = 0  # An error counter is used to count errors in command checking.

print("Checking all commands...")
for command in commands.values():
    if not check_command_key_specs(command):
        check_command_error_counter += 1

if check_command_error_counter != 0:
    print("Error: There are errors in the commands check, please check the above logs.")
    exit(1)

commands_filename = "commands_with_reply_schema" if args.with_reply_schema else "commands"
print("Generating %s.def..." % commands_filename)
with open("%s/%s.def" % (srcdir, commands_filename), "w") as f:
    f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
    f.write(
"""
/* We have fabulous commands from
 * the fantastic
 * Redis Command Table! */

/* Must match redisCommandGroup */
const char *COMMAND_GROUP_STR[] = {
    "generic",
    "string",
    "list",
    "set",
    "sorted-set",
    "hash",
    "pubsub",
    "transactions",
    "connection",
    "server",
    "scripting",
    "hyperloglog",
    "cluster",
    "sentinel",
    "geo",
    "stream",
    "bitmap",
    "module"
};

const char *commandGroupStr(int index) {
    return COMMAND_GROUP_STR[index];
}
"""
    )

    command_list = sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name))
    for command in command_list:
        command.write_internal_structs(f)

    f.write("/* Main command table */\n")
    f.write("struct COMMAND_STRUCT redisCommandTable[] = {\n")
    curr_group = None
    for command in command_list:
        if curr_group != command.group:
            curr_group = command.group
            f.write("/* %s */\n" % curr_group)
        f.write("{%s},\n" % command.struct_code())
    f.write("{0}\n")
    f.write("};\n")

print("All done, exiting.")
