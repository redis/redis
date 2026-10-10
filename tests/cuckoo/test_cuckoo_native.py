"""Native Cuckoo lifecycle, persistence, configuration and compatibility."""
import os
import time
from support import *

def chunks(r, key):
    pos = 0
    result = []
    while True:
        pos, data = r('CF.SCANDUMP', key, pos)
        if not pos:
            return result
        result.append((pos, data))

class testNativeCuckoo:
    def test_lifecycle(self):
        env = Env()
        r = env.cmd
        r('CF.RESERVE', 'cf', 100)
        r('CF.INSERT', 'cf', 'ITEMS', b'a\x00b', b'a\x00b', 'other')
        env.assertEqual(b'cuckoo', r('TYPE', 'cf'))
        env.assertEqual([b'0', [b'cf']], r('SCAN', 0, 'TYPE', 'cuckoo'))
        env.assertEqual(1, r('COPY', 'cf', 'copy'))
        env.assertEqual(r('DUMP', 'cf'), r('DUMP', 'copy'))
        r('CF.DEL', 'copy', b'a\x00b')
        env.assertEqual(2, r('CF.COUNT', 'cf', b'a\x00b'))
        env.assertEqual(1, r('CF.COUNT', 'copy', b'a\x00b'))
        env.dumpAndReload()
        env.assertEqual(2, r('CF.COUNT', 'cf', b'a\x00b'))
        env.assertEqual(1, r('UNLINK', 'copy'))
        r('PEXPIRE', 'cf', 1)
        time.sleep(.02)
        env.assertEqual(0, r('EXISTS', 'cf'))

    def test_aof(self):
        for preamble in ('no', 'yes'):
            s = Server(extra=('--appendonly', 'yes', '--aof-use-rdb-preamble', preamble))
            try:
                r = s.client.command
                r('CF.RESERVE', 'cf', 4, 'EXPANSION', 2)
                for i in range(100):
                    r('CF.ADD', 'cf', i)
                r('BGREWRITEAOF')
                for _ in range(400):
                    info = r('INFO', 'persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info:
                        break
                    time.sleep(.025)
                else:
                    raise AssertionError('AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
                r('CF.ADD', 'cf', 'after')
                r('CONFIG', 'SET', 'cf-max-expansions', 1)
                try:
                    r('CF.ADD', 'empty', 'not-inserted')
                except ResponseError:
                    pass
                else:
                    raise AssertionError('Expected expansion-limit error')
                s.stop()
                s.start()
                assert s.client.command('CF.COUNT', 'empty', 'not-inserted') == 0
                for item in [*range(100), 'after']:
                    assert s.client.command('CF.EXISTS', 'cf', item) == 1
            finally:
                s.close()

    def test_config_acl_notifications(self):
        env = Env()
        r = env.cmd
        defaults = {'cf-initial-size':1024, 'cf-bucket-size':2, 'cf-max-iterations':20,
                    'cf-expansion-factor':1, 'cf-max-expansions':32}
        for key, value in defaults.items():
            env.assertEqual([key.encode(), str(value).encode()], r('CONFIG', 'GET', key))
        r('CONFIG', 'SET', 'cf-initial-size', 32, 'cf-bucket-size', 4,
          'cf-max-iterations', 30, 'cf-expansion-factor', 2)
        r('CF.ADD', 'cf', 'a')
        info = dict(zip(*[iter(r('CF.INFO', 'cf'))]*2))
        env.assertEqual(4, info[b'Bucket size'])
        env.assertEqual(2, info[b'Expansion rate'])
        env.assertEqual(30, info[b'Max iterations'])
        env.assertEqual(14, len(r('ACL', 'CAT', 'cuckoo')))
        env.assertEqual([b'cf'], r('COMMAND', 'GETKEYS', 'CF.INSERT', 'cf', 'ITEMS', 'a', 'b'))
        r('CONFIG', 'SET', 'notify-keyspace-events', 'EA')
        listener = Client(env.server.path)
        try:
            listener.command('SUBSCRIBE', '__keyevent@0__:cf.add')
            r('CF.ADD', 'cf', 'b')
            env.assertEqual([b'message', b'__keyevent@0__:cf.add', b'cf'], listener.read())
        finally:
            listener.close()
        watcher = Client(env.server.path)
        try:
            watcher.command('WATCH', 'cf')
            r('CF.DEL', 'cf', 'a')
            watcher.command('MULTI')
            watcher.command('PING')
            env.assertIsNone(watcher.command('EXEC'))
        finally:
            watcher.close()
        r('ACL', 'SETUSER', 'reader', 'on', '>secret', '~cf', '+@cuckoo', '-@write')
        r('AUTH', 'reader', 'secret')
        env.assertEqual(1, r('CF.EXISTS', 'cf', 'b'))
        env.expect('CF.ADD', 'cf', 'c').error().contains('NOPERM')
        r('AUTH', 'default', '')

    def test_external_compatibility(self):
        env = Env()
        binary = os.environ.get('BLOOM_ORACLE_SERVER')
        module = os.environ.get('BLOOM_ORACLE_MODULE')
        if not binary or not module:
            env.skip('Set BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE for compatibility')
        oracle = Server(binary=binary, extra=('--loadmodule', module))
        try:
            for r in (env.cmd, oracle.client.command):
                r('CF.RESERVE', 'cf', 4, 'EXPANSION', 2)
                for i in range(200):
                    r('CF.ADD', 'cf', i)
                r('CF.DEL', 'cf', 10)
                r('CF.COMPACT', 'cf')
            env.assertEqual(chunks(env.cmd, 'cf'), chunks(oracle.client.command, 'cf'))
            native = env.cmd('DUMP', 'cf')
            external = oracle.client.command('DUMP', 'cf')
            env.cmd('RESTORE', 'copy', 0, external)
            oracle.client.command('RESTORE', 'copy', 0, native)
            for r in (env.cmd, oracle.client.command):
                for i in range(200):
                    if i != 10:
                        env.assertEqual(1, r('CF.EXISTS', 'copy', i))
        finally:
            oracle.close()
