# MOD-18948: per-write-command primary -> replica data-integrity verification.
#
# For every write command the module registers, run one representative
# invocation on the primary and assert the replica ends up holding the same
# data. Catches commands that are implemented correctly but propagate wrongly
# or not at all (MOD-16050: CF.LOADCHUNK replicated only its header chunk).

from common import *
from replication_utils import _copy_chunks, _wait_link_up, _verify_row


def _cf_loadchunk(con):
    con.execute_command('CF.RESERVE', 'src', '1000')
    for i in range(100):
        con.execute_command('CF.ADD', 'src', f'i{i}')
    _copy_chunks(con, 'CF.SCANDUMP', 'CF.LOADCHUNK', 'src', 'k')

ROWS = [
    ('cf.reserve', None, ['CF.RESERVE', 'k', '100', 'BUCKETSIZE', '2'],
     [['CF.INFO', 'k']]),
    ('cf.add', None, ['CF.ADD', 'k', 'a'],
     [['CF.COUNT', 'k', 'a'], ['CF.INFO', 'k']]),
    ('cf.addnx', [['CF.ADD', 'k', 'a']], ['CF.ADDNX', 'k', 'b'],
     [['CF.MEXISTS', 'k', 'a', 'b'], ['CF.INFO', 'k']]),
    ('cf.insert', None, ['CF.INSERT', 'k', 'ITEMS', 'a', 'b', 'c'],
     [['CF.MEXISTS', 'k', 'a', 'b', 'c'], ['CF.INFO', 'k']]),
    ('cf.insertnx', None, ['CF.INSERTNX', 'k', 'ITEMS', 'a', 'b', 'c'],
     [['CF.MEXISTS', 'k', 'a', 'b', 'c'], ['CF.INFO', 'k']]),
    # 'a' inserted twice so the surviving count after DEL is non-trivial.
    ('cf.del', [['CF.ADD', 'k', 'a'], ['CF.ADD', 'k', 'a']], ['CF.DEL', 'k', 'a'],
     [['CF.COUNT', 'k', 'a'], ['CF.INFO', 'k']]),
    ('cf.loadchunk', None, _cf_loadchunk,
     [['CF.MEXISTS', 'k', 'i0', 'i50', 'i99'], ['CF.INFO', 'k']]),
    ('cf.compact', [['CF.RESERVE', 'k', '4'],
                    ['CF.INSERT', 'k', 'ITEMS', *map(str, range(30))]], ['CF.COMPACT', 'k'],
     [['CF.MEXISTS', 'k', '0', '15', '29'], ['CF.INFO', 'k']]),
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
    names = con.execute_command('COMMAND', 'LIST', 'FILTERBY', 'PATTERN', 'cf.*')
    info = con.execute_command('COMMAND', 'INFO', *names)
    write_cmds = {c[0].decode().lower() for c in info if c and b'write' in c[2]}
    missing = write_cmds - {cmd for cmd, _, _, _ in ROWS} - KNOWN_EXCLUSIONS
    env.assertEqual(missing, set(), message=f'write commands with no replication test: {missing}')
