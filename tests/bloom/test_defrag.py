import time
from common import *

from RLTest import Defaults

def enableDefrag(env):
    version = env.cmd('info', 'server')['redis_version']
    if '6.0' in version:
        # skil on version 6.0 and defrag API for modules was not supported on this version.
        env.skip()

    # make defrag as aggressive as possible
    env.cmd('CONFIG', 'SET', 'hz', '100')
    env.cmd('CONFIG', 'SET', 'active-defrag-ignore-bytes', '1')
    env.cmd('CONFIG', 'SET', 'active-defrag-threshold-lower', '0')
    env.cmd('CONFIG', 'SET', 'active-defrag-cycle-min', '99')

    try:
        env.cmd('CONFIG', 'SET', 'activedefrag', 'yes')
    except Exception:
        # If active defrag is not supported by the current Redis, simply skip the test.
        env.skip()

def testDefrag(env):
    if VALGRIND:
        env.skip()
    enableDefrag(env)

    # Disable defrag so we can actually create fragmentation
    env.cmd('CONFIG', 'SET', 'activedefrag', 'no')
    
    # Match the heap pressure of the original mixed-type test using only Bloom.
    key_count = 100000
    for i in range(key_count):
        env.expect('bf.add', 'bf%d' % i, 'k1').equal(1)
    
    # Delete keys at even position
    for i in range(0, key_count, 2):
        env.expect('del', 'bf%d' % i).equal(1)

    # wait for fragmentation for up to 30 seconds
    frag = env.cmd('info', 'memory')['allocator_frag_ratio']
    startTime = time.time()
    while frag < 1.4:
        time.sleep(0.1)
        frag = env.cmd('info', 'memory')['allocator_frag_ratio']
        if time.time() - startTime > 30:
            # We will wait for up to 30 seconds and then we consider it a failure
            env.assertTrue(False, message='Failed waiting for fragmentation, current value %s which is expected to be above 1.4.' % frag)
            return

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
            env.assertTrue(False, message='Failed waiting for fragmentation to go down, current value %s which is expected to be bellow 1.1.' % frag)
            return

    for i in range(1, key_count, 2):
        env.expect('bf.exists', 'bf%d' % i, 'k1').equal(1)
