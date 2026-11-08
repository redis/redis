# RED-213286: BLESS (Redis 8.12) marks a key NO-EVICT so maxmemory eviction never
# picks it. The flag lives outside the module's value, so every module path that
# writes, replaces, saves or reloads a value has to keep it (RED-220725: TDIGEST.MERGE
# into an existing key replaced the value and dropped the flag).

import time
from common import *
from test_replication import ROWS, _exec

# Write commands that only create a key fail on an existing one, so there is no
# blessed key for them to write to.
CREATE_ONLY = {'bf.reserve', 'bf.loadchunk'}
TYPE_KEYS = [('bf', ['BF.ADD', 'bf', 'a'])]


def _skip_without_bless(env):
    env.skipOnCluster()  # BLESS state and INFO blessed_keys are per shard
    try:
        env.cmd('BLESS', 'GET', 'missing')
    except ResponseError as e:
        if 'unknown command' in str(e):
            env.skip()


def _create_blessed_keys(env):
    for key, create in TYPE_KEYS:
        env.cmd(*create)
        env.assertEqual(env.cmd('BLESS', 'SET', key, 'NO-EVICT'), 1, message=key)
    env.cmd('BF.ADD', 'plain', 'a')  # never blessed


def _assert_blessed_keys(env, when):
    for key, _ in TYPE_KEYS:
        env.assertEqual(env.cmd('BLESS', 'GET', key), [b'NO-EVICT'], message=f'{when}: {key}')
    env.assertEqual(env.cmd('BLESS', 'GET', 'plain'), [], message=f'{when}: plain')
    env.assertEqual(env.cmd('INFO', 'stats')['blessed_keys'], len(TYPE_KEYS), message=when)


def _wait_aof_rewrite(env, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        info = env.cmd('INFO', 'persistence')
        if not info['aof_rewrite_in_progress'] and not info['aof_rewrite_scheduled']:
            env.assertEqual(info['aof_last_bgrewrite_status'], 'ok')
            return
        time.sleep(0.1)
    env.assertTrue(False, message='AOF rewrite did not finish')


def testWriteCommandsKeepNoEvict(env):
    """Bless the key every write command touches, then run the command on it again."""
    _skip_without_bless(env)
    con = env.getConnection()
    rows = ROWS
    for cmd, setup, write, _ in rows:
        if cmd in CREATE_ONLY:
            continue
        con.execute_command('FLUSHALL')
        if setup is not None:
            for spec in (setup if isinstance(setup, list) else [setup]):
                _exec(con, spec)
        _exec(con, write)  # creates k when the setup didn't
        env.assertEqual(con.execute_command('BLESS', 'SET', 'k', 'NO-EVICT'), 1, message=cmd)
        _exec(con, write)
        env.assertEqual(con.execute_command('BLESS', 'GET', 'k'), [b'NO-EVICT'], message=cmd)
        env.assertEqual(con.execute_command('INFO', 'stats')['blessed_keys'], 1, message=cmd)


def testNoEvictSurvivesRdbAndAofRewrite(env):
    _skip_without_bless(env)
    _create_blessed_keys(env)

    env.cmd('DEBUG', 'RELOAD')
    _assert_blessed_keys(env, 'DEBUG RELOAD')

    # Without the RDB preamble the rewrite emits each type's aof_rewrite callback
    # output followed by BLESS SET.
    env.cmd('CONFIG', 'SET', 'aof-use-rdb-preamble', 'no')
    env.cmd('CONFIG', 'SET', 'appendonly', 'yes')
    _wait_aof_rewrite(env)
    env.cmd('BGREWRITEAOF')
    _wait_aof_rewrite(env)
    env.cmd('DEBUG', 'LOADAOF')
    _assert_blessed_keys(env, 'AOF rewrite')


def testBlessedKeysSurviveEviction(env):
    _skip_without_bless(env)
    env.cmd('CONFIG', 'SET', 'maxmemory-policy', 'allkeys-lru')
    _create_blessed_keys(env)

    # Under --use-slaves the 1MB replication backlog counts toward maxmemory, so the
    # headroom has to exceed it or eviction runs out of unblessed keys and fails with OOM.
    env.cmd('CONFIG', 'SET', 'maxmemory', env.cmd('INFO', 'memory')['used_memory'] + 4000000)
    with env.getConnection().pipeline(transaction=False) as pipe:
        for i in range(4000):
            pipe.set(f'flood:{i}', 'x' * 2000)
        pipe.execute()
    env.cmd('CONFIG', 'SET', 'maxmemory', 0)

    env.assertGreater(env.cmd('INFO', 'stats')['evicted_keys'], 0)
    for key, _ in TYPE_KEYS:
        env.assertEqual(env.cmd('EXISTS', key), 1, message=key)
