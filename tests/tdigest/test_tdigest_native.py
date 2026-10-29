import math
import os
import time
from support import *
from legacy_upgrade import upgrade

class testNativeTDigest:
    def test_upgrade_existing_keys(self):
        def create(r):
            r('TDIGEST.CREATE','subject')
            r('TDIGEST.CREATE','empty')
            r('TDIGEST.ADD','subject',*range(1000))
        def snapshot(r):
            # Native query caching changes allocation size, not logical metadata.
            return [r('TDIGEST.QUANTILE','subject',0,.5,1),r('TDIGEST.CDF','subject',500),
                    r('TDIGEST.MIN','empty'),r('TDIGEST.INFO','empty')[:-2]]
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
        # Let the harness shut down normally after exercising the restricted user.
        r('AUTH','default','')

    def test_query_cache_lifecycle_and_memory(self):
        env=Env(protocol=3,extra=('--key-memory-histograms','yes'))
        r=env.cmd
        r('TDIGEST.CREATE','t','COMPRESSION',1000)
        r('TDIGEST.ADD','t',*range(1000))
        before=r('DUMP','t'); memory=r('MEMORY','USAGE','t')
        initial_info=r('TDIGEST.INFO','t')
        expected=r('TDIGEST.QUANTILE','t',.1,.5,.9)
        cached_memory=r('MEMORY','USAGE','t')
        assert memory<cached_memory<memory+20000
        assert r('TDIGEST.INFO','t')[b'Memory usage']>initial_info[b'Memory usage']
        for _ in range(10):
            assert r('TDIGEST.QUANTILE','t',.1,.5,.9)==expected
            r('TDIGEST.CDF','t',250,750)
            r('TDIGEST.TRIMMED_MEAN','t',.1,.9)
            assert r('MEMORY','USAGE','t')==cached_memory
            assert r('DUMP','t')==before
        # Invalid batches preserve both the sketch and its valid query view.
        env.expect('TDIGEST.ADD','t',10000,'invalid').error()
        assert r('DUMP','t')==before
        assert r('MEMORY','USAGE','t')==cached_memory
        r('COPY','t','copy')
        assert r('MEMORY','USAGE','copy')<cached_memory
        r('TDIGEST.ADD','t',*([10000]*2000))
        assert r('MEMORY','USAGE','t')==memory
        assert r('TDIGEST.QUANTILE','t',.5)[0]>expected[1]
        assert r('TDIGEST.QUANTILE','copy',.1,.5,.9)==expected
        # MERGE replaces the destination and must discard its cached snapshot.
        r('TDIGEST.MERGE','t',1,'copy','OVERRIDE')
        assert r('TDIGEST.QUANTILE','t',.1,.5,.9)==expected
        r('TDIGEST.RESET','t')
        assert math.isnan(r('TDIGEST.QUANTILE','t',.5)[0])
        assert r('MEMORY','USAGE','t')==memory
        r('TDIGEST.ADD','t',42)
        assert r('TDIGEST.QUANTILE','t',.5)==[42.0]
        env.dumpAndReload()
        assert r('TDIGEST.QUANTILE','t',.5)==[42.0]
        assert r('TDIGEST.QUANTILE','copy',.1,.5,.9)==expected

    def test_add_compression_boundary_is_atomic(self):
        env=Env(protocol=3)
        r=env.cmd
        r('TDIGEST.CREATE','t','COMPRESSION',1)
        # Capacity is 16; the next batch takes the transactional compression path.
        r('TDIGEST.ADD','t',*range(15))
        r('TDIGEST.QUANTILE','t',.5)
        before=r('DUMP','t')
        env.expect('TDIGEST.ADD','t',16,17,'inf').error()
        assert r('DUMP','t')==before
        r('TDIGEST.ADD','t',16,17,18)
        assert r('TDIGEST.INFO','t')[b'Observations']==18
        assert r('TDIGEST.QUANTILE','t',1)==[18.0]
