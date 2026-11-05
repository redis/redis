import time
from common import ResponseError

WAIT_TIMEOUT_MS = 1000
NO_DUMP_COMPARE = set()

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
