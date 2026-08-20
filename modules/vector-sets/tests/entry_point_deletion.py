from test import TestCase, generate_random_vector
import struct
import random

"""
Regression test for the entry point replacement bug of issue #15424.

When the deleted node was the entry point, the replacement used to be
picked from the deleted node links, descending from the top layer until
some layer with links was found. If the upper layers of the entry point
were left without links (a state that small sets under insertion and
deletion churn can reach), the replacement could be a node of lower
level than the tallest node still in the graph, leaving
index->max_level too low. From there, deleting one of the taller nodes
made hnsw_reconnect_nodes() search a layer above the entry point level,
reading past the end of the entry point allocation and possibly
crashing the server inside select_neighbors() (see the issue for the
production stack trace).

The buggy state needs a rare combination of deletion order and node
levels (levels are drawn server side, so no fixed command sequence can
reproduce it deterministically). This test churns many small sets with
a low M (sparse upper layers erode much faster) and, instead of
waiting for the eventual crash, checks after every single VREM the
invariant that the bug breaks: the max-level reported by VINFO must
always match the level of the tallest element, known from VLINKS at
insertion time. Any run of the buggy code that enters the bad state
fails here immediately, long before the crash; with the fix the
invariant holds on every deletion.
"""

class EntryPointDeletion(TestCase):
    def getname(self):
        return "Entry point deletion keeps max-level correct (issue #15424)"

    def estimated_runtime(self):
        return 15

    def max_level(self):
        info = self.redis.execute_command('VINFO', self.test_key)
        if not isinstance(info, dict):
            info = {info[i]: info[i+1] for i in range(0, len(info), 2)}
        info = {k.decode() if isinstance(k, bytes) else k: v
                for k, v in info.items()}
        return int(info['max-level'])

    def test(self):
        random.seed(42)
        dim = 8
        num_sets = 60
        set_size = 80

        for s in range(num_sets):
            self.redis.delete(self.test_key)
            levels = {}
            for i in range(set_size):
                name = f"item:{i}"
                vec = generate_random_vector(dim)
                vec_bytes = struct.pack(f'{dim}f', *vec)
                self.redis.execute_command('VADD', self.test_key, 'FP32',
                                           vec_bytes, name, 'M', '4')
                # The number of layers reported by VLINKS is level+1.
                links = self.redis.execute_command('VLINKS', self.test_key,
                                                   name)
                levels[name] = len(links) - 1

            # Drain the set in insertion order, checking after every
            # deletion that the graph max level still matches the
            # tallest element left.
            for i in range(set_size):
                name = f"item:{i}"
                assert self.redis.execute_command('VREM', self.test_key,
                                                  name) == 1
                del levels[name]
                if not levels:
                    break
                reported = self.max_level()
                expected = max(levels.values())
                assert reported == expected, \
                    f"set {s}: after deleting {name} VINFO reports " \
                    f"max-level {reported} but the tallest element " \
                    f"has level {expected}: the entry point was " \
                    f"replaced by a node below the graph max level"

        assert self.redis.ping()
