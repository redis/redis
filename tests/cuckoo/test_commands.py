import random
import time

from common import *


class testCuckoo():
    def __init__(self):
        self.env = Env(decodeResponses=True)
        self.assertOk = self.env.assertTrue
        self.cmd = self.env.cmd
        self.assertEqual = self.env.assertEqual
        self.assertRaises = self.env.assertRaises
        self.assertTrue = self.env.assertTrue
        self.assertAlmostEqual = self.env.assertAlmostEqual
        self.assertGreater = self.env.assertGreater
        self.restart_and_reload = self.env.restartAndReload
        self.assertResponseError = self.env.assertResponseError
        self.retry_with_rdb_reload = self.env.dumpAndReload
        self.assertNotEqual = self.env.assertNotEqual
        self.assertGreaterEqual = self.env.assertGreaterEqual

    def test_number_of_buckets_does_not_overflow(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.LOADCHUNK', 'cf',  '1',
                 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x01\x00\x01\x00")
        self.cmd('CF.INSERT', 'cf', 'ITEMS', '1', '1', '1', '1', '1', '1', '1', '1', '1', '1')

    def test_counter_overflow_with_large_number_of_items(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE', 'cf', 2, 'BUCKETSIZE', 1, 'EXPANSION', 1)
        try:
            self.cmd('CF.INSERT', 'cf', 'ITEMS', *["1 "]*140000)
        except ResponseError:
            pass

    def test_count(self):
        self.cmd('FLUSHALL')

        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE', 'cf', 'str')
        self.cmd('CF.RESERVE', 'cf', '1000')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE', 'cf', '1000')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE', 'tooSmall', '1')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERT', 'cf', 'CAPACITY', '-1', 'ITEMS', 'k0')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERT', 'cf', 'CAPACITY', '3', 'ITEMS', 'k0')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERTNX', 'cf', 'CAPACITY', '-1', 'ITEMS', 'k0')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERTNX', 'cf', 'CAPACITY', '3', 'ITEMS', 'k0')
        self.assertEqual(0, self.cmd('cf.exists', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.add', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.add', 'cf', 'k1'))

        self.assertEqual(1, self.cmd('cf.exists', 'cf', 'k1'))
        self.assertEqual(2, self.cmd('cf.count', 'cf', 'k1'))

        # Delete the item
        self.assertEqual(1, self.cmd('cf.del', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.count', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.del', 'cf', 'k1'))
        self.assertEqual(0, self.cmd('cf.count', 'cf', 'k1'))
        self.assertEqual(0, self.cmd('cf.del', 'cf', 'k1'))
        self.assertRaises(ResponseError, self.cmd, 'cf.del', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.del', 'bf', 'k1')

        for x in range(100):
            self.cmd('cf.add', 'nums', str(x))

        for x in range(100):
            self.assertEqual(1, self.cmd('cf.exists', 'nums', str(x)))

        # TODO: re-enable this portion after RLTest migration
        # for _ in self.retry_with_rdb_reload():
        #     for x in range(100):
        #         self.assertEqual(1, self.cmd('cf.exists', 'nums', str(x)))

    # TODO: re-enable this portion after RLTest migration
    # def test_aof(self):
    #     self.cmd('FLUSHALL')
    #     # Ensure we have a pretty small filter
    #     self.cmd('cf.reserve', 'smallCF', 4)
    #     for x in range(100):
    #         self.cmd('cf.add', 'smallCF', str(x))
    #     # Sanity check
    #     for x in range(100):
    #         self.assertEqual(1, self.cmd('cf.exists', 'smallCF', str(x)))

    #     self.restart_and_reload()
    #     for x in range(100):
    #         self.assertEqual(1, self.cmd('cf.exists', 'smallCF', str(x)))

    #     self.cmd('cf.reserve', 'smallCF2', 4, 'expansion', 2)
    #     for x in range(100):
    #         self.cmd('cf.add', 'smallCF2', str(x))
    #     # Sanity check
    #     for x in range(100):
    #         self.assertEqual(1, self.cmd('cf.exists', 'smallCF2', str(x)))

    # TODO: re-enable this portion after RLTest migration
    # self.restart_and_reload()
    # for x in range(100):
    #     self.assertEqual(1, self.cmd('cf.exists', 'smallCF2', str(x)))
    # self.assertEqual(580, self.cmd('MEMORY USAGE', 'smallCF'))
    # self.assertEqual(284, self.cmd('MEMORY USAGE', 'smallCF2'))

    def test_setnx(self):
        self.cmd('FLUSHALL')
        self.assertEqual(1, self.cmd('cf.addnx', 'cf', 'k1'))
        self.assertEqual(0, self.cmd('cf.addnx', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.count', 'cf', 'k1'))
        self.assertEqual(1, self.cmd('cf.add', 'cf', 'k1'))
        self.assertEqual(2, self.cmd('cf.count', 'cf', 'k1'))

    def test_insert(self):
        self.cmd('FLUSHALL')
        # Ensure insert with default capacity works
        self.assertEqual(1, self.cmd('cf.add', 'f1', 'foo'))
        self.assertRaises(ResponseError, self.cmd, 'cf.add', 'f1')
        self.assertEqual([1], self.cmd('cf.insert', 'f2', 'ITEMS', 'foo'))
        self.assertRaises(ResponseError, self.cmd, 'cf.insert', 'cf')
        d1 = self.cmd('cf.debug', 'f1')
        d2 = self.cmd('cf.debug', 'f2')
        self.assertRaises(ResponseError, self.cmd, 'cf.debug')
        self.assertRaises(ResponseError, self.cmd, 'cf.debug', 'noexist')
        self.assertTrue(d1)
        self.assertEqual(d1, d2)

        # Test NX
        self.assertEqual([0], self.cmd('cf.insertnx', 'f1', 'items', 'foo'))

        # Create a new filter with non-default capacity
        self.assertEqual([1], self.cmd('cf.insert', 'f3', 'CAPACITY', '10000', 'ITEMS', 'foo'))
        self.assertRaises(ResponseError, self.cmd, 'cf.insert', 'f3', 'NOCREATE', 'CAPACITY')
        self.assertRaises(ResponseError, self.cmd, 'cf.insert', 'f3', 'NOCREATE', 'CAPACITY', 'str')
        self.assertRaises(ResponseError, self.cmd, 'cf.insert', 'f3', 'NOCREATE', 'DONTEXIST')
        self.assertRaises(ResponseError, self.cmd, 'cf.insert', 'f3', 'NOCREATE', 'ITEMS')
        d3 = self.cmd('cf.debug', 'f3')
        self.assertEqual('bktsize:2 buckets:8192 items:1 deletes:0 filters:1 max_iterations:20 expansion:1', d3)
        self.assertNotEqual(d1, d3)

        # Test multi
        self.assertEqual([0, 1, 1], self.cmd('cf.insertnx', 'f3', 'ITEMS', 'foo', 'bar', 'baz'))

        # Test no auto creation
        with self.assertResponseError():
            self.cmd('cf.insert', 'f4', 'nocreate', 'items', 'foo')
        # Create it
        self.cmd('cf.insert', 'f4', 'items', 'foo')
        # Insert again to ensure our prior error was because of NOCREATE
        self.cmd('cf.insert', 'f4', 'nocreate', 'items', 'foo')

    def test_exists(self):
        self.cmd('FLUSHALL')
        self.assertEqual([1, 1, 1], self.cmd('CF.INSERT', 'f1', 'ITEMS', 'foo', 'bar', 'baz'))
        self.assertEqual([1, 1, 1], self.cmd('CF.MEXISTS', 'f1', 'foo', 'bar', 'baz'))

        # Test missing redis key
        self.assertEqual(0, self.cmd('CF.EXISTS', 'nonexist-key', 'blah'))
        self.assertEqual([0], self.cmd('CF.MEXISTS', 'nonexist-key', 'blah'))

        self.assertRaises(ResponseError, self.cmd, 'CF.MEXISTS', 'key')
        self.assertRaises(ResponseError, self.cmd, 'CF.MEXISTS')
        self.assertRaises(ResponseError, self.cmd, 'CF.EXISTS', 'key')
        self.assertRaises(ResponseError, self.cmd, 'CF.EXISTS')

    def test_mem_usage(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE', 'cf', '1000')
        yield 1
        self.env.dumpAndReload()
        yield 2
        if not VALGRIND:
            self.assertEqual(1112, self.cmd('MEMORY USAGE', 'cf'))
        self.cmd('cf.insert', 'cf', 'nocreate', 'items', 'foo')
        if not VALGRIND:
            self.assertEqual(1112, self.cmd('MEMORY USAGE', 'cf'))

    def test_max_iterations(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE a 10 MAXITERATIONS 10')
        d1 = self.cmd('cf.debug', 'a')
        self.assertEqual('bktsize:2 buckets:8 items:0 deletes:0 filters:1 max_iterations:10 expansion:1', d1)

        self.cmd('CF.RESERVE b 10')
        d2 = self.cmd('cf.debug', 'b')
        self.assertEqual('bktsize:2 buckets:8 items:0 deletes:0 filters:1 max_iterations:20 expansion:1', d2)

        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE a 10 MAXITERATIONS string')

    def test_num_deletes(self):
        # bgsave can happens in bgsave or aof rewrite or slave full sync.
        # in this case, the dympAndReload function is flacky and depends on the timing of the bgsave.
        # so we skip this test on slave.
        self.env.skipOnSlave()
        self.cmd('FLUSHALL')
        self.cmd('cf.add', 'nums', 'Redis')
        self.cmd('cf.del', 'nums', 'Redis')
        d1 = self.cmd('cf.debug', 'nums')
        self.env.dumpAndReload()
        # for _ in self.client.retry_with_rdb_reload():
        #     self.cmd('ping')
        d2 = self.cmd('cf.debug', 'nums')
        self.assertEqual(d1, d2)

    def test_compact(self):
        self.env = Env()
        self.cmd('FLUSHALL')
        q = 100
        self.cmd('CF.RESERVE cf 8 MAXITERATIONS 50')

        for x in range(q):
            self.cmd('cf.add cf', str(x))

        for x in range(q):
            self.assertEqual(1, self.cmd('cf.exists cf', str(x)))

        str1 = self.cmd('cf.debug cf')[49:52]
        self.assertGreaterEqual(str1, "130")  # In experiments was larger than 130
        self.assertEqual(self.cmd('cf.compact cf'), 'OK')
        str2 = self.cmd('cf.debug cf')[49:52]
        self.assertGreaterEqual(str1, str2)  # Expect to see reduction after compaction

        self.assertRaises(ResponseError, self.cmd, 'CF.COMPACT a')
        self.assertRaises(ResponseError, self.cmd, 'CF.COMPACT a b')
        self.env = Env(decodeResponses=True)


    def test_compact(self):
        self.env = Env()
        self.cmd('FLUSHALL')
        q = 100
        self.cmd('CF.RESERVE cf 8 MAXITERATIONS 50')

        for x in range(q):
            self.cmd('cf.add cf', str(x))

        for x in range(q):
            self.assertEqual(1, self.cmd('cf.exists cf', str(x)))

        str1 = self.cmd('cf.debug cf')[49:52]
        self.assertGreaterEqual(str1, "130")  # In experiments was larger than 130
        self.assertEqual(self.cmd('cf.compact cf'), 'OK')
        str2 = self.cmd('cf.debug cf')[49:52]
        self.assertGreaterEqual(str1, str2)  # Expect to see reduction after compaction

        self.assertRaises(ResponseError, self.cmd, 'CF.COMPACT a')
        self.assertRaises(ResponseError, self.cmd, 'CF.COMPACT a b')
        self.env = Env(decodeResponses=True)


    def test_max_expansions(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE', 'cf', '4')
        for i in range(124):
            self.assertEqual(1, self.cmd('cf.add', 'cf', str(i)))
        self.assertRaises(ResponseError, self.cmd, 'cf.add', 'cf', str(2048))

    def test_bucket_size(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE a 64 BUCKETSIZE 1')
        self.cmd('CF.RESERVE b 64 BUCKETSIZE 2')
        self.cmd('CF.RESERVE c 256 BUCKETSIZE 4 MAXITERATIONS 500')
        for i in range(1000):
            self.cmd('CF.ADD a', str(i))
            self.cmd('CF.ADD b', str(i))
            self.cmd('CF.ADD c', str(i))

        for i in range(1000):
            self.assertEqual(self.cmd('CF.EXISTS a', str(i)), 1)
            self.assertEqual(self.cmd('CF.EXISTS b', str(i)), 1)
            self.assertEqual(self.cmd('CF.EXISTS c', str(i)), 1)

        self.assertEqual(self.cmd('CF.DEBUG a'),
                         'bktsize:1 buckets:64 items:1000 deletes:0 filters:18 max_iterations:20 expansion:1')
        self.assertEqual(self.cmd('CF.DEBUG b'),
                         'bktsize:2 buckets:32 items:1000 deletes:0 filters:17 max_iterations:20 expansion:1')
        self.assertEqual(self.cmd('CF.DEBUG c'),
                         'bktsize:4 buckets:64 items:1000 deletes:0 filters:4 max_iterations:500 expansion:1')

        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 BUCKETSIZE')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 BUCKETSIZE string')

    def test_expansion(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE a 64 EXPANSION 1')
        self.cmd('CF.RESERVE b 64 EXPANSION 2')
        self.cmd('CF.RESERVE c 64 EXPANSION 4 MAXITERATIONS 500')
        for i in range(1000):
            self.cmd('CF.ADD a', str(i))
            self.cmd('CF.ADD b', str(i))
            self.cmd('CF.ADD c', str(i))

        for i in range(1000):
            self.assertEqual(self.cmd('CF.EXISTS a', str(i)), 1)
            self.assertEqual(self.cmd('CF.EXISTS b', str(i)), 1)
            self.assertEqual(self.cmd('CF.EXISTS c', str(i)), 1)

        self.assertEqual(self.cmd('CF.DEBUG a'),
                         'bktsize:2 buckets:32 items:1000 deletes:0 filters:17 max_iterations:20 expansion:1')
        self.assertEqual(self.cmd('CF.DEBUG b'),
                         'bktsize:2 buckets:32 items:1000 deletes:0 filters:5 max_iterations:20 expansion:2')
        self.assertEqual(self.cmd('CF.DEBUG c'),
                         'bktsize:2 buckets:32 items:1000 deletes:0 filters:3 max_iterations:500 expansion:4')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 EXPANSION')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 EXPANSION string')

    def test_expansion_0(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE a 4 EXPANSION 0')
        self.assertEqual(self.cmd('CF.ADD a 1'), 1)
        self.assertEqual(self.cmd('CF.ADD a 2'), 1)
        self.assertEqual(self.cmd('CF.ADD a 3'), 1)
        self.assertEqual(self.cmd('CF.ADD a 4'), 1)
        self.assertEqual(self.cmd('CF.INSERT a ITEMS 5 6'), [-1, -1])
        self.assertRaises(ResponseError, self.cmd, 'Filter is full')

    def test_info(self):
        self.cmd('FLUSHALL')
        self.cmd('CF.RESERVE a 1000')
        self.assertEqual(self.cmd('CF.INFO a'), ['Size', 1080,
                                                 'Number of buckets', 512,
                                                 'Number of filters', 1,
                                                 'Number of items inserted', 0,
                                                 'Number of items deleted', 0,
                                                 'Bucket size', 2,
                                                 'Expansion rate', 1,
                                                 'Max iterations', 20])

        with self.assertResponseError():
            self.cmd('cf.info', 'bf')
        with self.assertResponseError():
            self.cmd('cf.info')

    def test_params(self):
        self.cmd('FLUSHALL')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 EXPANSION -1')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 BUCKETSIZE 0')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 BUCKETSIZE -1')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 MAXITERATIONS 0')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE err 10 MAXITERATIONS -1')
        self.cmd('CF.RESERVE err 1000')

        self.assertRaises(ResponseError, self.cmd, 'CF.ADD')
        self.assertRaises(ResponseError, self.cmd, 'CF.ADD err')
        self.assertRaises(ResponseError, self.cmd, 'CF.ADDNX')
        self.assertRaises(ResponseError, self.cmd, 'CF.ADDNX err')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERT')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERT err')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERT err element') # W/O ITEMS keyword
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERTNX')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERTNX err')
        self.assertRaises(ResponseError, self.cmd, 'CF.INSERTNX err element') # W/O ITEMS keyword

        self.assertRaises(ResponseError, self.cmd, 'CF.DEL')
        self.assertRaises(ResponseError, self.cmd, 'CF.DEL err')
        self.assertRaises(ResponseError, self.cmd, 'CF.EXISTS')
        self.assertRaises(ResponseError, self.cmd, 'CF.EXISTS err')
        self.assertRaises(ResponseError, self.cmd, 'CF.MEXISTS')
        self.assertRaises(ResponseError, self.cmd, 'CF.MEXISTS err')
        self.assertRaises(ResponseError, self.cmd, 'CF.COUNT')
        self.assertRaises(ResponseError, self.cmd, 'CF.COUNT err')
        self.assertRaises(ResponseError, self.cmd, 'CF.INFO')

        self.assertRaises(ResponseError, self.cmd, 'CF.LOADCHUNK err')
        self.assertRaises(ResponseError, self.cmd, 'CF.LOADCHUNK err iterator') # missing data
        self.assertRaises(ResponseError, self.cmd, 'CF.SCANDUMP err')

    def test_reserve_limits(self):
        self.cmd('FLUSHALL')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE cf 100 BUCKETSIZE 33554432')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE cf 100 MAXITERATIONS 165536')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE cf 100 EXPANSION 327695')
        self.assertRaises(ResponseError, self.cmd, 'CF.RESERVE CF 67108864 BUCKETSIZE 33554432 MAXITERATIONS 1337 EXPANSION 1337')

        self.cmd('CF.RESERVE cf 67108864 BUCKETSIZE 255 MAXITERATIONS 65535 EXPANSION 32768')
        info = self.cmd('CF.INFO cf')
        self.assertEqual(info[info.index('Bucket size') + 1], 255)
        self.assertEqual(info[info.index('Expansion rate') + 1], 32768)
        self.assertEqual(info[info.index('Max iterations') + 1], 65535)

class testCuckooNoCodec():
    def __init__(self):
        self.env = Env(decodeResponses=False)
        self.assertOk = self.env.assertTrue
        self.cmd = self.env.cmd
        self.assertEqual = self.env.assertEqual
        self.assertRaises = self.env.assertRaises
        self.assertTrue = self.env.assertTrue
        self.assertAlmostEqual = self.env.assertAlmostEqual
        self.assertGreater = self.env.assertGreater
        self.restart_and_reload = self.env.restartAndReload
        self.assertResponseError = self.env.assertResponseError
        self.retry_with_rdb_reload = self.env.dumpAndReload
        self.assertNotEqual = self.env.assertNotEqual
        self.assertGreaterEqual = self.env.assertGreaterEqual

    def test_scandump(self):
        self.cmd('FLUSHALL')
        maxrange = 500
        self.cmd('cf.reserve', 'cf', int(maxrange / 8))
        self.assertEqual([0, None], self.cmd('cf.scandump', 'cf', '0'))
        for x in range(maxrange):
            self.cmd('cf.add', 'cf', str(x))
        for x in range(maxrange):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', str(x)))
        # Start with scandump
        self.assertRaises(ResponseError, self.cmd, 'cf.scandump', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.scandump', 'cf', 'str')
        self.assertRaises(ResponseError, self.cmd, 'cf.scandump', 'noexist', '0')
        chunks = []
        while True:
            last_pos = chunks[-1][0] if chunks else 0
            chunk = self.cmd('cf.scandump', 'cf', last_pos)
            if not chunk[0]:
                break
            chunks.append(chunk)
            # self.env.debugPrint(f"Scaning chunk... (P={chunk[0]}. Len={len(chunk[1])})")
        self.cmd('del', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', 'str')
        for chunk in chunks:
            self.env.debugPrint(f"Loading chunk... (P={chunk[0]}. Len={len(chunk[1])})")
            self.cmd('cf.loadchunk', 'cf', *chunk)
        for x in range(maxrange):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', str(x)))

    def test_scandump_with_expansion(self):
        self.cmd('FLUSHALL')
        maxrange = 500

        self.cmd('cf.reserve', 'cf', int(maxrange / 8), 'expansion', '2')
        self.assertEqual([0, None], self.cmd('cf.scandump', 'cf', '0'))
        for x in range(maxrange):
            self.cmd('cf.add', 'cf', str(x))
        for x in range(maxrange):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', str(x)))

        chunks = []
        while True:
            i = 0
            last_pos = chunks[-1][0] if chunks else 0
            chunk = self.cmd('cf.scandump', 'cf', last_pos)
            if not chunk[0]:
                break
            chunks.append(chunk)
            self.env.debugPrint(f"Scaning chunk... (P={chunk[0]}. Len={len(chunk[1])})")
        # delete filter
        self.cmd('del', 'cf')

        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', 'str')
        for chunk in chunks:
            self.env.debugPrint(f"Loading chunk... (P={chunk[0]}. Len={len(chunk[1])})")
            self.cmd('cf.loadchunk', 'cf', *chunk)
        # check loaded filter
        for x in range(maxrange):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', str(x)))

    def test_scandump_huge(self):
        self.cmd('FLUSHALL')

        self.cmd('cf.reserve', 'cf', 1024 * 1024 * 64)
        self.assertEqual([0, None], self.cmd('cf.scandump', 'cf', '0'))
        for x in range(6):
            self.cmd('cf.add', 'cf', 'foo')
        for x in range(6):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', 'foo'))

        chunks = []
        while True:
            i = 0
            last_pos = chunks[-1][0] if chunks else 0
            chunk = self.cmd('cf.scandump', 'cf', last_pos)
            if not chunk[0]:
                break
            chunks.append(chunk)
            self.env.debugPrint(f"Scaning chunk... (P={chunk[0]}. Len={len(chunk[1])})")
        # delete filter
        self.cmd('del', 'cf')

        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', 'str')
        for chunk in chunks:
            self.env.debugPrint(f"Loading chunk... (P={chunk[0]}. Len={len(chunk[1])})")
            self.cmd('cf.loadchunk', 'cf', *chunk)
        # check loaded filter
        for x in range(6):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', 'foo'))

    def test_scandump_with_content(self):
        # Basic success scenario with content validation

        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 1024 * 1024 * 64)

        for x in range(1000):
            self.cmd('cf.add', 'cf', 'foo' + str(x))
        for x in range(1000):
            self.assertEqual(1, self.cmd('cf.exists', 'cf', 'foo' + str(x)))

        chunks = []
        while True:
            last_pos = chunks[-1][0] if chunks else 0
            chunk = self.cmd('cf.scandump', 'cf', last_pos)
            if not chunk[0]:
                break
            chunks.append(chunk)

        for chunk in chunks:
            self.cmd('cf.loadchunk', 'cf2', *chunk)

        # check loaded filter
        for x in range(1000):
            self.assertEqual(1, self.cmd('cf.exists', 'cf2', 'foo' + str(x)))


    def test_scandump_invalid(self):
        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 4)
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', '-9223372036854775808', '1')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', '922337203685477588', '1')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', '4', 'kdoasdksaodsadsadsadsadsadadsadadsdad')
        self.assertRaises(ResponseError, self.cmd, 'cf.loadchunk', 'cf', '4', 'abcd')
        self.cmd('cf.add', 'cf', 'x')
        self.assertRaises(ResponseError, self.cmd, 'cf.scandump', 'cf', '-1')


    def test_scandump_invalid_header(self):
        env = self.env
        env.cmd('FLUSHALL')

        env.cmd('cf.reserve', 'cf', 100)
        for x in range(50):
            env.cmd('cf.add', 'cf', 'foo' + str(x))

        chunk = env.cmd('cf.scandump', 'cf', 0)
        env.cmd('del', 'cf')

        arr = bytearray(chunk[1])

        # It corrupts first 8 bytes in the response. See struct CFHeader
        # for internals.
        for i in range(9):
            arr[i] = 0

        thrown = None
        try:
            env.cmd('cf.loadchunk', 'cf', 1, bytes(arr))
        except Exception as e:
            thrown = e

        if thrown is None or str(thrown) != "Couldn't create filter!":
            print("Exception was: " + str(thrown))
            assert False

    def test_scandump_invalid_header_alloc(self):
        self.env.skipOnVersionSmaller('7.4')
        # Calls cf.loadchunk with a corrupt header that tries to do a huge
        # memory allocation
        env = self.env
        env.cmd('FLUSHALL')

        env.cmd('cf.reserve', 'cf', 100)
        for x in range(50):
            env.cmd('cf.add', 'cf', 'foo' + str(x))

        chunk = env.cmd('cf.scandump', 'cf', 0)
        env.cmd('del', 'cf')

        # It corrupts 'numBuckets' in the response. See struct CFHeader
        # for internals.
        arr = bytearray(chunk[1])
        for i in range(8, 13):
            arr[i] = 0
        arr[14] = 0x01

        self.env.expect('cf.loadchunk', 'cf', 1, bytes(arr)).error().contains("Couldn't create filter!")


    def test_scandump_random_scan_small(self):
        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 50)

        for i in range(0, 10000):
            try:
                self.cmd('cf.add', 'cf', 'x' + str(i))
            except ResponseError as e:
                if str(e) == "Maximum expansions reached":
                    break
                raise e

        info = self.cmd('CF.INFO', 'cf')
        size = info[info.index(b'Size') + 1]

        for i in range(0, size + 1024):
            self.cmd('cf.scandump', 'cf', i)


    def test_scandump_scan_big(self):
        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 1024, 'EXPANSION', 30000)

        for i in range(0, 100):
            arr = []
            for j in range(0, 10000):
                arr.append('x' + str(i) + str(j))

            try:
                self.cmd('cf.insert', 'cf', 'ITEMS', *arr)
            except ResponseError as e:
                if str(e) == "Maximum expansions reached":
                    break
                raise e

        info = self.cmd('CF.INFO', 'cf')
        size = info[info.index(b'Size') + 1]

        for i in range(0, 100):
            self.cmd('cf.scandump', 'cf', random.randint(0, size * 2))


    def test_scandump_load_small(self):
        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 10)

        for i in range(0, 100):
            arr = []
            for j in range(0, 1000):
                arr.append('x' + str(i) + str(j))

            try:
                self.cmd('cf.insert', 'cf', 'ITEMS', *arr)
            except ResponseError as e:
                if str(e) == "Maximum expansions reached":
                    break
                raise e

        info = self.cmd('CF.INFO', 'cf')
        size = info[info.index(b'Size') + 1]

        for i in range (0, size + 100):
            b = bytearray(os.urandom(random.randint(0, 100)))
            try:
                self.cmd('cf.loadchunk', 'cf', random.randint(0, 10000), bytes(b))
            except Exception as e:
                if (str(e) != "Couldn't load chunk!" and
                        str(e) != "Invalid position" and
                        str(e) != "item exists"):
                    raise e


    def test_scandump_load_big(self):
        self.cmd('FLUSHALL')
        self.cmd('cf.reserve', 'cf', 1024, 'EXPANSION', 30000)

        for i in range(0, 100):
            arr = []
            for j in range(0, 1000):
                arr.append('x' + str(i) + str(j))

            try:
                self.cmd('cf.insert', 'cf', 'ITEMS', *arr)
            except ResponseError as e:
                if str(e) == "Maximum expansions reached":
                    break
                raise e

        info = self.cmd('CF.INFO', 'cf')
        size = info[info.index(b'Size') + 1]

        for i in range (0, 100):
            b = bytearray(os.urandom(random.randint(1024, 36 * 1024 * 1024)))
            try:
                self.cmd('cf.loadchunk', 'cf', random.randint(2, size), bytes(b))
            except Exception as e:
                if str(e) != "Couldn't load chunk!":
                    raise e

    def test_insufficient_memory(self):
        # skip because Capacity must be in the range [2 * BUCKETSIZE, 1073741824]
        self.env.skip()
        self.cmd('FLUSHALL')
        self.env.skipOnVersionSmaller('7.4')

        self.env.expect('cf.reserve', 'cf', '100000000000000').error().contains('Insufficient memory to create filter')
        self.env.expect('cf.reserve', 'cf', '1844674407370955165', 'EXPANSION', '32').error().contains('Insufficient memory to create filter')
        self.env.expect('cf.reserve', 'cf', '100000000000000', 'BUCKETSIZE', '255', 'EXPANSION', '32768').error().contains('Insufficient memory to create filter')
        self.env.expect('cf.insert', 'cf', 'capacity', '100000000000000', 'ITEMS', 1).error().contains('Insufficient memory to create filter')

    def test_rdb_load_zero_numfilters(self):
        self.cmd('FLUSHALL')
        rdb_payload = b'\x07\x810\x16\xe5\xa2\x89\x82\x14\x04\x02\x00\x02\x08\x02\x01\x02\x00\x02\x01\x02\x01\x02\x01\x00\xff\x0c\x00`\x07q:\xe0pL\r'
        self.env.expect('RESTORE', "hax", 0, rdb_payload, 'REPLACE').error().contains('Bad data format')


def test_cf_loadchunk_replicates_to_replica():
    # MOD-16050: CF.LOADCHUNK must replicate its data chunks, not only the header.
    # Otherwise a replica of a CF.LOADCHUNK-restored filter has the correct
    # metadata (CF.INFO reports the full item count) but empty buckets, and a
    # failover silently loses all the data.
    # freshEnv=True forces a dedicated master+replica env, so this test is not
    # affected by env reuse from preceding (non-replicated) tests.
    env = Env(useSlaves=True, decodeResponses=False, freshEnv=True)
    if env.isCluster():
        env.skip()

    master = env.getConnection()
    slave = env.getSlaveConnection()

    n = 2000
    # Build a source filter that spans several sub-filters (expansion), then
    # serialize it with CF.SCANDUMP.
    master.execute_command('CF.RESERVE', 'src', 100, 'EXPANSION', 2)
    for i in range(n):
        master.execute_command('CF.ADD', 'src', 'item%d' % i)

    chunks = []
    pos = 0
    while True:
        pos, data = master.execute_command('CF.SCANDUMP', 'src', pos)
        if pos == 0:
            break
        chunks.append((pos, data))
    # Must have data chunks beyond the header, otherwise this would not exercise
    # the data-chunk replication path.
    env.assertGreater(len(chunks), 1)

    # Restore into a fresh key via CF.LOADCHUNK on the master.
    for pos, data in chunks:
        master.execute_command('CF.LOADCHUNK', 'cf', pos, data)

    # The replica must be caught up to the master's replication offset. Poll WAIT
    # to tolerate the replica still connecting/doing its initial sync.
    acked = 0
    deadline = time.time() + 15
    while time.time() < deadline:
        acked = master.execute_command('WAIT', 1, 1000)
        if acked >= 1:
            break
    env.assertEqual(acked, 1)

    # ...and must hold the actual filter data, not just the header metadata.
    missing = [i for i in range(n)
               if slave.execute_command('CF.EXISTS', 'cf', 'item%d' % i) != 1]
    env.assertEqual(missing, [],
                    message='%d/%d items missing on replica after CF.LOADCHUNK' % (len(missing), n))


def test_cf_add_autocreate_expansion_limit_replicates_to_replica():
    # cfInsertCommon() autocreates the filter (RedisModule_ModuleTypeSetValue,
    # a real keyspace mutation) *before* it checks cf->numFilters against
    # cf-max-expansions. When that guard trips on a brand-new key, the
    # function returns an error without ever reaching the single
    # RedisModule_ReplicateVerbatim() call at the end of cfInsertCommon. So a
    # CF.ADD that both creates the key and immediately hits the expansion
    # limit is never replicated, even though the key now exists on the
    # master. Master and replica must never disagree on whether a key exists.
    # freshEnv=True forces a dedicated master+replica env, so this test is not
    # affected by env reuse from preceding (non-replicated) tests.
    env = Env(useSlaves=True, decodeResponses=True, freshEnv=True)
    if env.isCluster():
        env.skip()

    master = env.getConnection()
    slave = env.getSlaveConnection()

    # A freshly created cuckoo filter has numFilters == 1 (CuckooFilter_Init
    # grows once), so cf-max-expansions=1 makes the very first insert into a
    # new key trip the "Maximum expansions reached" guard.
    master.execute_command('CONFIG', 'SET', 'cf-max-expansions', '1')

    # 'newkey' does not exist yet; CF.ADD's autocreate path should create it
    # and then immediately fail the expansion-limit check.
    env.assertFalse(master.execute_command('EXISTS', 'newkey'))
    with env.assertResponseError(contained='Maximum expansions reached'):
        master.execute_command('CF.ADD', 'newkey', 'x')

    existsOnMaster = bool(master.execute_command('EXISTS', 'newkey'))

    # The replica must be caught up to the master's replication offset. Poll
    # WAIT to tolerate the replica still connecting/doing its initial sync.
    acked = 0
    deadline = time.time() + 15
    while time.time() < deadline:
        acked = master.execute_command('WAIT', 1, 1000)
        if acked >= 1:
            break
    env.assertEqual(acked, 1)

    existsOnSlave = bool(slave.execute_command('EXISTS', 'newkey'))

    # Master and replica must agree on whether 'newkey' exists.
    env.assertEqual(existsOnMaster, existsOnSlave,
                    message='newkey existence diverged: master=%s replica=%s' %
                            (existsOnMaster, existsOnSlave))
