#!/usr/bin/env python
# Generate a commands.json file from the output of the `COMMAND` command
# Prerequisites: Python 3.5+ and redis-py
import argparse
import functools
import json
import redis

def convert_list_to_dict(arr, recurse):
    rep = {}
    for i in range(0,len(arr),2):
        key = arr[i].replace('-', '_')
        val = arr[i+1]
        if type(val) is list and recurse:
            val = convert_list_to_dict(val, recurse)
        rep[key] = val
    return rep

def convert_flags_to_truthy_dict(flags):
    rep = {}
    for f in flags:
        rep[f] = True
    return rep

def convert_argument(arg):
    rep = convert_list_to_dict(arg, False)
    if 'flags' in rep:
        rep = {
            **rep,
            **convert_flags_to_truthy_dict(rep['flags'])
        }
        del rep['flags']
    if 'arguments' in rep:
        rep['arguments'] = list(map(lambda x: convert_argument(x), rep['arguments']))
    return rep

def convert_keyspec(spec):
    rep = convert_list_to_dict(spec, False)
    if 'flags' in rep:
        rep = {
            **rep,
            **convert_flags_to_truthy_dict(rep['flags'])
        }
        del rep['flags']
    rep['begin_search'] = convert_list_to_dict(rep['begin_search'], True)
    rep['find_keys'] = convert_list_to_dict(rep['find_keys'], True)
    return rep

def set_key_if_in_value_and_not_empty_list(obj, key, value):
    if key in value and (type(value[key]) is not list or len(value[key]) > 0):
        obj[key] = value[key]
    return obj

def convert_command_to_objects(container, cmd):
    obj = {}
    rep = [obj]
    name = cmd[0].upper()
    key = f'{container} {name}' if container else name

    meta = convert_list_to_dict(cmd[7], False)
    meta['command_flags'] = cmd[2]
    meta['acl_categories'] = cmd[6]

    if 'key_specs' in meta:
        meta['key_specs'] = list(map(lambda x: convert_keyspec(x), meta['key_specs']))
    if 'arguments' in meta:
        meta['arguments'] = list(map(lambda x: convert_argument(x), meta['arguments']))
    if 'subcommands' in meta:
        sub = list(map(lambda x: convert_command_to_objects(name, x), meta['subcommands']))
        rep += [s for sc in sub for s in sc]
    if 'doc_flags' in meta:
        meta['doc_flags'] = convert_flags_to_truthy_dict(meta['doc_flags'])
    else:
        meta['doc_flags'] = {}

    FIELDS = [
        'summary',
        'since',
        'group',
        'complexity',
        'deprecated_since',
        'replaced_by',
        'history',
        'acl_categories',
        'key_specs',
        'arguments',
        'command_flags'
    ]
    obj[key] = {}
    for field in FIELDS:
        obj[key] = set_key_if_in_value_and_not_empty_list(obj[key], field, meta)

    obj[key] = {
        **obj[key],
        'arity': cmd[1],
        'first': cmd[3],
        'last': cmd[4],
        'step': cmd[5],
        **meta['doc_flags'],
    }
    return rep

# MAIN

# Parse arguments
parser = argparse.ArgumentParser(description="generate commands.json from a Redis server")
parser.add_argument("-u", "--uri", type=str, help="the server's URI", default="redis://default:@localhost:6379")
args = parser.parse_args()

# Get `COMMAND`'s output
r = redis.Redis().from_url(args.uri, decode_responses=True)
command_output = r.command()
r.close()

# Transform the output
commands = list(map(lambda x: convert_command_to_objects(None,x), command_output))
commands = [command for cmd in commands for command in cmd]
commands.sort(key=lambda x: list(x.keys())[0])

payload = {}
for cmd in commands:
    name = list(cmd.keys())[0]
    payload[name] = cmd[name]

# Output JSON
print(json.dumps(payload, indent=4))