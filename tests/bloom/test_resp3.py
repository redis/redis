"""Bloom portion of RedisBloom v8.11.81 tests/flow/test_resp3.py."""
from support import *

class testResp3:
    def __init__(self):
        self.env = Env(protocol=3)

    def test_bf_resp3(self):
        env = self.env
        env.cmd('FLUSHALL')
        res = env.cmd('bf.add', 'test', 'item')
        assert type(res) == bool
        assert res == True

        res = env.cmd('bf.card', 'test')
        assert res == 1

        res = env.cmd('bf.EXISTS', 'test', 'item')
        assert type(res) == bool
        assert res == True

        res = env.cmd('bf.info', 'test')
        assert res == {b'Capacity': 100, b'Size': 240, b'Number of filters': 1,
            b'Number of items inserted': 1, b'Expansion rate': 2}

        res = env.cmd('bf.insert', 'test', 'ITEMS', 'item2', 'item3', 'item2')
        assert type(res[0]) == bool
        assert res == [True, True, False]

        res = env.cmd('bf.madd', 'test', 'item4', 'item5', 'item2')
        assert type(res[0]) == bool
        assert res == [True, True, False]

        res = env.cmd('bf.MEXISTS', 'test', 'item4', 'item5', 'item6')
        assert type(res[0]) == bool
        assert res == [True, True, False]
