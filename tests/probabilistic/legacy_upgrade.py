"""Real server-restart upgrades from external RedisBloom, without pip dependencies."""
import os
import time
from pathlib import Path
from support import Server

def upgrade(env, create, snapshot, mutate):
    binary=os.environ.get('BLOOM_ORACLE_SERVER')
    module=os.environ.get('BLOOM_ORACLE_MODULE')
    if not binary or not module:
        env.skip('Upgrade tests require BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE')
    native=os.environ.get('REDIS_SERVER', str(Path(__file__).resolve().parents[2]/'src/redis-server'))
    for mode in ('rdb','aof','aof-rdb'):
        options=() if mode=='rdb' else ('--appendonly','yes','--aof-use-rdb-preamble','yes' if mode=='aof-rdb' else 'no')
        s=Server(binary=binary,extra=(*options,'--loadmodule',module))
        try:
            r=s.client.command
            before=[]
            # Exercise DB selection, empty/populated keys, absolute expiry and ordinary keys.
            for db in (0,1):
                r('SELECT',db)
                create(r)
                r('SET','ordinary','kept')
                r('PEXPIRE','subject',3600000)
                before.append((snapshot(r),r('PEXPIRETIME','subject'),r('DBSIZE')))
            r('SELECT',0)
            if mode=='rdb': r('SAVE')
            else:
                # Upgrade with writes quiesced and a completed rewrite. In particular,
                # legacy Top-K randomized commands in an incremental AOF are not an
                # exact-state representation; the module rewrite serializes the state.
                r('BGREWRITEAOF')
                for _ in range(800):
                    info=r('INFO','persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info: break
                    time.sleep(.025)
                else: raise AssertionError('Old-server AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
            s.stop()
            s.binary=native
            s.extra=options
            s.start()
            r=s.client.command
            names=[dict(zip(m[::2],m[1::2]))[b'name'] for m in r('MODULE','LIST')]
            env.assertNotIn(b'bf',names)
            for db in (0,1):
                r('SELECT',db)
                env.assertEqual(before[db],(snapshot(r),r('PEXPIRETIME','subject'),r('DBSIZE')))
                env.assertEqual(b'kept',r('GET','ordinary'))
                mutate(r)
            r('SAVE')
            s.stop(); s.start()
            for db in (0,1):
                s.client.command('SELECT',db)
                env.assertEqual(b'kept',s.client.command('GET','ordinary'))
                env.assertEqual(before[db][1],s.client.command('PEXPIRETIME','subject'))
        finally: s.close()
