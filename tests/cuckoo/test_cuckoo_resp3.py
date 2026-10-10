"""Cuckoo cases migrated from RedisBloom test_resp3.py."""
from support import *

class testCuckooResp3:
    def test_resp3(self):
        env = Env(protocol=3)
        env.cmd('CF.RESERVE a 9 EXPANSION 0')
        res = env.cmd('CF.ADD a 3')
        assert type(res) == bool
        env.assertEqual(res, True)
        res = env.cmd('CF.ADD a 4')
        assert type(res) == bool
        env.assertEqual(res, True)
        res = env.cmd('CF.ADDNX a 4')
        assert type(res) == bool
        env.assertEqual(res, False)
        res = env.cmd('CF.ADDNX a 9')
        assert type(res) == bool
        env.assertEqual(res, True)
        res = env.cmd('CF.INSERT a ITEMS 4 5')
        assert type(res[0]) == bool
        env.assertEqual(res, [True, True])
        res = env.cmd('CF.INSERTNX a ITEMS 4 6')
        assert type(res[0]) == int
        env.assertEqual(res, [0, 1])
        res = env.cmd('CF.del a 3')
        assert type(res) == bool
        env.assertEqual(res, True)
        res = env.cmd('CF.del a 3')
        assert type(res) == bool
        env.assertEqual(res, False)
        res = env.cmd('CF.EXISTS a 3')
        assert type(res) == bool
        env.assertEqual(res, False)
        res = env.cmd('CF.EXISTS a 4')
        assert type(res) == bool
        env.assertEqual(res, True)
        res = env.cmd('CF.MEXISTS a 4 3')
        assert type(res[0]) == bool
        env.assertEqual(res, [True, False])
        res = env.cmd('CF.COUNT a 3')
        assert type(res) == int
        env.assertEqual(res, 0)
        res = env.cmd('CF.COUNT a 4')
        assert type(res) == int
        env.assertEqual(res, 2)

        res = env.cmd('cf.info a')
        assert res == {b'Size': 64, b'Number of buckets': 4,
                        b'Number of filters': 1, b'Number of items inserted': 5,
                        b'Number of items deleted': 1, b'Bucket size': 2,
                        b'Expansion rate': 0, b'Max iterations': 20}
