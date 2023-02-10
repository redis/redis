#!/usr/bin/env python3
import os
import glob
import json
import jsonschema
import subprocess
import redis
import time
import argparse
import threading
import multiprocessing
import collections

try:
    from jsonschema import Draft201909Validator as schema_validator
except ImportError:
    from jsonschema import Draft7Validator as schema_validator


class Request(object):
    def __init__(self, f, docs):
        self.command = None
        self.schema = None
        self.argv = []
        self.lines_read = 0

        while True:
            line = f.readline()
            self.lines_read += 1
            if not line:
                break
            length = int(line)
            arg = str(f.read(length))
            f.read(2)  # read \r\n
            self.lines_read += 1
            if arg == "__argv_end__":
                break
            self.argv.append(arg)

        if not self.argv:
            return

        self.command = self.argv[0].lower()

        doc = docs.get(self.command, {})
        if "subcommands" in doc and len(self.argv) > 1:
            fullname = "{}|{}".format(self.argv[0].lower(), self.argv[1].lower())
            for k, v in doc["subcommands"].items():
                if fullname == k:
                    self.command = fullname
                    doc = v
        if not doc:
            print(f"Warning, missing COMMAND DOCS for '{self.command}'")
        self.schema = doc.get("reply_schema")

    def __str__(self):
        return json.dumps(self.argv)


class Response(object):
    def __init__(self, f):
        self.error = False
        self.queued = False
        self.json = None
        self.lines_read = 0

        line = f.readline()[:-2]
        self.lines_read += 1
        if line[0] == '+':
            self.json = line[1:]
            if self.json == "QUEUED":
                self.queued = True
        elif line[0] == '-':
            self.json = line[1:]
            self.error = True
        elif line[0] == '$':
            self.json = str(f.read(int(line[1:])))
            f.read(2)  # read \r\n
            self.lines_read += 1
        elif line[0] == ':':
            self.json = int(line[1:])
        elif line[0] == ',':
            self.json = float(line[1:])
        elif line[0] == '_':
            self.json = None
        elif line[0] == '#':
            self.json = line[1] == 't'
        elif line[0] == '!':
            self.json = str(f.read(int(line[1:])))
            f.read(2)  # read \r\n
            self.lines_read += 1
            self.error = True
        elif line[0] == '=':
            self.json = str(f.read(int(line[1:])))[4:]   # skip "txt:" or "mkd:"
            f.read(2)  # read \r\n
            self.lines_read += 1 + self.json.count("\r\n")
        elif line[0] == '(':
            self.json = line[1:]  # big-number is actually a string
        elif line[0] in ['*', '~', '>']:  # unfortunately JSON doesn't tell the difference between a list and a set
            self.json = []
            count = int(line[1:])
            for i in range(count):
                ele = Response(f)
                self.json.append(ele.json)
        elif line[0] in ['%', '|']:
            self.json = {}
            count = int(line[1:])
            for i in range(count):
                field = Response(f)
                # Redis allows fields to be non-strings but JSON doesn't.
                # Luckily, for any kind of response we can validate, the fields are
                # always strings (example: XINFO STREAM)
                # The reason we can't always convert to string is because of DEBUG PROTOCOL MAP
                # which anyway doesn't have a schema
                if isinstance(field.json, str):
                    field = field.json
                value = Response(f)
                self.json[field] = value.json
            if line[0] == '|':
                # We don't care abou the attributes, read the real response
                real_res = Response(f)
                self.__dict__.update(real_res.__dict__)


    def __str__(self):
        return json.dumps(self.json)


class Worker(threading.Thread):
    def __init__(self, iden):
        super(Worker, self).__init__()
        self.iden = iden
        self.terminate = False
        self.working_on = ""
        self.missing_schema = set()
        self.command_counter = dict()

    def run(self):
        while not self.terminate:
            if not self.working_on:
                time.sleep(0.01)
                continue
                
            lines_read = 0
            with open(self.working_on, "r", newline="\r\n", encoding="latin-1") as f:
                while True:
                    try:
                        req = Request(f, docs)
                        lines_read += req.lines_read
                        if not req.command:
                            break
                        res = Response(f)
                        lines_read += req.lines_read
                    except json.decoder.JSONDecodeError as err:
                       print("JSON decoder error while processing %s:%d: %s" % (filename, i, err))
                       os._exit(1)
                    except Exception as err:
                       print("General error while processing %s:%d: %s" % (filename, lines_read, err))
                       os._exit(1)

                    if res.error or res.queued:
                        continue

                    self.command_counter[req.command] = self.command_counter.get(req.command, 0) + 1

                    if not req.schema:
                        self.missing_schema.add(req.command)
                        continue

                    try:
                        jsonschema.validate(instance=res.json, schema=req.schema,
                                            cls=schema_validator)
                    except (jsonschema.ValidationError, jsonschema.exceptions.SchemaError) as err:
                        print(f"JSON schema validation error on {filename}: {err}")
                        print(f"argv: {req.argv}")
                        try:
                            print(f"Response: {res}")
                        except UnicodeDecodeError as err:
                           print("Response: (unprintable)")
                        print(f"Schema: {json.dumps(req.schema, indent=2)}")
                        os._exit(1)

            self.working_on = ""

        
# Figure out where the sources are
srcdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../src")
testdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../tests")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=str, default='%s/redis-server' % srcdir)
    parser.add_argument('--port', type=int, default=6534)
    parser.add_argument('--cli', type=str, default='%s/redis-cli' % srcdir)
    parser.add_argument('--module', type=str, action='append', default=[])
    args = parser.parse_args()

    print('Starting Redis server')
    redis_args = [args.server, '--port', str(args.port)]
    for module in args.module:
        redis_args += ['--loadmodule', 'tests/modules/%s.so' % module]
    redis_proc = subprocess.Popen(redis_args, stdout=subprocess.PIPE)
    
    while True:
        try:
            print('Connecting to Redis...')
            r = redis.Redis(port=args.port)
            r.ping()
            break
        except Exception as e:
            time.sleep(0.1)
            pass
    print('Connected')

    cli_proc = subprocess.Popen([args.cli, '-p', str(args.port), '--json', 'command', 'docs'], stdout=subprocess.PIPE)
    stdout, stderr = cli_proc.communicate()
    docs = json.loads(stdout)

    redis_proc.terminate()
    redis_proc.wait()

    workers = []
    for i in range(multiprocessing.cpu_count()):
        worker = Worker(i)
        worker.start()
        workers.append(worker)

    print("Processing files...")
    for filename in glob.glob('%s/tmp/*/*.reqres' % testdir):
        found = False
        while not found:
            for worker in workers:
                if not worker.working_on:
                    print(f"[T{worker.iden}] Processing %s ..." % filename)
                    worker.working_on = filename
                    found = True
                    break
 
    for worker in workers:
        worker.terminate = True

    missing_schema = set()
    counter = collections.Counter()
    for worker in workers:
        worker.join()
        # sum the values with same keys
        counter.update(worker.command_counter)
        missing_schema |= worker.missing_schema
    command_counter = dict(counter)

    print("Done.")
    print("Hits per command:")
    for k, v in sorted(command_counter.items()):
        print(f"  {k}: {v}")
    if missing_schema:
        print("WARNING! The following commands are missing a reply_schema:")
        for k in sorted(missing_schema):
            print(f"  {k}")



