"""Additional native-core regressions for the complete BF command surface."""
from support import *
from bloom import snapshot


class testNativeBloom:
    def __init__(self):
        self.env = Env()

    def test_partial_multi_write_persistence_and_watch(self):
        env = self.env
        env.cmd('BF.RESERVE', 'bf', 0.0001, 3, 'NONSCALING')
        watcher = Client(env.server.path)
        try:
            watcher.command('WATCH', 'bf')
            reply = env.cmd('BF.INSERT', 'bf', 'ITEMS', 'a', 'b', 'c', 'd', 'e')
            env.assertEqual([1, 1, 1], reply[:3])
            env.assertEqual(4, len(reply))
            env.assertIsInstance(reply[3], ResponseError)
            env.assertEqual('non scaling filter is full', str(reply[3]))
            watcher.command('MULTI')
            watcher.command('PING')
            env.assertIsNone(watcher.command('EXEC'))
            # A failed NOCREATE must not create a key or dirty the database.
            env.cmd('SAVE')
            env.expect('BF.INSERT missing NOCREATE ITEMS item').error().contains('not found')
            env.assertEqual(0, env.cmd('INFO', 'persistence')['rdb_changes_since_last_save'])
            env.assertEqual(0, env.cmd('EXISTS', 'missing'))
            env.dumpAndReload()
            env.assertEqual(3, env.cmd('BF.CARD', 'bf'))
            env.assertEqual([1, 1, 1], env.cmd('BF.MEXISTS', 'bf', 'a', 'b', 'c'))
        finally:
            watcher.close()

    def test_multi_write_aof(self):
        for preamble in ('yes', 'no'):
            env = Env(extra=('--appendonly', 'yes', '--appendfsync', 'always',
                             '--aof-use-rdb-preamble', preamble))
            env.cmd('BF.INSERT', 'bf', 'CAPACITY', 2, 'ERROR', 0.001, 'ITEMS', 'a', 'b')
            env.cmd('BF.MADD', 'bf', 'c', 'd', 'a')
            expected = snapshot(env.server.client, 'bf')
            # Restart directly from the command AOF (rewrite is covered separately).
            env.server.stop()
            env.server.start()
            env.assertEqual(expected, snapshot(env.server.client, 'bf'))
            env.assertEqual([1, 1, 1, 1], env.cmd('BF.MEXISTS', 'bf', 'a', 'b', 'c', 'd'))

    def test_binary_options_and_items(self):
        env = self.env
        for token in (b'ITEMS\x00', b'ERROR\x00', b'', b'I', b'N', b'NONSCALING\x00'):
            env.expect('BF.INSERT', 'bf', token, 'a').error()
            env.assertEqual(0, env.cmd('EXISTS', 'bf'))
        env.assertEqual([1, 1, 0], env.cmd('BF.INSERT', 'bf', 'iTeMs', b'', b'a\x00\xff', b''))
        env.assertEqual([1, 1], env.cmd('BF.MEXISTS', 'bf', b'', b'a\x00\xff'))
        env.cmd('SET', 'wrong', 'string')
        env.assertEqual([0, 0], env.cmd('BF.MEXISTS', 'wrong', 'a', 'b'))
        for command in ('BF.MADD', 'BF.INSERT'):
            args = ('ITEMS', 'a') if command == 'BF.INSERT' else ('a',)
            env.expect(command, 'wrong', *args).error().contains('WRONGTYPE')

    def test_resp3_info_fields_and_partial_errors(self):
        env = Env(protocol=3)
        env.cmd('BF.RESERVE', 'bf', 0.001, 1, 'NONSCALING')
        reply = env.cmd('BF.MADD', 'bf', 'a', 'b', 'c')
        env.assertIs(reply[0], True)
        env.assertEqual(2, len(reply))
        env.assertIsInstance(reply[1], ResponseError)
        info = env.cmd('BF.INFO', 'bf')
        for field, label in (('CAPACITY', b'Capacity'), ('SIZE', b'Size'),
                             ('FILTERS', b'Number of filters'), ('ITEMS', b'Number of items inserted'),
                             ('EXPANSION', b'Expansion rate')):
            env.assertEqual({label: info[label]}, env.cmd('BF.INFO', 'bf', field))
        env.assertIsNone(info[b'Expansion rate'])
        # Embedded errors must not leave unread replies on the connection.
        env.assertTrue(env.cmd('PING'))

    def test_full_command_oracle(self):
        if not os.environ.get('BLOOM_ORACLE_SERVER') or not os.environ.get('BLOOM_ORACLE_MODULE'):
            self.env.skip('set BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE for differential tests')
        oracle = Server(binary=os.environ['BLOOM_ORACLE_SERVER'],
                        extra=('--loadmodule', os.environ['BLOOM_ORACLE_MODULE']))
        def result(client, command):
            try:
                value = client.command(*command)
            except ResponseError as error:
                return ('error', str(error))
            def normalize(v):
                if isinstance(v, ResponseError):
                    return ('error', str(v))
                if isinstance(v, list):
                    return [normalize(x) for x in v]
                return v
            return normalize(value)
        try:
            native = self.env.server.client
            commands = [
                ('BF.CARD', 'missing'), ('BF.MEXISTS', 'missing', 'x', 'y'),
                ('BF.RESERVE', 'bf', 0.001, 2),
                ('BF.MADD', 'bf', 'a', 'b', 'a'),
                ('BF.INSERT', 'bf', 'NOCREATE', 'ITEMS', 'c', 'd', 'a'),
                ('BF.MEXISTS', 'bf', 'a', 'b', 'c', 'd', 'absent'),
                ('BF.INFO', 'bf'), ('BF.DEBUG', 'bf'), ('BF.CARD', 'bf'),
                ('BF.INSERT', 'ns', 'CAPACITY', 2, 'NONSCALING', 'ITEMS', 'a', 'b', 'c', 'd'),
                ('BF.MADD', 'ns', 'a', 'b', 'c'), ('BF.INFO', 'ns'),
                ('BF.INSERT', 'missing', 'NOCREATE', 'ITEMS', 'a'),
            ]
            commands += [('BF.MADD', 'bf', *range(start, start + 50)) for start in range(0, 500, 50)]
            commands += [('BF.INFO', 'bf'), ('BF.DEBUG', 'bf')]
            commands += [('BF.INFO', 'bf', field) for field in ('CAPACITY', 'SIZE', 'FILTERS', 'ITEMS', 'EXPANSION')]
            for protocol in (2, 3):
                for client in (native, oracle.client):
                    client.command('HELLO', protocol)
                    client.command('FLUSHALL')
                for command in commands:
                    self.env.assertEqual(result(oracle.client, command), result(native, command), command)
                self.env.assertEqual(snapshot(oracle.client, 'bf'), snapshot(native, 'bf'))
        finally:
            oracle.close()
