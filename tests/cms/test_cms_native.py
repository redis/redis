"""Native CMS lifecycle, metadata, replication, and AOF regression tests."""
import os
import socket
import time
from support import *

class testNativeCMS:
    def test_lifecycle(self):
        env = Env()
        r = env.cmd
        r('CMS.INITBYDIM', 'cms', 100, 5)
        r('CMS.INCRBY', 'cms', b'a\x00b', 7)
        env.assertEqual(b'cms', r('TYPE', 'cms'))
        env.assertEqual([b'0', [b'cms']], r('SCAN', 0, 'TYPE', 'cms'))
        env.assertEqual(1, r('COPY', 'cms', 'copy'))
        env.assertEqual(r('DUMP', 'cms'), r('DUMP', 'copy'))
        r('CMS.INCRBY', 'copy', b'a\x00b', -2)
        env.assertEqual([7], r('CMS.QUERY', 'cms', b'a\x00b'))
        env.assertEqual([5], r('CMS.QUERY', 'copy', b'a\x00b'))
        r('SAVE')
        env.dumpAndReload()
        env.assertEqual([7], r('CMS.QUERY', 'cms', b'a\x00b'))
        env.assertEqual(1, r('UNLINK', 'copy'))
        r('PEXPIRE', 'cms', 1)
        time.sleep(.02)
        env.assertEqual(0, r('EXISTS', 'cms'))

    def test_metadata_resp3_acl_notifications(self):
        env = Env()
        r = env.cmd
        expected = [b'cms.initbydim', b'cms.initbyprob', b'cms.incrby',
                    b'cms.query', b'cms.merge', b'cms.info']
        env.assertEqual(sorted(expected), sorted(r('ACL', 'CAT', 'cms')))
        env.assertEqual([b'dst{t}', b'a{t}', b'b{t}'],
                        r('COMMAND', 'GETKEYS', 'CMS.MERGE', 'dst{t}', 2, 'a{t}', 'b{t}', 'WEIGHTS', 1, -1))
        r('CONFIG', 'SET', 'notify-keyspace-events', 'EA')
        listener = Client(env.server.path)
        try:
            listener.command('SUBSCRIBE', '__keyevent@0__:cms.incrby')
            r('CMS.INITBYDIM', 'cms', 100, 5)
            r('CMS.INCRBY', 'cms', 'a', 3)
            env.assertEqual([b'message', b'__keyevent@0__:cms.incrby', b'cms'], listener.read())
        finally:
            listener.close()
        r('HELLO', 3)
        env.assertEqual({b'width':100, b'depth':5, b'count':3, b'cell_size':4}, r('CMS.INFO', 'cms'))
        r('ACL', 'SETUSER', 'reader', 'on', '>secret', '~cms', '+@cms', '-@write')
        r('AUTH', 'reader', 'secret')
        env.assertEqual([3], r('CMS.QUERY', 'cms', 'a'))
        env.expect('CMS.INCRBY', 'cms', 'a', 1).error().contains('NOPERM')
        r('AUTH', 'default', '')

    def test_aof(self):
        for preamble in ('no', 'yes'):
            s = Server(extra=('--appendonly', 'yes', '--aof-use-rdb-preamble', preamble))
            try:
                r = s.client.command
                r('CMS.INITBYDIM', 'cms', 100, 5, 'CELL_SIZE', 1)
                r('CMS.INCRBY', 'cms', 'a', 5)
                r('BGREWRITEAOF')
                for _ in range(400):
                    info = r('INFO', 'persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info:
                        break
                    time.sleep(.025)
                else:
                    raise AssertionError('AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
                # Verify mixed success/error propagation after the rewrite too.
                r('CMS.INCRBY', 'cms', 'a', 300, 'b', 2, 'a', -1)
                s.stop()
                s.start()
                assert s.client.command('CMS.QUERY', 'cms', 'a', 'b') == [4, 2]
            finally:
                s.close()

    def test_replication(self):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        primary = Server(extra=('--bind', '127.0.0.1', '--port', str(port)))
        replica = Server()
        try:
            r = primary.client.command
            r('CMS.INITBYDIM', 'cms', 100, 5)
            r('CMS.INCRBY', 'cms', 'a', 3)
            replica.client.command('REPLICAOF', '127.0.0.1', port)
            for _ in range(600):
                if b'master_link_status:up' in replica.client.command('INFO', 'replication'):
                    break
                time.sleep(.025)
            else:
                raise AssertionError('Replication timed out')
            r('CMS.INCRBY', 'cms', 'a', 2)
            assert r('WAIT', 1, 5000) == 1
            assert replica.client.command('CMS.QUERY', 'cms', 'a') == [5]
        finally:
            replica.close()
            primary.close()

    def test_external_compatibility(self):
        env = Env()
        binary = os.environ.get('BLOOM_ORACLE_SERVER')
        module = os.environ.get('BLOOM_ORACLE_MODULE')
        if not binary or not module:
            env.skip('Set BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE for compatibility')
        oracle = Server(binary=binary, extra=('--loadmodule', module))
        try:
            for size in (1, 2, 4, 8):
                key = 'cms' + str(size)
                for r in (env.cmd, oracle.client.command):
                    r('CMS.INITBYDIM', key, 100, 5, 'CELL_SIZE', size)
                    r('CMS.INCRBY', key, b'a\x00b', 12, 'other', 3)
                # Compression and RDB versions are owned by the respective cores.
                native = env.cmd('DUMP', key)
                external = oracle.client.command('DUMP', key)
                env.cmd('RESTORE', key + '-copy', 0, external)
                oracle.client.command('RESTORE', key + '-copy', 0, native)
                for r in (env.cmd, oracle.client.command):
                    env.assertEqual([12,3], r('CMS.QUERY', key + '-copy', b'a\x00b', 'other'))
        finally:
            oracle.close()
