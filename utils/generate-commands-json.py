#!/usr/bin/env python
'''
Generate a commands.json file from the output of `redis-cli --jspn COMMAND`
'''
import argparse
import json
import sys

def convert_flags_to_truthy_dict(flags):
    ''' Return a dict with a key set to `True` per element in the flags list. '''
    return {f: True for f in flags} # TODO: remove after guy's PR

def convert_argument(arg):
    ''' Transform an argument. '''
    arg.update(convert_flags_to_truthy_dict(arg.pop('flags', [])))
    arg['arguments'] = [convert_argument(x) for x in arg.pop('arguments',[])]
    return arg

def convert_keyspec(spec):
    ''' Transform a key spec. '''
    spec.update(convert_flags_to_truthy_dict(spec.pop('flags', [])))
    return spec

def convert_entry_to_objects_array(container, cmd):
    ''' 
    Transform the JSON output of `COMMAND` to a friendlier format.

    `COMMAND`'s outout per command is a fixed-size (8) list as follows:
    1. Name (lower case, e.g. "lolwut")
    2. Arity
    3. Flags
    4-6. First/last/step key specification (deprecated as of Redis v7.0)
    7. ACL categories
    8. A dict of meta information (as of Redis 7.0)

    This returns a list with a dict for the command and per each of its
    subcommands. Each dict contains one key, the command's full name, with a
    value of a dict that's set with the command's properties.
    '''
    obj = {}
    rep = [obj]
    name = cmd[0].upper()
    key = f'{container} {name}' if container else name

    meta = cmd[7]
    meta['arity'] = cmd[1]
    meta['command_flags'] = cmd[2]
    meta['acl_categories'] = cmd[6]
    meta['key_specs'] = [convert_keyspec(x) for x in meta.pop('key_specs',[])]
    meta['arguments'] = [convert_argument(x) for x in meta.pop('arguments', [])]
    meta['doc_flags'] = [convert_flags_to_truthy_dict(x) for x in meta.pop('doc_flags', [])]
    rep.extend([convert_entry_to_objects_array(name, x)[0] for x in meta.pop('subcommands', [])])

    FIELDS = [
        'summary',
        'since',
        'group',
        'complexity',
        'deprecated_since',
        'replaced_by',
        'history',
        'acl_categories',
        'arity',
        'key_specs',
        'arguments',
        'command_flags',
        'doc_flags',
    ]
    obj[key] = {}
    for field in FIELDS:
        if field in meta and (type(meta[field]) is not list or len(meta[field]) > 0):
            obj[key][field] = meta[field]

    return rep

# MAIN
parser = argparse.ArgumentParser()
# parser.add_argument('input', nargs='?', type=argparse.FileType(), default=sys.stdin)
parser.add_argument('input', nargs='?', type=argparse.FileType(), default='/Users/itamar/commands.json')
args = parser.parse_args()

payload = {}
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