from docs_utils import *

class testCommandDocsAndHelp:
    def __init__(self):
        self.env = Env(decodeResponses=True)


    def test_command_docs_bf_add(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.add',
            summary="Adds an item to a Bloom Filter",
            complexity='O(k * f), where k is the number of hash functions and f is the number of sub-filters.',
            arity=3,
            since='8.12.0',
            args=[('key', 'key'), ('item', 'string')],
            key_pos=1,
        )


    def test_command_docs_bf_exists(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.exists',
            summary='Checks whether an item exists in a Bloom Filter',
            complexity='O(k * f), where k is the number of hash functions and f is the number of sub-filters.',
            arity=3,
            since='8.12.0',
            args=[('key', 'key'), ('item', 'string')],
            key_pos=1,
        )


    def test_command_docs_bf_info(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.info',
            summary='Returns information about a Bloom Filter',
            complexity='O(f), where f is the number of sub-filters.',
            arity=-2,
            since='8.12.0',
            args=[('key', 'key'), ('single-value', 'oneof', [('capacity', 'pure-token', 'CAPACITY'), ('size', 'pure-token', 'SIZE'), ('filters', 'pure-token', 'FILTERS'), ('items', 'pure-token', 'ITEMS'), ('expansion', 'pure-token', 'EXPANSION')])],
            key_pos=1,
        )


    def test_command_docs_bf_card(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.card',
            summary='Returns the cardinality of a Bloom filter',
            complexity='O(1)',
            arity=2,
            since='8.12.0',
            args=[('key', 'key')],
            key_pos=1,
        )


    def test_command_docs_bf_insert(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        
        assert_docs(
            env,
            "bf.insert",
            summary="Adds one or more items to a Bloom Filter. A filter will be created if it does not exist",
            complexity="O(n * k * f), where n is the number of items, k is the number of hash functions and f is the number of sub-filters.",
            arity=-4,
            since="8.12.0",
            args=[("key", "key"), ("capacity", "block"), ("error", "block"), ("expansion", "block"), ("nocreate", "pure-token"), ("nonscaling", "pure-token"), ("items", "pure-token"), ("item", "string")],
            key_pos=1,
        )


    def test_command_docs_bf_loadchunk(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.loadchunk',
            summary='Restores a filter previously saved using SCANDUMP',
            complexity='O(n), where n is the capacity',
            arity=4,
            since='8.12.0',
            args=[('key', 'key'), ('iterator', 'integer'), ('data', 'string')],
            key_pos=1,
        )


    def test_command_docs_bf_madd(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.madd',
            summary='Adds one or more items to a Bloom Filter. A filter will be created if it does not exist',
            complexity='O(n * k * f), where n is the number of items, k is the number of hash functions and f is the number of sub-filters.',
            arity=-3,
            since='8.12.0',
            args=[('key', 'key'), ('item', 'string')],
            key_pos=1,
        )


    def test_command_docs_bf_mexists(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.mexists',
            summary='Checks whether one or more items exist in a Bloom Filter',
            complexity='O(n * k * f), where n is the number of items, k is the number of hash functions and f is the number of sub-filters.',
            arity=-3,
            since='8.12.0',
            args=[('key', 'key'), ('item', 'string')],
            key_pos=1,
        )


    def test_command_docs_bf_reserve(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.reserve',
            summary='Creates a new Bloom Filter',
            complexity='O(n), where n is the capacity.',
            arity=-4,
            since='8.12.0',
            args=[('key', 'key'), ('error-rate', 'double'), ('capacity', 'integer'), ('expansion', 'block'), ('nonscaling', 'pure-token')],
            key_pos=1,
        )


    def test_command_docs_bf_scandump(self):
        env = self.env
        if server_version_less_than(env, '7.0.0'):
            env.skip()
        assert_docs(
            env, 'bf.scandump',
            summary='Begins an incremental save of the bloom filter',
            complexity='O(n), where n is the capacity',
            arity=3,
            since='8.12.0',
            args=[('key', 'key'), ('iterator', 'integer')],
            key_pos=1,
        )


