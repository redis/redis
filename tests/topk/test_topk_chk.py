"""Opt-in CHK keys must retain their engine independently of server defaults."""
import struct
import tempfile
import time

from support import *
from legacy_upgrade import upgrade
from rdb_corruption_utils import crc64_redis, encode_len, load_len


CONFIG = 'topk-default-algorithm'
CHK = 'cuckoo-heavykeeper'
LEGACY = 'heavykeeper'


def version(payload):
    return load_len(payload, 1)[0] & 1023


def checked_payload(body, trailer):
    value = body + trailer
    return value + struct.pack('<Q', crc64_redis(value))


def chk_fields(payload):
    """Read our uncompressed test payload; return editable logical fields."""
    module_id, _, pos, _ = load_len(payload, 1)
    fields = []
    while payload[pos] != 0:
        opcode, _, pos, _ = load_len(payload, pos)
        if opcode == 2:
            value, _, pos, _ = load_len(payload, pos)
        elif opcode == 4:
            value = struct.unpack_from('<d', payload, pos)[0]
            pos += 8
        elif opcode == 5:
            length, encoded, pos, _ = load_len(payload, pos)
            assert not encoded, 'test uses uncompressed, nonnumeric strings'
            value = payload[pos:pos + length]
            pos += length
        else:
            raise AssertionError(opcode)
        fields.append((opcode, value))
    return module_id, fields


def chk_payload(payload, fields, module_id=None):
    if module_id is None:
        module_id = load_len(payload, 1)[0]
    body = payload[:1] + encode_len(module_id)
    for opcode, value in fields:
        body += encode_len(opcode)
        if opcode == 2:
            body += encode_len(value)
        elif opcode == 4:
            body += struct.pack('<d', value)
        else:
            body += encode_len(len(value)) + value
    return checked_payload(body + b'\x00', payload[-10:-8])


def wait_aof(r):
    for _ in range(800):
        info = r('INFO', 'persistence')
        if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info:
            assert b'aof_last_bgrewrite_status:ok' in info
            return
        time.sleep(.025)
    raise AssertionError('AOF rewrite timed out')


class testTopKChk:
    def test_config_file_and_rewrite(self):
        with tempfile.TemporaryDirectory(prefix='topk-config-') as directory:
            config = Path(directory) / 'redis.conf'
            config.write_text('topk-default-algorithm heavykeeper\n')
            s = Server(configfile=config)
            try:
                s.client.command('CONFIG', 'SET', CONFIG, CHK)
                s.client.command('CONFIG', 'REWRITE')
                assert 'topk-default-algorithm cuckoo-heavykeeper' in config.read_text()
                s.stop()
                s.start()
                r = s.client.command
                assert r('CONFIG', 'GET', CONFIG) == [CONFIG.encode(), CHK.encode()]
                r('TOPK.RESERVE', 'new', 3)
                assert version(r('DUMP', 'new')) == 1
            finally:
                s.close()

    def test_opt_in_only_new_keys(self):
        env = Env()
        r = env.cmd
        env.assertEqual([CONFIG.encode(), LEGACY.encode()], r('CONFIG', 'GET', CONFIG))
        r('TOPK.RESERVE', 'old', 3)
        r('TOPK.INCRBY', 'old', 'existing', 7)
        before = r('DUMP', 'old')
        r('CONFIG', 'SET', CONFIG, CHK)
        env.assertEqual(before, r('DUMP', 'old'))
        r('TOPK.RESERVE', 'new', 3)
        env.assertEqual(0, version(before))
        env.assertEqual(1, version(r('DUMP', 'new')))
        env.assertEqual([b'k', 3, b'buckets', 32, b'decay', b'1.08',
                         b'algorithm', CHK.encode()], r('TOPK.INFO', 'new'))
        r('TOPK.INCRBY', 'old', 'existing', 4)
        env.assertEqual([11], r('TOPK.COUNT', 'old', 'existing'))
        r('CONFIG', 'SET', CONFIG, LEGACY)
        r('TOPK.RESERVE', 'later', 3)
        env.assertEqual(0, version(r('DUMP', 'later')))
        env.assertEqual(1, version(r('DUMP', 'new')))
        env.assertEqual(b'topk', r('TYPE', 'new'))
        env.expect('TOPK.RESERVE', 'old', 3).error().contains('already exists')
        env.expect('CONFIG', 'SET', CONFIG, 'unknown').error()

    def test_reserve_validation_and_resp3(self):
        env = Env(extra=('--topk-default-algorithm', CHK), protocol=3)
        r = env.cmd
        for k in (0, -1, 'x', 134217729, 2147483647, 2**64):
            env.expect('TOPK.RESERVE', 'invalid', k).error()
        env.expect('TOPK.RESERVE', 'invalid', 3, 8, 7, .9).error().contains('only supported by heavykeeper')
        env.assertEqual(0, r('EXISTS', 'invalid'))
        r('TOPK.RESERVE', 't', 3)
        r('TOPK.ADD', 't', 'a')
        env.assertEqual({b'k': 3, b'buckets': 32, b'decay': 1.08,
                         b'algorithm': CHK.encode()}, r('TOPK.INFO', 't'))
        env.assertEqual([True, False], r('TOPK.QUERY', 't', 'a', 'missing'))
        env.expect('TOPK.LIST', 't', 'bad').error()
        env.expect('TOPK.INCRBY', 't', 'unpaired').error()
        r('SET', 'string', 'v')
        env.expect('TOPK.ADD', 'string', 'a').error().contains('WRONGTYPE')
        env.expect('TOPK.QUERY', 'missing', 'a').error().contains('does not exist')

    def test_commands_binary_count_outside_heap_and_zero(self):
        env = Env(extra=('--topk-default-algorithm', CHK))
        r = env.cmd
        r('TOPK.RESERVE', 't', 1)
        env.assertEqual([None], r('TOPK.INCRBY', 't', 'zero', 0))
        env.assertEqual([], r('TOPK.LIST', 't'))
        env.assertEqual([0], r('TOPK.QUERY', 't', 'zero'))
        env.assertEqual([None], r('TOPK.INCRBY', 't', b'a\x00b', 10))
        env.assertEqual([None], r('TOPK.INCRBY', 't', 'light', 1))
        env.assertEqual([0], r('TOPK.QUERY', 't', 'light'))
        env.assertEqual([1], r('TOPK.COUNT', 't', 'light'))
        env.assertEqual([b'a\x00b'], r('TOPK.INCRBY', 't', '', 100))
        env.assertEqual([b'', 100], r('TOPK.LIST', 't', 'WITHCOUNT'))
        env.assertEqual([1], r('TOPK.QUERY', 't', ''))
        result = r('TOPK.INCRBY', 't', '', 5, 'bad', -1, 'too-big', 100001, '', 5)
        env.assertTrue(isinstance(result[1], ResponseError))
        env.assertTrue(isinstance(result[2], ResponseError))
        env.assertEqual([110], r('TOPK.COUNT', 't', ''))

    def test_copy_restore_memory_digest_expiry_unlink(self):
        env = Env(extra=('--topk-default-algorithm', CHK, '--key-memory-histograms', 'yes'))
        r = env.cmd
        r('TOPK.RESERVE', 't', 3)
        empty_size = r('MEMORY', 'USAGE', 't')
        r('TOPK.INCRBY', 't', b'binary\x00item', 3, b'', 2, b'x' * 1000, 1)
        env.assertGreater(r('MEMORY', 'USAGE', 't'), empty_size)
        before = r('DUMP', 't')
        env.assertEqual(1, r('COPY', 't', 'copy'))
        env.assertEqual(before, r('DUMP', 'copy'))
        r('CONFIG', 'SET', CONFIG, LEGACY)
        r('RESTORE', 'restored', 0, before)
        env.assertEqual(before, r('DUMP', 'restored'))
        env.assertEqual(r('DEBUG', 'DIGEST-VALUE', 't'), r('DEBUG', 'DIGEST-VALUE', 'copy'))
        r('CONFIG', 'SET', 'rdbcompression', 'no')
        env.assertEqual(r('DEBUG', 'DIGEST-VALUE', 't'), r('DEBUG', 'DIGEST-VALUE', 'copy'))
        r('TOPK.INCRBY', 'copy', b'binary\x00item', 4)
        env.assertEqual([3], r('TOPK.COUNT', 't', b'binary\x00item'))
        env.assertEqual([7], r('TOPK.COUNT', 'copy', b'binary\x00item'))
        r('RENAME', 'restored', 'renamed')
        r('MOVE', 'renamed', 1)
        r('SELECT', 1)
        env.assertEqual(1, version(r('DUMP', 'renamed')))
        r('PEXPIRE', 'renamed', 1)
        time.sleep(.02)
        env.assertEqual(0, r('EXISTS', 'renamed'))
        r('SELECT', 0)
        env.assertEqual(2, r('UNLINK', 'copy', 't'))
        env.assertEqual(0, r('DBSIZE'))

    def test_collision_heavy_roundtrips(self):
        env = Env(extra=('--topk-default-algorithm', CHK))
        r = env.cmd
        r('TOPK.RESERVE', 't', 2)
        for batch in range(10):
            for i in range(100):
                r('TOPK.INCRBY', 't', 'item-' + str((i * 17 + batch * 13) % 251),
                  1 + (i * 83 + batch) % 1000)
            before = r('DUMP', 't')
            r('RESTORE', 'copy', 0, before, 'REPLACE')
            env.assertEqual(before, r('DUMP', 'copy'))
            listing = r('TOPK.LIST', 'copy', 'WITHCOUNT')
            env.assertGreaterEqual(listing[1], listing[3])
            env.assertNotEqual(listing[0], listing[2])

    def test_active_defrag_preserves_both_engines(self):
        env = Env(extra=('--key-memory-histograms', 'yes'))
        r = env.cmd
        try:
            r('CONFIG', 'SET', 'activedefrag', 'yes')
        except ResponseError as error:
            if 'cannot be enabled' in str(error):
                env.skip('requires jemalloc defrag or DEBUG_DEFRAG=force')
            raise
        r('CONFIG', 'SET', 'activedefrag', 'no')
        for i in range(600):
            r('CONFIG', 'SET', CONFIG, CHK if i % 2 else LEGACY)
            r('TOPK.RESERVE', 't-' + str(i), 3)
            r('TOPK.INCRBY', 't-' + str(i), b'long\x00' + b'x' * 1500, 5, '', 3)
        keys = []
        for i in range(600):
            if i % 4 < 2: r('DEL', 't-' + str(i))
            else: keys.append('t-' + str(i))
        before = [r('DUMP', k) for k in keys]
        r('CONFIG', 'SET', 'hz', 100, 'active-defrag-ignore-bytes', 1,
          'active-defrag-threshold-lower', 1, 'active-defrag-threshold-upper', 1,
          'active-defrag-cycle-min', 25, 'active-defrag-cycle-max', 75)
        r('CONFIG', 'SET', 'activedefrag', 'yes')
        for _ in range(800):
            stats = r('INFO', 'stats')
            if stats['active_defrag_key_hits'] + stats['active_defrag_key_misses'] >= len(keys):
                break
            time.sleep(.025)
        else: env.fail('Active defrag did not scan the remaining keys')
        r('CONFIG', 'SET', 'activedefrag', 'no')
        env.assertGreater(stats['active_defrag_hits'], 0)
        env.assertEqual(before, [r('DUMP', k) for k in keys])
        for k in keys:
            r('TOPK.ADD', k, 'after-defrag')
            r('MEMORY', 'USAGE', k)
        r('FLUSHALL')

    def test_mixed_rdb_restart_uses_saved_engine_not_config(self):
        env = Env()
        r = env.cmd
        keys = []
        for engine in (LEGACY, CHK):
            r('CONFIG', 'SET', CONFIG, engine)
            for suffix in ('empty', 'populated'):
                key = engine + suffix
                keys.append(key)
                r('TOPK.RESERVE', key, 3)
                if suffix == 'populated': r('TOPK.INCRBY', key, b'a\x00b', 7, '', 2)
                r('PEXPIRE', key, 600000)
        before = [(r('DUMP', k), r('PEXPIRETIME', k)) for k in keys]
        for default in (CHK, LEGACY):
            r('SAVE')
            env.server.stop()
            env.server.extra = ('--topk-default-algorithm', default)
            env.server.start()
            env.assertEqual(before, [(r('DUMP', k), r('PEXPIRETIME', k)) for k in keys])
        for key in keys:
            r('TOPK.INCRBY', key, 'after-restart', 9)
            env.assertEqual([9], r('TOPK.COUNT', key, 'after-restart'))

    def test_replication_different_defaults_empty_and_randomized(self):
        env = Env(useSlaves=True)
        r = env.cmd
        replica = env.getSlaveConnection().execute_command
        for engine, opposite in ((CHK, LEGACY), (LEGACY, CHK)):
            r('CONFIG', 'SET', CONFIG, engine)
            replica('CONFIG', 'SET', CONFIG, opposite)
            r('TOPK.RESERVE', engine, 2)
            env.assertEqual(1, r('WAIT', 1, 5000))
            env.assertEqual(r('DUMP', engine), replica('DUMP', engine))
            r('PEXPIRE', engine, 600000)
            for i in range(100):
                r('TOPK.INCRBY', engine, 'item-' + str(i % 71), 1 + i % 19)
            result = r('TOPK.INCRBY', engine, '', 5, 'bad', -1, 'last', 2)
            env.assertTrue(isinstance(result[1], ResponseError))
            env.assertEqual(1, r('WAIT', 1, 5000))
            env.assertEqual(r('DUMP', engine), replica('DUMP', engine))
            env.assertEqual(r('PEXPIRETIME', engine), replica('PEXPIRETIME', engine))
        # Discard replica state and force a full RDB synchronization as well.
        replica('REPLICAOF', 'NO', 'ONE')
        replica('FLUSHALL')
        replica('REPLICAOF', '127.0.0.1', env.server.extra[-1])
        for _ in range(800):
            if b'master_link_status:up' in replica('INFO', 'replication'):
                break
            time.sleep(.025)
        else: raise AssertionError('Full resync timed out')
        env.assertEqual(1, r('WAIT', 1, 5000))
        for engine in (LEGACY, CHK):
            env.assertEqual(r('DUMP', engine), replica('DUMP', engine))

    def test_aof_append_and_both_rewrite_modes(self):
        for preamble in ('no', 'yes'):
            env = Env(extra=('--appendonly', 'yes', '--aof-use-rdb-preamble', preamble))
            s = env.server
            r = env.cmd
            keys = []
            for engine in (LEGACY, CHK):
                r('CONFIG', 'SET', CONFIG, engine)
                for suffix in ('empty', 'populated'):
                    key = engine + suffix
                    keys.append(key)
                    r('TOPK.RESERVE', key, 2)
                    if suffix == 'populated': r('TOPK.ADD', key, b'a\x00b', '')
            for rewrite in (False, True):
                if rewrite:
                    r('BGREWRITEAOF')
                    wait_aof(s.client.command)
                    for engine in (LEGACY, CHK):
                        r('TOPK.INCRBY', engine + 'populated', 'post-rewrite', 100)
                before = [r('DUMP', k) for k in keys]
                s.stop()
                s.extra = (*s.extra, '--topk-default-algorithm', LEGACY if rewrite else CHK)
                s.start()
                env.assertEqual(before, [r('DUMP', k) for k in keys])

    def test_historical_command_aof_stays_legacy_with_opt_in(self):
        env = Env(extra=('--appendonly', 'yes', '--aof-use-rdb-preamble', 'no'))
        s = env.server
        s.stop()
        commands = [('TOPK.RESERVE', 'old-empty', '3'),
                    ('TOPK.RESERVE', 'old', '3'),
                    ('TOPK.INCRBY', 'old', 'existing', '7')]
        data = b''
        for command in commands:
            data += b'*%d\r\n' % len(command)
            for arg in command:
                arg = arg.encode()
                data += b'$%d\r\n' % len(arg) + arg + b'\r\n'
        incremental = list((s.directory / 'appendonlydir').glob('*.incr.aof'))
        env.assertEqual(1, len(incremental))
        incremental[0].write_bytes(data)
        s.extra = (*s.extra, '--topk-default-algorithm', CHK)
        s.start()
        for key in ('old', 'old-empty'):
            env.assertEqual(0, version(env.cmd('DUMP', key)))
        env.assertEqual([7], env.cmd('TOPK.COUNT', 'old', 'existing'))
        env.cmd('TOPK.RESERVE', 'new', 3)
        env.assertEqual(1, version(env.cmd('DUMP', 'new')))

    def test_reject_corrupt_chk_payloads_without_replacing_key(self):
        env = Env(extra=('--topk-default-algorithm', CHK))
        r = env.cmd
        r('CONFIG', 'SET', 'rdbcompression', 'no')
        r('TOPK.RESERVE', 't', 1)
        r('TOPK.INCRBY', 't', 'item', 5)
        original = r('DUMP', 't')
        module_id, fields = chk_fields(original)
        env.assertEqual(original, chk_payload(original, fields))
        corrupt = []
        for index, value in ((0, 99), (1, 0), (1, 2**32), (2, 0), (2, 3),
                             (2, 2**31), (3, float('nan')), (3, float('inf')),
                             (3, .9), (3, 2.), (4, 2**63)):
            changed = list(fields)
            changed[index] = (changed[index][0], value)
            corrupt.append(chk_payload(original, changed))
        for index in (5, 6):
            changed = list(fields)
            changed[index] = (5, changed[index][1][:-1])
            corrupt.append(chk_payload(original, changed))
        for index, offset, patch in ((5, 2, struct.pack('<Q', 6)),
                                     (6, 0, b'\xff' * 8),
                                     (6, 8, struct.pack('<Q', 6)),
                                     (6, 16, b'\x02'), (6, 16, b'\x00')):
            changed = list(fields)
            blob = bytearray(changed[index][1])
            blob[offset:offset + len(patch)] = patch
            changed[index] = (5, bytes(blob))
            corrupt.append(chk_payload(original, changed))
        corrupt.append(chk_payload(original, fields, (module_id & ~1023) | 1023))
        corrupt.append(chk_payload(original, fields[:-1]))
        for payload in corrupt:
            env.expect('RESTORE', 't', 0, payload, 'REPLACE').error()
            env.assertEqual(original, r('DUMP', 't'))
            env.assertTrue(r('PING'))

    def test_64bit_counts_and_overflow_rejection(self):
        env = Env(extra=('--topk-default-algorithm', CHK))
        r = env.cmd
        r('CONFIG', 'SET', 'rdbcompression', 'no')
        r('TOPK.RESERVE', 't', 1)
        r('TOPK.INCRBY', 't', 'item', 5)
        original = r('DUMP', 't')
        _, fields = chk_fields(original)
        for count in (2**32 + 7, 2**63 - 2):
            changed = list(fields)
            changed[4] = (2, count)
            tables = bytearray(fields[5][1])
            for offset in range(0, len(tables), 23):
                for heavy in (offset + 2, offset + 12):
                    if struct.unpack_from('<Q', tables, heavy)[0]:
                        struct.pack_into('<Q', tables, heavy, count)
            changed[5] = (5, bytes(tables))
            heap = bytearray(fields[6][1])
            struct.pack_into('<Q', heap, 8, count)
            changed[6] = (5, bytes(heap))
            r('RESTORE', 'large', 0, chk_payload(original, changed), 'REPLACE')
            env.assertEqual([count], r('TOPK.COUNT', 'large', 'item'))
            r('TOPK.ADD', 'large', 'item')
            env.assertEqual([count + 1], r('TOPK.COUNT', 'large', 'item'))
            before = r('DUMP', 'large')
            r('RESTORE', 'copy', 0, before, 'REPLACE')
            env.assertEqual(before, r('DUMP', 'copy'))
            if count == 2**63 - 2:
                result = r('TOPK.INCRBY', 'large', 'item', 1, 'item', 0)
                env.assertTrue(isinstance(result[0], ResponseError))
                env.assertEqual(None, result[1])
                env.assertEqual(before, r('DUMP', 'large'))

    def test_reject_duplicate_names_and_invalid_heap_order(self):
        env = Env(extra=('--topk-default-algorithm', CHK))
        r = env.cmd
        r('CONFIG', 'SET', 'rdbcompression', 'no')
        r('TOPK.RESERVE', 't', 2)
        r('TOPK.INCRBY', 't', 'first', 3, 'second', 7)
        original = r('DUMP', 't')
        _, fields = chk_fields(original)
        duplicate = list(fields)
        heap = bytearray(fields[6][1])
        heap[17:34] = heap[:17]
        duplicate[6] = (5, bytes(heap))
        duplicate[8] = duplicate[7]
        env.expect('RESTORE', 'bad', 0, chk_payload(original, duplicate)).error()
        unordered = list(fields)
        heap = bytearray(fields[6][1])
        heap[:17], heap[17:34] = heap[17:34], heap[:17]
        unordered[6] = (5, bytes(heap))
        unordered[7], unordered[8] = unordered[8], unordered[7]
        env.expect('RESTORE', 'bad', 0, chk_payload(original, unordered)).error()
        env.assertEqual(0, r('EXISTS', 'bad'))
        env.assertEqual(original, r('DUMP', 't'))

    def test_old_module_upgrade_with_chk_default(self):
        def create(r):
            r('TOPK.RESERVE', 'subject', 3)
            r('TOPK.INCRBY', 'subject', 'existing', 7)
            r('TOPK.RESERVE', 'empty', 3)

        def snapshot(r):
            return [r('TOPK.INFO', 'subject'), r('TOPK.LIST', 'subject', 'WITHCOUNT'),
                    r('TOPK.INFO', 'empty')]

        def mutate(r):
            assert version(r('DUMP', 'subject')) == 0
            assert version(r('DUMP', 'empty')) == 0
            r('TOPK.INCRBY', 'subject', 'existing', 3)
            assert r('TOPK.COUNT', 'subject', 'existing') == [10]
            r('TOPK.RESERVE', 'new', 3)
            assert version(r('DUMP', 'new')) == 1

        upgrade(Env(), create, snapshot, mutate,
                native_extra=('--topk-default-algorithm', CHK))
