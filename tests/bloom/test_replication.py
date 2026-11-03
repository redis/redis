# MOD-18948: per-write-command primary -> replica data-integrity verification.
#
# For every write command the module registers, run one representative
# invocation on the primary and assert the replica ends up holding the same
# data. Catches commands that are implemented correctly but propagate wrongly
# or not at all (MOD-16050: CF.LOADCHUNK replicated only its header chunk).

import time
from common import *

WAIT_TIMEOUT_MS = 1000


def _exec(con, spec):
    """A spec is either a command arg-list or a callable(con)."""
    return spec(con) if callable(spec) else con.execute_command(*spec)


def _copy_chunks(con, dump_cmd, load_cmd, src, dst):
    """Round-trip every SCANDUMP chunk into dst -- not just the header."""
    it = 0
    while True:
        it, data = con.execute_command(dump_cmd, src, it)
        if it == 0:
            return
        con.execute_command(load_cmd, dst, it, data)


def _bf_loadchunk(con):
    con.execute_command('BF.RESERVE', 'src', '0.01', '1000')
    con.execute_command('BF.MADD', 'src', *[f'i{i}' for i in range(100)])
    _copy_chunks(con, 'BF.SCANDUMP', 'BF.LOADCHUNK', 'src', 'k')


# (command, setup, write-under-test, reads compared on both sides)
#
# One invocation per command, no argument permutations -- but the invocation is
# the form most likely to diverge: autocreate-on-write over a pre-reserved key,
# a scale-out over a single insert, a full chunk round-trip over a header.
ROWS = [
    ('bf.reserve', None, ['BF.RESERVE', 'k', '0.01', '100'],
     [['BF.INFO', 'k']]),
    ('bf.add', None, ['BF.ADD', 'k', 'a'],
     [['BF.EXISTS', 'k', 'a'], ['BF.INFO', 'k']]),
    ('bf.madd', None, ['BF.MADD', 'k', 'a', 'b', 'c'],
     [['BF.MEXISTS', 'k', 'a', 'b', 'c'], ['BF.INFO', 'k']]),
    # CAPACITY 2 forces scaling: the new sub-filters must replicate too.
    ('bf.insert', None, ['BF.INSERT', 'k', 'CAPACITY', '2', 'ITEMS', 'a', 'b', 'c', 'd', 'e'],
     [['BF.MEXISTS', 'k', 'a', 'b', 'c', 'd', 'e'], ['BF.INFO', 'k']]),
    ('bf.loadchunk', None, _bf_loadchunk,
     [['BF.MEXISTS', 'k', 'i0', 'i50', 'i99'], ['BF.INFO', 'k'], ['BF.CARD', 'k']]),

]

# Write commands deliberately not in ROWS, with the reason. Keep this empty.
KNOWN_EXCLUSIONS = set()

NO_DUMP_COMPARE = set()


def _wait_link_up(env, timeout=10):
    """RLTest starts the replica with --slaveof but never waits for the sync."""
    slave = env.getSlaveConnection()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if slave.execute_command('INFO', 'replication')['master_link_status'] == 'up':
            return
        time.sleep(0.1)
    env.assertTrue(False, message='replica link never came up')


def _is_trivial(val):
    """A read that returns the type default proves nothing -- both sides match."""
    if val is None or val == 0 or val == b'' or val == '':
        return True
    if isinstance(val, (list, tuple)):
        return len(val) == 0 or all(_is_trivial(v) for v in val)
    return False


def _read_slave(con, spec):
    """A divergence must be reported as a diff, not raised -- keep rows independent."""
    try:
        return con.execute_command(*spec)
    except ResponseError as e:
        return f'error: {e}'


def _keys(con):
    return sorted(con.execute_command('KEYS', '*'))


def _dumps(con, keys):
    return [con.execute_command('DUMP', k) for k in keys]


def _verify_row(env, master, slave, cmd, setup, write, reads):
    master.execute_command('FLUSHALL')
    if setup is not None:
        for spec in (setup if isinstance(setup, list) else [setup]):
            _exec(master, spec)
    _exec(master, write)

    acked = master.execute_command('WAIT', 1, WAIT_TIMEOUT_MS)
    env.assertEqual(acked, 1, message=f'{cmd}: replica did not ack the write')

    # Snapshot before reading: some reads (TDIGEST.QUANTILE) compact in place.
    keys = _keys(master)
    env.assertEqual(keys, _keys(slave), message=f'{cmd}: key set diverged')
    if cmd not in NO_DUMP_COMPARE:
        env.assertEqual(_dumps(master, keys), _dumps(slave, keys),
                        message=f'{cmd}: DUMP diverged')

    for spec in reads:
        got = master.execute_command(*spec)
        env.assertFalse(_is_trivial(got), message=f'{cmd}: master {spec[0]} returned a default value')
        env.assertEqual(got, _read_slave(slave, spec), message=f'{cmd}: {spec[0]} diverged')


def testWriteCommandsReplicate(env):
    env.skipOnCluster()  # a cluster env has no replica connection to read
    # Under --use-aof the replica below would run with AOF, and RLTest intermittently hangs
    # stopping such a replica (it ignores SIGTERM; RLTest then waits on it with no timeout).
    # Replication is covered by the other env groups, so skip rather than add it here.
    env.skipOnAOF()
    env = Env(useSlaves=True, protocol=2)
    master, slave = env.getConnection(), env.getSlaveConnection()
    _wait_link_up(env)
    for cmd, setup, write, reads in ROWS:
        _verify_row(env, master, slave, cmd, setup, write, reads)


def testEveryWriteCommandIsCovered(env):
    """The command table is only as good as its coverage of the real command set."""
    env = Env(protocol=2)  # no replica needed: this only reads the command table
    if server_version_less_than(env, '7.0'):
        env.skip()
    con = env.getConnection()
    # redis-py pipes every COMMAND * reply through its COMMAND INFO parser,
    # which cannot read a COMMAND LIST reply. Take the raw replies instead.
    con.set_response_callback('COMMAND', lambda r, **_: r)
    names = con.execute_command('COMMAND', 'LIST', 'FILTERBY', 'PATTERN', 'bf.*')
    info = con.execute_command('COMMAND', 'INFO', *names)
    write_cmds = {c[0].decode().lower() for c in info if c and b'write' in c[2]}
    missing = write_cmds - {cmd for cmd, _, _, _ in ROWS} - KNOWN_EXCLUSIONS
    env.assertEqual(missing, set(), message=f'write commands with no replication test: {missing}')


