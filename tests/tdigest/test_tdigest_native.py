import math
import os
import time
from support import *
from upgrade import upgrade

class testNativeTDigest:
    def test_upgrade_existing_keys(self):
        def create(r):
            r('TDIGEST.CREATE','subject')
            r('TDIGEST.CREATE','empty')
            r('TDIGEST.ADD','subject',*range(1000))
        def snapshot(r):
            return [r('TDIGEST.QUANTILE','subject',0,.5,1),r('TDIGEST.CDF','subject',500),
                    r('TDIGEST.MIN','empty'),r('TDIGEST.INFO','empty')]
        def mutate(r):
            r('TDIGEST.ADD','subject',2000)
            assert float(r('TDIGEST.MAX','subject'))==2000
        upgrade(Env(),create,snapshot,mutate)

    def test_lifecycle_and_no_query_mutation(self):
        env=Env()
        r=env.cmd
        r('TDIGEST.CREATE','t')
        r('TDIGEST.ADD','t',*range(100))
        env.assertEqual(b'tdigest',r('TYPE','t'))
        before=r('DUMP','t')
        r('TDIGEST.QUANTILE','t',.5)
        r('TDIGEST.CDF','t',50)
        env.assertEqual(before,r('DUMP','t'))
        env.assertEqual(1,r('COPY','t','copy'))
        env.assertEqual(before,r('DUMP','copy'))
        r('TDIGEST.ADD','copy',1000)
        env.assertEqual(99,float(r('TDIGEST.MAX','t')))
        env.dumpAndReload()
        env.assertEqual(before,r('DUMP','t'))
        env.assertEqual(1000,float(r('TDIGEST.MAX','copy')))
        env.assertEqual(1,r('UNLINK','copy'))
        r('PEXPIRE','t',1); time.sleep(.02)
        env.assertEqual(0,r('EXISTS','t'))

    def test_replication_read_interleaving(self):
        env=Env(useSlaves=True)
        r=env.cmd
        replica=env.getSlaveConnection().execute_command
        r('TDIGEST.CREATE','t')
        for i in range(50):
            r('TDIGEST.ADD','t',i,i*2)
            r('TDIGEST.QUANTILE','t',.5)
        env.assertEqual(1,r('WAIT',1,5000))
        env.assertEqual(r('DUMP','t'),replica('DUMP','t'))
        r('TDIGEST.MERGE','t',1,'t')
        env.assertEqual(1,r('WAIT',1,5000))
        env.assertEqual(r('DUMP','t'),replica('DUMP','t'))

    def test_aof(self):
        for preamble in ('no','yes'):
            s=Server(extra=('--appendonly','yes','--aof-use-rdb-preamble',preamble))
            try:
                r=s.client.command
                r('TDIGEST.CREATE','t'); r('TDIGEST.ADD','t',*range(100))
                r('BGREWRITEAOF')
                for _ in range(400):
                    info=r('INFO','persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info: break
                    time.sleep(.025)
                else: raise AssertionError('AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
                r('TDIGEST.ADD','t',1000)
                expected=r('DUMP','t')
                s.stop(); s.start()
                assert expected==s.client.command('DUMP','t')
            finally: s.close()

    def test_resp3_acl_metadata(self):
        env=Env(protocol=3)
        r=env.cmd
        r('TDIGEST.CREATE','t')
        env.assertTrue(math.isnan(r('TDIGEST.MIN','t')))
        r('TDIGEST.ADD','t',1,2,3)
        env.assertEqual([2.0],r('TDIGEST.QUANTILE','t',.5))
        env.assertEqual(3,r('TDIGEST.INFO','t')[b'Observations'])
        env.assertEqual(14,len(r('ACL','CAT','tdigest')))
        env.assertEqual([b'dst',b'a',b'b'],r('COMMAND','GETKEYS','TDIGEST.MERGE','dst',2,'a','b'))
        r('ACL','SETUSER','reader','on','>secret','~t','+@tdigest','-@write')
        r('AUTH','reader','secret')
        env.assertEqual(1.0,r('TDIGEST.MIN','t'))
        env.expect('TDIGEST.ADD','t',4).error().contains('NOPERM')
