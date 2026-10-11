
from support import *
from types import SimpleNamespace
from rdb_corruption_utils import rewrite_module_uint, rewrite_largest_module_string
import struct
from random import randint


class testCMS():
    def __init__(self):
        self.env = Env(decodeResponses=True)
        self.assertOk = self.env.assertTrue
        self.cmd = self.env.cmd
        self.assertEqual = self.env.assertEqual
        self.assertRaises = self.env.assertRaises
        self.assertTrue = self.env.assertTrue
        self.assertAlmostEqual = self.env.assertAlmostEqual
        self.assertGreater = self.env.assertGreater

    def test_simple(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1', '20', '5'))
        self.assertEqual([5], self.cmd('cms.incrby', 'cms1', 'a', '5'))
        self.assertEqual([5], self.cmd('cms.query', 'cms1', 'a'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 5, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1'))

        self.assertOk(self.cmd('cms.initbyprob', 'cms2', '0.001', '0.01'))
        self.assertEqual([5], self.cmd('cms.incrby', 'cms2', 'a', '5'))
        self.assertEqual([5], self.cmd('cms.query', 'cms2', 'a'))
        self.assertEqual(['width', 2000, 'depth', 7, 'count', 5, 'cell_size', 4],
                         self.cmd('cms.info', 'cms2'))
        yield 1
        self.env.dumpAndReload()
        yield 2
        # Native objects use core allocator accounting, not module overhead.
        self.assertGreater(self.cmd('MEMORY USAGE', 'cms1'), 400)

    def test_validation(self):
        self.cmd('FLUSHALL')
        for args in (
                (),
                ('foo',),
                ('foo', '0.1'),
                ('foo', '0.1', 'blah'),
                ('foo', '10'),
                ('foo', '10', 'blah'),
                ('foo', 'blah', '10'),
                ('foo', '0', '0'),
                ('foo', '0', '100'),
                ('foo', '100', '0'),
                ('foo', '8589934592', '8589934592'),
        ):
            self.assertRaises(ResponseError, self.cmd, 'cms.initbydim', *args)

        for args in (
                (),
                ('foo',),
                ('foo', '1000'),
                ('foo', '0.1'),
                ('foo', '1000', '0.1'),
                ('foo', '1000', 'blah'),
                ('foo', '1000', '10'),
                ('foo', '0.1', 'blah'),
                ('foo', '10', 'blah'),
                ('foo', 'blah', '10'),
                ('foo', '0', '0'),
                ('foo', '1000', '0',),
                ('foo', '0', '100'),
                ('foo', '0.9', '0.9999999999999999'),
                ('foo', '0.0000000000000000001', '0.9'),
        ):
            self.assertRaises(ResponseError, self.cmd, 'cms.initbyprob', *args)

        self.assertRaises(ResponseError, self.cmd, 'cms.initbydim', '0.1', '0.1')
        self.assertRaises(ResponseError, self.cmd, 'cms.initbyprob', '10', '10')

        self.assertOk(self.cmd('cms.initbydim', 'testDim', '100', '5'))
        self.assertOk(self.cmd('cms.initbyprob', 'testProb', '0.1', '0.1'))

        for args in ((), ('test',)):
            for cmd in ('cms.incrby', 'cms.query', 'cms.merge', 'cms.info'):
                self.assertRaises(ResponseError, self.cmd, cmd, *args)

    def test_incrby_query(self):
        self.cmd('FLUSHALL')
        self.cmd('SET', 'A', 'B')
        self.cmd('cms.initbydim', 'cms', '1000', '5')
        self.cmd('cms.incrby', 'cms', 'bar', '5', 'baz', '42')
        self.assertEqual([0], self.cmd('cms.query', 'cms', 'foo'))
        self.assertEqual([0, 5, 42], self.cmd('cms.query',
                                              'cms', 'foo', 'bar', 'baz'))
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby', 'noexist', 'bar', '5')
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby', 'A', 'bar', '5')
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby',
                          'cms', 'bar', '5', 'baz')
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby',
                          'cms', 'bar', '5', 'baz')
        self.assertEqual([0, 5, 42], self.cmd('cms.query',
                                              'cms', 'foo', 'bar', 'baz'))

        # c = self.client
        self.cmd('cms.initbydim', 'test', '1000', '5')
        self.assertEqual([1], self.cmd('cms.incrby', 'test', 'foo', '1'))
        self.assertEqual([1], self.cmd('cms.query', 'test', 'foo'))
        self.assertEqual([0], self.cmd('cms.query', 'test', 'bar'))

        self.assertEqual([2, 1], self.cmd('cms.incrby', 'test', 'foo', '1', 'bar', '1'))
        # for _ in c.retry_with_rdb_reload():
        #     self.assertEqual([2], self.cmd('cms.query', 'test', 'foo'))
        #     self.assertEqual([1], self.cmd('cms.query', 'test', 'bar'))
        #     self.assertEqual([0], self.cmd('cms.query', 'test', 'nonexist'))

    def test_merge(self):
        self.cmd('FLUSHALL')
        self.cmd('cms.initbydim', 'small_1{1}', '20', '5')
        self.cmd('cms.initbydim', 'small_2{1}', '20', '5')
        self.cmd('cms.initbydim', 'small_3{1}', '20', '5')
        self.cmd('cms.initbydim', 'large_4{1}', '2000', '10')
        self.cmd('cms.initbydim', 'large_5{1}', '2000', '10')
        self.cmd('cms.initbydim', 'large_6{1}', '2000', '10')

        # empty small batch
        self.assertOk(self.cmd('cms.merge', 'small_3{1}', 2, 'small_1{1}', 'small_2{1}'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'small_3{1}'))

        # empty large batch
        self.assertOk(self.cmd('cms.merge', 'large_6{1}', 2, 'large_4{1}', 'large_5{1}'))
        self.assertEqual(['width', 2000, 'depth', 10, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'large_6{1}'))

        # non-empty small batch
        self.cmd('cms.incrby', 'small_1{1}', 'a', '21')
        self.cmd('cms.incrby', 'small_2{1}', 'a', '21')
        self.assertOk(self.cmd('cms.merge', 'small_3{1}', 2, 'small_1{1}', 'small_2{1}'))
        self.assertEqual([42], self.cmd('cms.query', 'small_3{1}', 'a'))

        # non-empty small batch
        self.cmd('cms.incrby', 'large_4{1}', 'a', '21')
        self.cmd('cms.incrby', 'large_5{1}', 'a', '21')
        self.assertOk(self.cmd('cms.merge', 'large_6{1}', 2, 'large_4{1}', 'large_5{1}'))
        self.assertEqual([42], self.cmd('cms.query', 'large_6{1}', 'a'))

        # mixed batch
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'small_3{1}', 2,
                          'small_2{1}', 'large_5{1}')

    def test_merge_crossslot(self):
        if True:
            self.env.skip('Standalone runner; native MERGE key specifications are tested separately')

        self.assertOk(self.cmd('cms.initbydim', 'dst', '100', '5'))
        self.assertOk(self.cmd('cms.initbydim', 's1', '100', '5'))
        self.assertOk(self.cmd('cms.initbydim', 's2', '100', '5'))
        self.assertEqual([5], self.cmd('cms.incrby', 's1', 'x', '5'))
        self.assertEqual([2], self.cmd('cms.incrby', 's2', 'x', '2'))
        res = self.env.expect('cms.merge', 'dst', 2, 's1', 's2').error()
        res.contains("Keys in request don't hash to the same slot")
        res.notContains('CMS: key does not exist')

    def test_errors(self):
        self.cmd('FLUSHALL')
        self.cmd('SET', 'A', '2000')
        self.assertRaises(ResponseError, self.cmd, 'cms.initbydim', 'A', '2000', '10')
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby', 'A', 'foo')
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby', 'B', '5')
        self.assertRaises(ResponseError, self.cmd, 'cms.info', 'A')

        self.assertOk(self.cmd('cms.initbydim', 'foo', '2000', '10'))
        self.assertOk(self.cmd('cms.initbydim', 'bar', '2000', '10'))
        self.assertOk(self.cmd('cms.initbydim', 'baz', '2000', '10'))
        self.assertRaises(ResponseError, self.cmd, 'cms.incrby', 'foo', 'item', 'foo')
        # a negative increment is valid syntax now, but underflows an empty sketch
        res = self.cmd('cms.incrby', 'foo', 'item', '-1')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 2, 'foo')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 'B', 3, 'foo')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 1, 'bar', 'weights', 'B')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 3, 'foo', 'weights', 'B')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 'A', 'foo', 'weights', 1)
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 3, 'bar', 'baz' 'weights', 1, 'a')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', 3, 'bar', 'baz' 'weights', 1)
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', '0')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', '0', 'weights')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', '-1')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', '-1', 'foo', 'bar')
        self.assertRaises(ResponseError, self.cmd, 'cms.merge', 'foo', '-1', 'foo', 'bar', 'weights', 1, 1)

    def test_merge_extensive(self):
        self.cmd('FLUSHALL')
        self.cmd('cms.initbydim', 'A{1}', '2000', '10')
        self.cmd('cms.initbydim', 'B{1}', '2000', '10')
        self.cmd('cms.initbydim', 'C{1}', '2000', '10')

        itemsA = []
        itemsB = []
        for i in range(10000):
            itemsA.append(randint(0, 100))
            self.cmd('cms.incrby', 'A{1}', str(i), itemsA[i])
            itemsB.append(randint(0, 100))
            self.cmd('cms.incrby', 'B{1}', str(i), itemsB[i])
        self.assertOk(self.cmd('cms.merge', 'C{1}', 2, 'A{1}', 'B{1}'))

    def test_overflow(self):
        large_val = 1024*1024*1024*2 - 1

        self.cmd('FLUSHALL')
        self.cmd('cms.initbydim', 'cms', '5', '2')
        self.assertEqual([large_val, 10, 17, 5], self.cmd('cms.incrby', 'cms', 'a', large_val, 'b', 10, 'c', 7, 'd', 5))
        self.assertEqual([large_val, 17, 17, 5], self.cmd('cms.query', 'cms', 'a', 'b', 'c', 'd'))
        self.assertEqual([large_val * 2, 27, 34, 10], self.cmd('cms.incrby', 'cms', 'a', large_val, 'b', 10, 'c', 7, 'd', 5))
        self.assertEqual([large_val * 2, 34, 34, 10], self.cmd('cms.query', 'cms', 'a', 'b', 'c', 'd'))

        # overflow as result > UNIT32_MAX
        res = self.cmd('cms.incrby', 'cms', 'a', large_val, 'b', 10, 'c', 7, 'd', 5)
        # result of insert is an error message
        self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')
        self.assertEqual(res[1:], [44, 51, 15])
        # the rejected increment left 'a' untouched, the other items went through
        self.assertEqual([large_val * 2, 51, 51, 15], self.cmd('cms.query', 'cms', 'a', 'b', 'c', 'd'))
        # and it did not advance the total count either: two full rounds of
        # (large_val + 10 + 7 + 5) plus one round without 'a'
        info = dict(zip(*[iter(self.cmd('cms.info', 'cms'))] * 2))
        self.assertEqual(2 * (large_val + 22) + 22, info['count'])

    def test_smallset(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1', '2', '2'))
        self.assertEqual([10, 42], self.cmd('cms.incrby', 'cms1', 'foo', '10', 'bar', '42'))
        self.assertEqual([10, 42], self.cmd('cms.query', 'cms1', 'foo', 'bar'))
        self.assertEqual(['width', 2, 'depth', 2, 'count', 52, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1'))
        self.assertEqual([10, 42], self.cmd('cms.incrby', 'cms1', 'foo', '0', 'bar', '0'))

    def test_merge_success(self):
        # Merge three sketches and then delete one sketch (merge with -1 weight)
        # Validate content after merge operations.
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1{t}', '1000', '5'))
        self.assertOk(self.cmd('cms.initbydim', 'cms2{t}', '1000', '5'))
        self.assertOk(self.cmd('cms.initbydim', 'cms3{t}', '1000', '5'))
        self.assertOk(self.cmd('cms.initbydim', 'cms4{t}', '1000', '5'))
        self.assertOk(self.cmd('cms.initbydim', 'cms5{t}', '1000', '5'))
        self.assertOk(self.cmd('cms.initbydim', 'cms6{t}', '1000', '5'))

        for i in range(0, 100):
            self.assertOk(self.cmd('cms.incrby', 'cms1{t}', 'foo' + str(i), 1))
            self.assertOk(self.cmd('cms.incrby', 'cms2{t}', 'bar' + str(i), 1))
            self.assertOk(self.cmd('cms.incrby', 'cms3{t}', 'baz' + str(i), 1))

        # Merge cms1{t} and cms2{t} into cms4{t}
        self.env.expect('cms.merge', 'cms4{t}', 2, 'cms1{t}', 'cms2{t}', 'weights', '1', '1').ok()
        for i in range(0, 100):
            self.assertEqual([1], self.cmd('cms.query', 'cms4{t}', 'foo' + str(i)))
            self.assertEqual([1], self.cmd('cms.query', 'cms4{t}', 'bar' + str(i)))

        # Merge cms1{t}, cms2{t} and cms3{t} into cms5{t}
        self.env.expect('cms.merge', 'cms5{t}', 3,
                        'cms1{t}', 'cms2{t}', 'cms3{t}',
                        'weights', '1', '1', '1').ok()
        for i in range(0, 100):
            self.assertEqual([1], self.cmd('cms.query', 'cms5{t}', 'foo' + str(i)))
            self.assertEqual([1], self.cmd('cms.query', 'cms5{t}', 'bar' + str(i)))
            self.assertEqual([1], self.cmd('cms.query', 'cms5{t}', 'baz' + str(i)))

        # Delete cms3{t} from cms5{t} and store in cms6{t}
        self.env.expect('cms.merge', 'cms6{t}', 2, 'cms5{t}', 'cms3{t}', 'weights', '1', '-1').ok()
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 200, 'cell_size', 4], self.cmd('cms.info', 'cms6{t}'))
        # Validate cms6{t} has cms1{t} and cms2{t} only.
        for i in range(0, 100):
            self.assertEqual([1], self.cmd('cms.query', 'cms6{t}', 'foo' + str(i)))
            self.assertEqual([1], self.cmd('cms.query', 'cms6{t}', 'bar' + str(i)))
            self.assertEqual([0], self.cmd('cms.query', 'cms6{t}', 'baz' + str(i)))

        # Same test as above, negative weight first.
        self.env.expect('cms.merge', 'cms6{t}', 2, 'cms3{t}', 'cms5{t}', 'weights', '-1', '1').ok()
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 200, 'cell_size', 4], self.cmd('cms.info', 'cms6{t}'))
        # Validate cms6{t} has cms1{t} and cms2{t} only.
        for i in range(0, 100):
            self.assertEqual([1], self.cmd('cms.query', 'cms6{t}', 'foo' + str(i)))
            self.assertEqual([1], self.cmd('cms.query', 'cms6{t}', 'bar' + str(i)))
            self.assertEqual([0], self.cmd('cms.query', 'cms6{t}', 'baz' + str(i)))

        # Validate you can't delete cms3{t} again.
        self.env.expect('cms.merge', 'cms6{t}', 2, 'cms6{t}', 'cms3{t}',
                        'weights', '1', '-1').error().contains('CMS: MERGE overflow')

        self.env.expect('cms.merge', 'cms6{t}', 2, 'cms3{t}', 'cms6{t}',
                        'weights', '-1', '1').error().contains('CMS: MERGE overflow')

    def test_merge_success_large(self):
        # Create relatively big sketches and verify merge operation works fine
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1{t}', '3000', '30'))
        self.assertOk(self.cmd('cms.initbydim', 'cms2{t}', '3000', '30'))
        self.assertOk(self.cmd('cms.initbydim', 'cms3{t}', '3000', '30'))

        for i in range(0, 1000):
            self.assertOk(self.cmd('cms.incrby', 'cms1{t}', 'foo' + str(i), 1))
            self.assertOk(self.cmd('cms.incrby', 'cms2{t}', 'foo' + str(i), 1))
            self.assertOk(self.cmd('cms.incrby', 'cms3{t}', 'bar' + str(i), 1))

        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '1', '-1').ok()
        self.assertEqual(['width', 3000, 'depth', 30, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1{t}'))

        for i in range(0, 1000):
            self.assertEqual([0], self.cmd('cms.query', 'cms1{t}', 'foo' + str(i)))

        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '1', '2').ok()
        self.assertEqual(['width', 3000, 'depth', 30, 'count', 2000, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1{t}'))

        for i in range(0, 1000):
            self.assertEqual([2], self.cmd('cms.query', 'cms1{t}', 'foo' + str(i)))

        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '1', '-1').ok()
        self.assertEqual(['width', 3000, 'depth', 30, 'count', 1000, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1{t}'))
        for i in range(0, 1000):
            self.assertEqual([1], self.cmd('cms.query', 'cms1{t}', 'foo' + str(i)))

        # Repeatedly add cms3{t} to cms1{t} and verify content.
        for i in range(0, 10):
            self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms3{t}',
                            'weights', '1', '1').ok()
            for j in range(0, 1000):
                self.assertEqual([i + 1], self.cmd('cms.query', 'cms1{t}', 'bar' + str(j)))
                self.assertEqual([1], self.cmd('cms.query', 'cms1{t}', 'foo' + str(j)))

        # Repeatedly delete cms3{t} from cms1{t} and verify content.
        # 'i' will increment by 2, and it will delete cms3{t} with weight -2
        for i in range(0, 10, 2):
            self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms3{t}',
                            'weights', '1', '-2').ok()
            for j in range(0, 1000):
                self.assertEqual([8 - i], self.cmd('cms.query', 'cms1{t}', 'bar' + str(j)))
                self.assertEqual([1], self.cmd('cms.query', 'cms1{t}', 'foo' + str(j)))

        # Validate you can't delete cms3{t} again.
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms3{t}',
                        'weights', '1', '-1').error().contains('CMS: MERGE overflow')

    def test_merge_overflow_large_cell(self):
        # Validate cms.merge fails if there is overflow while merging cells
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1{t}', '2', '2'))
        self.assertOk(self.cmd('cms.initbydim', 'cms2{t}', '2', '2'))

        # 4000000000 will fit into 32-bit unsigned integer.
        self.assertEqual([4000000000], self.cmd('cms.incrby', 'cms1{t}', 'foo', '4000000000'))
        self.assertEqual([4000000000], self.cmd('cms.incrby', 'cms2{t}', 'foo', '4000000000'))

        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '1', '-2').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-2', '-2').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-2', '1').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '2', '0').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '0', '2').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-1', '-1').error().contains('CMS: MERGE overflow')

        # Validate keys did not change
        self.assertEqual(['width', 2, 'depth', 2, 'count', 4000000000, 'cell_size', 4],
                         self.cmd('cms.info', 'cms1{t}'))
        self.assertEqual(['width', 2, 'depth', 2, 'count', 4000000000, 'cell_size', 4],
                         self.cmd('cms.info', 'cms2{t}'))
        self.assertEqual([4000000000], self.cmd('cms.query', 'cms1{t}', 'foo'))
        self.assertEqual([4000000000], self.cmd('cms.query', 'cms2{t}', 'foo'))

    def test_merge_overflow_large_weight(self):
        # Validate cms.merge fails if there is overflow due to large weight arg
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms1{t}', '2', '2'))
        self.assertOk(self.cmd('cms.initbydim', 'cms2{t}', '2', '2'))
        self.assertEqual([4], self.cmd('cms.incrby', 'cms1{t}', 'foo', '4'))
        self.assertEqual([4], self.cmd('cms.incrby', 'cms2{t}', 'foo', '4'))

        # Test boundaries
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '9223372036854775807', '-4000000000').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '8000000000', '-4000000000').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-8000000000', '4000000000').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-800000000000', '8000000000').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-1', '-1').error().contains('CMS: MERGE overflow')
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-5', '-4').error().contains('CMS: MERGE overflow')

        # Validate keys did not change
        self.assertEqual(['width', 2, 'depth', 2, 'count', 4, 'cell_size', 4], self.cmd('cms.info', 'cms1{t}'))
        self.assertEqual(['width', 2, 'depth', 2, 'count', 4, 'cell_size', 4], self.cmd('cms.info', 'cms2{t}'))
        self.assertEqual([4], self.cmd('cms.query', 'cms1{t}', 'foo'))
        self.assertEqual([4], self.cmd('cms.query', 'cms2{t}', 'foo'))

        # An extreme test for a success scenario
        self.env.expect('cms.merge', 'cms1{t}', 2, 'cms1{t}', 'cms2{t}',
                        'weights', '-922337203685477500', '922337203685477502').ok()
        self.assertEqual(['width', 2, 'depth', 2, 'count', 8, 'cell_size', 4], self.cmd('cms.info', 'cms1{t}'))
        self.assertEqual([8], self.cmd('cms.query', 'cms1{t}', 'foo'))

    def test_watch(self):
        env = self.env
        env.cmd('FLUSHALL')
        env.cmd('CMS.INITBYDIM', 'basecms1', 1000, 5)
        watcher = Client(env.server.path)
        try:
            watcher.command('WATCH', 'basecms1')
            env.cmd('CMS.INCRBY', 'basecms1', 'smur', 5, 'rr', 9, 'ff', 99)
            watcher.command('MULTI')
            watcher.command('SET', 'x', 1)
            env.assertIsNone(watcher.command('EXEC'))
        finally:
            watcher.close()

    def test_insufficient_memory(self):
        self.env.skipOnVersionSmaller('7.4')
        self.cmd('FLUSHALL')
        self.env.expect('CMS.INITBYPROB', 'x', '0.0000000000000001', '0.0000000000000001').error().contains('CMS: Insufficient memory to create the key')
        self.env.expect('CMS.INITBYDIM',  'x', '1000000000', '1000000000').error().contains('CMS: Insufficient memory to create the key')
        self.env.expect('CMS.INITBYDIM',  'x', '2294967296', '2294967296').error().contains('CMS: Insufficient memory to create the key')
        self.env.expect('CMS.INITBYDIM',  'x', '100000000000000', '100000000000000').error().contains('CMS: invalid init arguments')


    def test_cell_size_create(self):
        self.cmd('FLUSHALL')
        for cell_size, width, depth in ((1, 20, 5), (2, 20, 5), (4, 20, 5), (8, 20, 5)):
            key = 'dim{}'.format(cell_size)
            self.assertOk(self.cmd('cms.initbydim', key, width, depth, 'CELL_SIZE', cell_size))
            self.assertEqual(['width', width, 'depth', depth, 'count', 0, 'cell_size', cell_size],
                             self.cmd('cms.info', key))

            key = 'prob{}'.format(cell_size)
            self.assertOk(self.cmd('cms.initbyprob', key, '0.001', '0.01', 'cell_size', cell_size))
            self.assertEqual(['width', 2000, 'depth', 7, 'count', 0, 'cell_size', cell_size],
                             self.cmd('cms.info', key))

        # no CELL_SIZE keeps the historical 4-byte cells
        self.assertOk(self.cmd('cms.initbydim', 'default', '20', '5'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'default'))

    def test_cell_size_validation(self):
        self.cmd('FLUSHALL')
        for cell_size in ('0', '3', '5', '6', '7', '9', '16', '-1', '-4', 'blah', '', '4.5'):
            self.env.expect('cms.initbydim', 'x', '20', '5', 'CELL_SIZE', cell_size) \
                .error().contains('CMS: CELL_SIZE must be 1, 2, 4 or 8')
            self.env.expect('cms.initbyprob', 'x', '0.001', '0.01', 'CELL_SIZE', cell_size) \
                .error().contains('CMS: CELL_SIZE must be 1, 2, 4 or 8')

        # unknown token in the optional slot
        self.env.expect('cms.initbydim', 'x', '20', '5', 'CELLSIZE', '1') \
            .error().contains('CMS: unknown argument')
        self.env.expect('cms.initbyprob', 'x', '0.001', '0.01', 'OOR', '1') \
            .error().contains('CMS: unknown argument')

        # wrong arity: token without a value, or trailing junk
        for args in (('x', '20', '5', 'CELL_SIZE'),
                     ('x', '20', '5', 'CELL_SIZE', '1', '2'),
                     ('x', '20', '5', '1')):
            self.assertRaises(ResponseError, self.cmd, 'cms.initbydim', *args)

        # width/depth are still validated when CELL_SIZE is given
        self.env.expect('cms.initbydim', 'x', '0', '5', 'CELL_SIZE', '1') \
            .error().contains('CMS: invalid width')
        self.env.expect('cms.initbyprob', 'x', '2', '0.01', 'CELL_SIZE', '1') \
            .error().contains('CMS: invalid overestimation value')

    def test_cell_size_max_value(self):
        # a 1x1 sketch has a single cell, so the estimate is exact
        cell_max = {1: 2 ** 8 - 1, 2: 2 ** 16 - 1, 4: 2 ** 32 - 1, 8: 2 ** 63 - 1}
        for cell_size, maximum in cell_max.items():
            self.cmd('FLUSHALL')
            self.assertOk(self.cmd('cms.initbydim', 'cms', '1', '1', 'CELL_SIZE', cell_size))
            self.assertEqual([maximum], self.cmd('cms.incrby', 'cms', 'a', maximum))
            self.assertEqual([maximum], self.cmd('cms.query', 'cms', 'a'))

            res = self.cmd('cms.incrby', 'cms', 'a', 1)
            self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')
            self.assertEqual([maximum], self.cmd('cms.query', 'cms', 'a'))
            self.assertEqual(['width', 1, 'depth', 1, 'count', maximum, 'cell_size', cell_size],
                             self.cmd('cms.info', 'cms'))

    def test_cell_size_overflow_in_one_shot(self):
        # an increment larger than the cell can ever hold is rejected outright
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '20', '5', 'CELL_SIZE', '1'))
        res = self.cmd('cms.incrby', 'cms', 'a', 256)
        self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')
        self.assertEqual([0], self.cmd('cms.query', 'cms', 'a'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 0, 'cell_size', 1],
                         self.cmd('cms.info', 'cms'))

    def test_cell_size_persistence(self):
        self.cmd('FLUSHALL')
        for cell_size in (1, 2, 4, 8):
            key = 'cms{}'.format(cell_size)
            self.assertOk(self.cmd('cms.initbydim', key, '20', '5', 'CELL_SIZE', cell_size))
            self.assertEqual([7], self.cmd('cms.incrby', key, 'a', '7'))

        self.env.dumpAndReload()

        for cell_size in (1, 2, 4, 8):
            key = 'cms{}'.format(cell_size)
            self.assertEqual(['width', 20, 'depth', 5, 'count', 7, 'cell_size', cell_size],
                             self.cmd('cms.info', key))
            self.assertEqual([7], self.cmd('cms.query', key, 'a'))

    def test_cell_size_dump_restore(self):
        self.cmd('FLUSHALL')
        # DUMP payloads are binary, so they need a connection that does not decode
        raw = SimpleNamespace(execute_command=self.env.server.client.command)
        for cell_size in (1, 2, 4, 8):
            key = 'cms{}'.format(cell_size)
            self.assertOk(self.cmd('cms.initbydim', key, '20', '5', 'CELL_SIZE', cell_size))
            self.assertEqual([7], self.cmd('cms.incrby', key, 'a', '7'))
            payload = raw.execute_command('DUMP', key)
            raw.execute_command('RESTORE', key + '_copy', 0, payload)
            self.assertEqual(['width', 20, 'depth', 5, 'count', 7, 'cell_size', cell_size],
                             self.cmd('cms.info', key + '_copy'))
            self.assertEqual([7], self.cmd('cms.query', key + '_copy', 'a'))

    def test_cell_size_memory(self):
        if VALGRIND:
            self.env.skip()
        self.cmd('FLUSHALL')
        usage = {}
        for cell_size in (1, 2, 4, 8):
            key = 'cms{}'.format(cell_size)
            self.assertOk(self.cmd('cms.initbydim', key, '1000', '5', 'CELL_SIZE', cell_size))
            usage[cell_size] = self.cmd('MEMORY USAGE', key)
        # the array dominates, so a wider cell costs proportionally more
        self.assertGreater(usage[2], usage[1])
        self.assertGreater(usage[4], usage[2])
        self.assertGreater(usage[8], usage[4])

    def test_decrby(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '1000', '5'))
        self.assertEqual([100], self.cmd('cms.incrby', 'cms', 'a', '100'))
        self.assertEqual([60], self.cmd('cms.incrby', 'cms', 'a', '-40'))
        self.assertEqual([60], self.cmd('cms.query', 'cms', 'a'))
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 60, 'cell_size', 4],
                         self.cmd('cms.info', 'cms'))

        # all the way down to zero
        self.assertEqual([0], self.cmd('cms.incrby', 'cms', 'a', '-60'))
        self.assertEqual([0], self.cmd('cms.query', 'cms', 'a'))
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'cms'))

        # a zero increment is a no-op that reports the current estimate
        self.assertEqual([0], self.cmd('cms.incrby', 'cms', 'a', '0'))

    def test_decrby_underflow(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '1000', '5'))
        self.assertEqual([10], self.cmd('cms.incrby', 'cms', 'a', '10'))

        # one past what was added
        res = self.cmd('cms.incrby', 'cms', 'a', '-11')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
        self.assertEqual([10], self.cmd('cms.query', 'cms', 'a'))
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 10, 'cell_size', 4],
                         self.cmd('cms.info', 'cms'))

        # an item that was never added
        res = self.cmd('cms.incrby', 'cms', 'never_added', '-1')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
        self.assertEqual([0], self.cmd('cms.query', 'cms', 'never_added'))

        # failures do not stop the other pairs in the same command
        res = self.cmd('cms.incrby', 'cms', 'a', '-100', 'b', '3', 'a', '-4')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
        self.assertEqual(res[1:], [3, 6])
        self.assertEqual([6, 3], self.cmd('cms.query', 'cms', 'a', 'b'))
        self.assertEqual(['width', 1000, 'depth', 5, 'count', 9, 'cell_size', 4],
                         self.cmd('cms.info', 'cms'))

    def test_decrby_cell_sizes(self):
        for cell_size in (1, 2, 4, 8):
            self.cmd('FLUSHALL')
            self.assertOk(self.cmd('cms.initbydim', 'cms', '1', '1', 'CELL_SIZE', cell_size))
            self.assertEqual([200], self.cmd('cms.incrby', 'cms', 'a', '200'))
            self.assertEqual([50], self.cmd('cms.incrby', 'cms', 'a', '-150'))
            res = self.cmd('cms.incrby', 'cms', 'a', '-51')
            self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
            self.assertEqual([50], self.cmd('cms.query', 'cms', 'a'))

    def test_decrby_validation(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '20', '5'))
        # LLONG_MIN cannot be negated
        self.env.expect('cms.incrby', 'cms', 'a', '-9223372036854775808') \
            .error().contains('CMS: invalid increment')
        self.env.expect('cms.incrby', 'cms', 'a', 'blah') \
            .error().contains('CMS: Cannot parse number')

    def test_merge_cell_size(self):
        self.cmd('FLUSHALL')
        for name in ('a', 'b', 'dest'):
            self.assertOk(self.cmd('cms.initbydim', name + '{t}', '20', '5', 'CELL_SIZE', '2'))
        self.assertEqual([10], self.cmd('cms.incrby', 'a{t}', 'x', '10'))
        self.assertEqual([7], self.cmd('cms.incrby', 'b{t}', 'x', '7'))
        self.assertOk(self.cmd('cms.merge', 'dest{t}', '2', 'a{t}', 'b{t}'))
        self.assertEqual([17], self.cmd('cms.query', 'dest{t}', 'x'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 17, 'cell_size', 2],
                         self.cmd('cms.info', 'dest{t}'))

        # merging past the destination cell maximum is rejected
        self.assertOk(self.cmd('cms.merge', 'dest{t}', '2', 'a{t}', 'b{t}', 'WEIGHTS', '1000', '1'))
        self.env.expect('cms.merge', 'dest{t}', '2', 'a{t}', 'b{t}',
                        'WEIGHTS', '10000', '1').error().contains('CMS: MERGE overflow')

    def test_merge_cell_size_mismatch(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'small{t}', '20', '5', 'CELL_SIZE', '1'))
        self.assertOk(self.cmd('cms.initbydim', 'large{t}', '20', '5', 'CELL_SIZE', '4'))
        self.env.expect('cms.merge', 'large{t}', '1', 'small{t}') \
            .error().contains('CMS: cell size is not equal')
        self.env.expect('cms.merge', 'small{t}', '1', 'large{t}') \
            .error().contains('CMS: cell size is not equal')

    def test_total_count_overflow(self):
        # the total count is a RESP integer too, so it is capped at INT64_MAX even
        # when the item's own cells still have room
        maximum = 2 ** 63 - 1
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '100', '1', 'CELL_SIZE', '8'))
        self.assertEqual([maximum], self.cmd('cms.incrby', 'cms', 'a', maximum))

        # an item that landed in a different cell, so its own cell is empty
        other = next(item for item in ('x{}'.format(i) for i in range(1000))
                     if self.cmd('cms.query', 'cms', item) == [0])
        res = self.cmd('cms.incrby', 'cms', other, '1')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')
        self.assertEqual([0], self.cmd('cms.query', 'cms', other))
        self.assertEqual(['width', 100, 'depth', 1, 'count', maximum, 'cell_size', 8],
                         self.cmd('cms.info', 'cms'))

    def _raw_connection(self):
        # DUMP payloads are binary, so they need a connection that does not decode
        return SimpleNamespace(execute_command=self.env.server.client.command)

    def test_rdb_load_rejects_out_of_range_counter(self):
        # A total count above INT64_MAX cannot be produced by any command. Loading one
        # would wrap the `INT64_MAX - counter` headroom check in CMS_IncrBy, letting
        # the total escape the RESP integer range that check exists to protect.
        self.cmd('FLUSHALL')
        raw = self._raw_connection()
        self.assertOk(self.cmd('cms.initbydim', 'src', '20', '5'))
        self.assertEqual([7], self.cmd('cms.incrby', 'src', 'a', '7'))

        bad = rewrite_module_uint(raw.execute_command('DUMP', 'src'), 2, 2 ** 63)
        self.env.expect('RESTORE', 'bad', 0, bad).error().contains('Bad data format')

    def test_rdb_load_rejects_out_of_range_cell(self):
        # Only CELL_SIZE 8 can store a cell above its maximum, since CMS_CELL_MAX(8) is
        # INT64_MAX while the slot holds a full uint64. Such a cell would wrap the
        # `cellMax - cell` headroom check and make CMS.QUERY reply a negative count.
        self.cmd('FLUSHALL')
        raw = self._raw_connection()
        self.assertOk(self.cmd('cms.initbydim', 'src', '20', '5', 'CELL_SIZE', '8'))
        self.assertEqual([7], self.cmd('cms.incrby', 'src', 'a', '7'))

        cell_count = 20 * 5
        cells = struct.pack('<%dQ' % cell_count, *([2 ** 63] * cell_count))
        bad = rewrite_largest_module_string(raw.execute_command('DUMP', 'src'),
                                           lambda _old: cells, require_same_length=True)
        self.env.expect('RESTORE', 'bad', 0, bad).error().contains('Bad data format')

    def test_incrby_rejection_undoes_partial_writes(self):
        self.cmd('FLUSHALL')
        self.assertOk(self.cmd('cms.initbydim', 'cms', '2', '4', 'CELL_SIZE', '1'))
        self.assertEqual([200], self.cmd('cms.incrby', 'cms', 'filler', '200'))

        before_a = self.cmd('cms.query', 'cms', 'a')
        before_filler = self.cmd('cms.query', 'cms', 'filler')

        res = self.cmd('cms.incrby', 'cms', 'a', '100')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')

        self.assertEqual(before_a, self.cmd('cms.query', 'cms', 'a'))
        self.assertEqual(before_filler, self.cmd('cms.query', 'cms', 'filler'))
        self.assertEqual(['width', 2, 'depth', 4, 'count', 200, 'cell_size', 1],
                         self.cmd('cms.info', 'cms'))

        # the sketch is still fully usable afterwards
        self.assertEqual([201], self.cmd('cms.incrby', 'cms', 'filler', '1'))

    def test_incrby_deep_sketches(self):
        # Deep sketches walk many rows per operation; increments, decrements and
        # rejected operations must behave the same at any depth.
        for depth in (63, 64, 65, 130):
            self.cmd('FLUSHALL')
            key = 'd{}'.format(depth)
            self.assertOk(self.cmd('cms.initbydim', key, '50', depth))
            self.assertEqual([10], self.cmd('cms.incrby', key, 'a', '10'))
            self.assertEqual([10], self.cmd('cms.query', key, 'a'))

            # decrement, then a rejected decrement must leave everything untouched
            self.assertEqual([4], self.cmd('cms.incrby', key, 'a', '-6'))
            res = self.cmd('cms.incrby', key, 'a', '-5')
            self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
            self.assertEqual([4], self.cmd('cms.query', key, 'a'))
            self.assertEqual(['width', 50, 'depth', depth, 'count', 4, 'cell_size', 4],
                             self.cmd('cms.info', key))

            # a rejected overflow must leave everything untouched too
            self.assertOk(self.cmd('cms.initbydim', 'small', '50', depth, 'CELL_SIZE', '1'))
            self.assertEqual([255], self.cmd('cms.incrby', 'small', 'b', '255'))
            res = self.cmd('cms.incrby', 'small', 'b', '1')
            self.env.assertResponseError(res[0], contained='CMS: INCRBY overflow')
            self.assertEqual([255], self.cmd('cms.query', 'small', 'b'))

    def test_incrby_underflow_corrupt_counter(self):
        # A sketch whose total count disagrees with its cell array cannot be reached by
        # any sequence of commands - only by a crafted RESTORE, since the load path
        # validates the dimensions and the buffer length but never counter against the
        # array. Decrementing such a sketch must be refused, not wrap the counter.
        self.cmd('FLUSHALL')
        raw = SimpleNamespace(execute_command=self.env.server.client.command)

        self.assertOk(self.cmd('cms.initbydim', 'honest', '20', '5'))
        self.assertEqual([7], self.cmd('cms.incrby', 'honest', 'a', '7'))

        # CMSRdbSave writes width, depth, counter, cellSize -> counter is UINT #2
        bad = rewrite_module_uint(raw.execute_command('DUMP', 'honest'), 2, 0)
        raw.execute_command('RESTORE', 'bad', 0, bad)

        # the cells still hold 7 while the total count claims 0
        self.assertEqual(['width', 20, 'depth', 5, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'bad'))
        self.assertEqual([7], self.cmd('cms.query', 'bad', 'a'))

        # every cell has exactly enough for this decrement, so only the counter check
        # can reject it - this is what pins the counter guard in CMS_IncrBy
        res = self.cmd('cms.incrby', 'bad', 'a', '-7')
        self.env.assertResponseError(res[0], contained='CMS: INCRBY underflow')
        self.assertEqual([7], self.cmd('cms.query', 'bad', 'a'))
        self.assertEqual(['width', 20, 'depth', 5, 'count', 0, 'cell_size', 4],
                         self.cmd('cms.info', 'bad'))

        # the honest sketch is unaffected and still decrements normally
        self.assertEqual([0], self.cmd('cms.incrby', 'honest', 'a', '-7'))

    def test_rdb_load_encver_0(self):
        # A CMS.INITBYDIM 20 5 sketch with 'a' incremented by 7 and 'b' by 3,
        # dumped by a module version that predates the CELL_SIZE support
        # (encoding version 0). It must load as a 4-byte-cell sketch.
        self.cmd('FLUSHALL')
        rdb_payload = b"\x07\x81\x08\xc4\xa4\xf96\x0f\x10\x00\x02\x14\x02\x05\x02\n\x05\xc34A\x90\x01\x00\x00\xe0-\x00\x00\x07\xa06\x00\x03\xa0\x07\xe0\x03\x00\xe0\x03\x13\xc0'\xe03\x00\xe0\x03C\xe0\x03[\xe0c\x00\xc0w\xe0\x03\x8b\xe0+\x00\xc0G\xe0\x07\x00\x80W\x01\x00\x00\x00\x0f\x00\xf8\xb3|^\xc6\xb6\xadO"
        self.env.server.client.command('RESTORE', 'legacy', 0, rdb_payload)
        self.assertEqual(['width', 20, 'depth', 5, 'count', 10, 'cell_size', 4],
                         self.cmd('cms.info', 'legacy'))
        self.assertEqual([7, 3], self.cmd('cms.query', 'legacy', 'a', 'b'))
        # and it takes part in the new operations
        self.assertEqual([5], self.cmd('cms.incrby', 'legacy', 'a', '-2'))

    def test_rdb_load_overflow(self):
        self.cmd('FLUSHALL')
        rdb_payload = b'\x07\x81\x08\xc4\xa4\xf96\x0f\x10\x00\x02D\x00\x02\x01\x02\x00\x05B\xbbXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\x00\xff\x0c\x00\x12C\xd6\xb4\xacP&\xa2'
        self.env.expect('RESTORE', "key", 0, rdb_payload, 'REPLACE').error().contains('Bad data format')
