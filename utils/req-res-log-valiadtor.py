
class Request(object):
    def __init__(self, f):
        argv = []
        while True:
            line = f.readline()
            if line
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

    

# Figure out where the sources are
srcdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../src")
testdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../tests")

# MAIN
if __name__ == '__main__':
    opts = {
        'description': 'Transform the output from `redis-cli --json` using COMMAND and COMMAND DOCS to a single commands.json format.',
        'epilog': f'Usage example: {argv[0]} --cli src/redis-cli --port 6379 > commands.json'
    }
    parser = argparse.ArgumentParser(**opts)
    parser.add_argument('--host', type=str, default='localhost')
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--cli', type=str, default='%s/redis-cli' % srcdir)
    args = parser.parse_args()

    p = subprocess.Popen([args.cli, '-h', args.host, '-p', str(args.port), '--json', 'command', 'docs'],
                         stdout=subprocess.PIPE)
    stdout, stderr = p.communicate()
    docs = json.loads(stdout)

    # Create all command objects
    print("Processing files...")
    for filename in glob.glob('%s/*/*.reqres' % testdir):
        with open(filename, "r") as f:
            try:
                while f.readable():
                    req = Request(f)
                    res = Response(f)
                    s = Schema(docs, req.command())
                    if s:
                        s.validate(res)
            except json.decoder.JSONDecodeError as err:
                print("Error processing %s: %s" % (filename, err))
                exit(1)
