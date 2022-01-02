#!/usr/bin/env python
import argparse
import json
from collections import OrderedDict
from sys import argv, stdin

def convert_flags_to_boolean_dict(flags):
    """Return a dict with a key set to `True` per element in the flags list."""
    return {f: True for f in flags}


def set_if_not_none_or_empty(dst, key, value):
    """Set 'key' in 'dst' if 'value' is not `None` or an empty list."""
    if value is not None and (type(value) is not list or len(value)):
        dst[key] = value


def convert_argument(arg):
    """Transform an argument."""
    arg.update(convert_flags_to_boolean_dict(arg.pop('flags', [])))
    set_if_not_none_or_empty(arg, 'arguments', 
                            [convert_argument(x) for x in arg.pop('arguments',[])])
    return arg


def convert_keyspec(spec):
    """Transform a key spec."""
    spec.update(convert_flags_to_boolean_dict(spec.pop('flags', [])))
    return spec


def convert_entry_to_objects_array(container, cmd):
    """Transform the JSON output of `COMMAND` to a friendlier format.

    `COMMAND`'s output per command is a fixed-size (8) list as follows:
    1. Name (lower case, e.g. "lolwut")
    2. Arity
    3. Flags
    4-6. First/last/step key specification (deprecated as of Redis v7.0)
    7. ACL categories
    8. A dict of meta information (as of Redis 7.0)

    This returns a list with a dict for the command and per each of its
    subcommands. Each dict contains one key, the command's full name, with a
    value of a dict that's set with the command's properties and meta
    information."""
    assert len(cmd) >= 8
    obj = {}
    rep = [obj]
    name = cmd[0].upper()
    arity = cmd[1]
    command_flags = cmd[2]
    acl_categories   = cmd[6]
    meta = cmd[7]
    key = f'{container} {name}' if container else name

    rep.extend([convert_entry_to_objects_array(name, x)[0] for x in meta.pop('subcommands', [])])

    # The command's value is ordered so the interesting stuff that we care about
    # is at the start. Optional `None` and empty list values are filtered out.
    value = OrderedDict()
    value['summary'] = meta.pop('summary')
    value['since'] = meta.pop('since')
    value['group'] = meta.pop('group')
    set_if_not_none_or_empty(value, 'complexity', meta.pop('complexity', None))
    set_if_not_none_or_empty(value, 'deprecated_since', meta.pop('deprecated_since', None))
    set_if_not_none_or_empty(value, 'replaced_by', meta.pop('replaced_by', None))
    set_if_not_none_or_empty(value, 'history', meta.pop('history', []))
    set_if_not_none_or_empty(value, 'acl_categories', acl_categories)
    value['arity'] = arity
    set_if_not_none_or_empty(value, 'key_specs', 
                            [convert_keyspec(x) for x in meta.pop('key_specs',[])])
    set_if_not_none_or_empty(value, 'arguments',
                            [convert_argument(x) for x in meta.pop('arguments', [])])
    set_if_not_none_or_empty(value, 'command_flags', command_flags)
    set_if_not_none_or_empty(value, 'doc_flags', meta.pop('doc_flags', []))
    set_if_not_none_or_empty(value, 'hints', meta.pop('hints', []))

    # All remaining meta key-value tuples, if any, are appended to the command
    # to be future-proof.
    while len(meta) > 0:
        (k, v) = meta.popitem()
        value[k] = v

    obj[key] = value
    return rep


# MAIN
if __name__ == '__main__':
    opts = {
        'description': 'Transform the output from `redis-cli --json COMMAND` to commands.json format.',
        'epilog': f'Usage example: src/redis-cli --json COMMAND | {argv[0]}'
    }
    parser = argparse.ArgumentParser(**opts)
    parser.add_argument('input', help='JSON-formatted input file (default: stdin)',
                        nargs='?', type=argparse.FileType(), default=stdin)
    args = parser.parse_args()

    payload = OrderedDict()
    commands = []
    data = json.load(args.input)

    for entry in data:
        cmds = convert_entry_to_objects_array(None, entry)
        commands.extend(cmds)

    # The final output is a dict of all commands, ordered by name.
    commands.sort(key=lambda x: list(x.keys())[0])
    for cmd in commands:
        name = list(cmd.keys())[0]
        payload[name] = cmd[name]

    print(json.dumps(payload, indent=4))
