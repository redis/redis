"""Bloom portion of RedisBloom v8.11.81 tests/flow/test_defrag.py."""
import time
from support import *

def enableDefrag(env):
    # make defrag as aggressive as possible
    env.cmd('CONFIG', 'SET', 'hz', '100')
    env.cmd('CONFIG', 'SET', 'active-defrag-ignore-bytes', '1')
    env.cmd('CONFIG', 'SET', 'active-defrag-threshold-lower', '0')
    env.cmd('CONFIG', 'SET', 'active-defrag-cycle-min', '99')
    env.cmd('CONFIG', 'SET', 'active-defrag-cycle-max', '99')

    try:
        env.cmd('CONFIG', 'SET', 'activedefrag', 'yes')
    except ResponseError as error:
        # If active defrag is not supported by the current Redis, simply skip the test.
        if 'requires a Redis server compiled with' not in str(error):
            raise
        env.skip('active defragmentation requires jemalloc')

def testDefrag(env):
    if VALGRIND:
        env.skip()
    enableDefrag(env)

    # Disable defrag so we can actually create fragmentation
    env.cmd('CONFIG', 'SET', 'activedefrag', 'no')
    # As in the core defrag tests, keep lookahead allocations from changing
    # the allocation order and fragmentation of the workload.
    env.cmd('CONFIG', 'SET', 'lookahead', '1')

    # Use 2 KiB bitmaps so Bloom allocations dominate the server's fixed
    # allocator overhead. Tiny default filters cannot reliably reach 1.1.
    for i in range(10000):
        env.expect('bf.reserve', 'bf%d' % i, 0.01, 1000).ok()
        env.expect('bf.add', 'bf%d' % i, 'k1').equal(1)

    # Delete keys at even position
    for i in range(0, 10000, 2):
        env.expect('del', 'bf%d' % i).equal(1)

    # wait for fragmentation for up to 30 seconds
    frag = env.cmd('info', 'memory')['allocator_frag_ratio']
    startTime = time.time()
    while frag < 1.4:
        time.sleep(0.1)
        frag = env.cmd('info', 'memory')['allocator_frag_ratio']
        if time.time() - startTime > 30:
            # We will wait for up to 30 seconds and then we consider it a failure
            env.assertTrue(False, msg='Failed waiting for fragmentation, current value %s which is expected to be above 1.4.' % frag)
            return

    hits_before = env.cmd('info', 'stats')['active_defrag_hits']
    #enable active defrag
    env.cmd('CONFIG', 'SET', 'activedefrag', 'yes')

    # wait for fragmentation for go down for up to 30 seconds
    frag = env.cmd('info', 'memory')['allocator_frag_ratio']
    startTime = time.time()
    while frag > 1.1:
        time.sleep(0.1)
        frag = env.cmd('info', 'memory')['allocator_frag_ratio']
        if time.time() - startTime > 30:
            # We will wait for up to 30 seconds and then we consider it a failure
            env.fail('Failed waiting for fragmentation below 1.1: memory=%r stats=%r' %
                     (env.cmd('info', 'memory'), env.cmd('info', 'stats')))
            return

    env.assertGreater(env.cmd('info', 'stats')['active_defrag_hits'], hits_before)
    for i in range(1, 10000, 2):
        env.assertEqual(1, env.cmd('BF.EXISTS', 'bf%d' % i, 'k1'))

class testDefragmentation:
    def test_bloom_defrag(self):
        testDefrag(Env())
