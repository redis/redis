"""Native Top-K lifecycle and exact-state propagation regressions."""
import os
import time
from support import *

class testNativeTopK:
    def test_zero_count_empty_item_survives_reload(self):
        env=Env()
        env.cmd('TOPK.RESERVE','t',3)
        env.cmd('TOPK.INCRBY','t','',0)
        env.assertEqual([1],env.cmd('TOPK.QUERY','t',''))
        env.dumpAndReload()
        env.assertEqual([1],env.cmd('TOPK.QUERY','t',''))

    def test_lifecycle_binary(self):
        env=Env()
        r=env.cmd
        r('TOPK.RESERVE','t',3)
        r('TOPK.ADD','t',b'a\x00b','')
        env.assertEqual(b'topk',r('TYPE','t'))
        env.assertEqual(1,r('COPY','t','copy'))
        env.assertEqual(r('DUMP','t'),r('DUMP','copy'))
        r('TOPK.INCRBY','copy',b'a\x00b',4)
        env.assertEqual([1],r('TOPK.COUNT','t',b'a\x00b'))
        env.dumpAndReload()
        env.assertEqual([5],r('TOPK.COUNT','copy',b'a\x00b'))
        env.assertIn(b'a\x00b',r('TOPK.LIST','copy'))
        env.assertEqual(1,r('UNLINK','copy'))
        r('PEXPIRE','t',1)
        time.sleep(.02)
        env.assertEqual(0,r('EXISTS','t'))

    def test_replication_randomized_partial_and_ttl(self):
        env=Env(useSlaves=True)
        r=env.cmd
        replica=env.getSlaveConnection().execute_command
        r('TOPK.RESERVE','t',3,5,3,.9)
        r('PEXPIRE','t',60000)
        expire=r('PEXPIRETIME','t')
        for i in range(100):
            r('TOPK.ADD','t',i%13,i%7)
        result=r('TOPK.INCRBY','t','valid',3,'invalid',-1,'last',2)
        env.assertTrue(isinstance(result[1],ResponseError))
        env.assertEqual(1,r('WAIT',1,5000))
        env.assertEqual(r('DUMP','t'),replica('DUMP','t'))
        env.assertEqual(expire,replica('PEXPIRETIME','t'))

    def test_aof_exact_state(self):
        for preamble in ('no','yes'):
            s=Server(extra=('--appendonly','yes','--aof-use-rdb-preamble',preamble))
            try:
                r=s.client.command
                r('TOPK.RESERVE','t',3,5,3,.9)
                for i in range(50): r('TOPK.ADD','t',i%11)
                r('BGREWRITEAOF')
                for _ in range(400):
                    info=r('INFO','persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info: break
                    time.sleep(.025)
                else: raise AssertionError('AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
                for i in range(50): r('TOPK.INCRBY','t',i%17,3)
                before=r('DUMP','t')
                s.stop(); s.start()
                assert before==s.client.command('DUMP','t')
            finally: s.close()

    def test_metadata_resp3(self):
        env=Env()
        r=env.cmd
        r('TOPK.RESERVE','t',3)
        r('TOPK.ADD','t','a')
        env.assertEqual(7,len(r('ACL','CAT','topk')))
        env.assertEqual([b't'],r('COMMAND','GETKEYS','TOPK.INCRBY','t','a',1))
        r('HELLO',3)
        env.assertEqual({b'k':3,b'width':8,b'depth':7,b'decay':.9},r('TOPK.INFO','t'))
        env.assertEqual([True,False],r('TOPK.QUERY','t','a','missing'))
        r('ACL','SETUSER','reader','on','>secret','~t','+@topk','-@write')
        r('AUTH','reader','secret')
        env.assertEqual([True],r('TOPK.QUERY','t','a'))
        env.expect('TOPK.ADD','t','b').error().contains('NOPERM')

    def test_external_compatibility(self):
        env=Env()
        binary=os.environ.get('BLOOM_ORACLE_SERVER')
        module=os.environ.get('BLOOM_ORACLE_MODULE')
        if not binary or not module: env.skip('Set the external RedisBloom oracle paths')
        oracle=Server(binary=binary,extra=('--loadmodule',module))
        try:
            for r in (env.cmd,oracle.client.command):
                r('TOPK.RESERVE','t',3)
                r('TOPK.INCRBY','t','a',3,'b',2)
            env.cmd('RESTORE','copy',0,oracle.client.command('DUMP','t'))
            oracle.client.command('RESTORE','copy',0,env.cmd('DUMP','t'))
            for r in (env.cmd,oracle.client.command):
                env.assertEqual([b'a',3,b'b',2],r('TOPK.LIST','copy','WITHCOUNT'))
        finally: oracle.close()
