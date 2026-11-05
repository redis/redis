# MOD-18948: per-write-command primary -> replica data-integrity verification.
#
# For every write command the module registers, run one representative
# invocation on the primary and assert the replica ends up holding the same
# data. Catches commands that are implemented correctly but propagate wrongly
# or not at all (MOD-16050: CF.LOADCHUNK replicated only its header chunk).

from common import *
from replication_utils import _copy_chunks, _wait_link_up, _verify_row


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


