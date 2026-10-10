#!/usr/bin/env python3
"""Generate external-module fixtures, or verify existing-key upgrade without a module."""
import base64
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'bloom'))
from support import Server

EXPIRY=4102444800000  # 2100-01-01 UTC: stable fixture, not a relative TTL.
QUERIES=[
    ('BF.MEXISTS','bf','present','absent'), ('BF.CARD','bf-empty'),
    ('CMS.QUERY','cms','present'), ('CMS.QUERY','cms-empty','present'),
    ('CF.COUNT','cf','present'), ('CF.COUNT','cf-empty','present'),
    ('TOPK.LIST','topk','WITHCOUNT'), ('TOPK.LIST','topk-empty'),
    ('TDIGEST.QUANTILE','td',0,.5,1), ('TDIGEST.MIN','td-empty'),
    ('GET','ordinary'),
]
TYPES={'bf':'bloom','cms':'cms','cf':'cuckoo','topk':'topk','td':'tdigest'}

def normalize(v):
    if isinstance(v,bytes): return v.decode()
    if isinstance(v,list): return [normalize(x) for x in v]
    return v

def populate(r):
    for suffix in ('','-empty'):
        r('BF.RESERVE','bf'+suffix,.01,100)
        r('CMS.INITBYDIM','cms'+suffix,100,5)
        r('CF.RESERVE','cf'+suffix,100)
        r('TOPK.RESERVE','topk'+suffix,3)
        r('TDIGEST.CREATE','td'+suffix)
    r('BF.ADD','bf','present')
    r('CMS.INCRBY','cms','present',12)
    r('CF.ADD','cf','present'); r('CF.ADD','cf','present')
    r('TOPK.INCRBY','topk','a',3,'b',2)
    r('TDIGEST.ADD','td',*range(100))
    r('SET','ordinary','kept')
    for key in TYPES: r('PEXPIREAT',key,EXPIRY)

def generate():
    binary=os.environ['BLOOM_ORACLE_SERVER']
    module=os.environ['BLOOM_ORACLE_MODULE']
    fixtures={'source':'RedisBloom v8.11.81, aarch64 little-endian, 64-bit heap ABI','cases':[]}
    for mode in ('rdb','aof','aof-rdb'):
        options=[] if mode=='rdb' else ['--appendonly','yes','--aof-use-rdb-preamble','yes' if mode=='aof-rdb' else 'no']
        s=Server(binary=binary,extra=(*options,'--loadmodule',module))
        try:
            r=s.client.command
            for db in (0,1):
                r('SELECT',db); populate(r)
            expected=[normalize(r(*q)) for q in QUERIES]
            r('SELECT',0)
            if mode=='rdb': r('SAVE')
            else:
                r('BGREWRITEAOF')
                for _ in range(800):
                    info=r('INFO','persistence')
                    if b'aof_rewrite_in_progress:0' in info and b'aof_rewrite_scheduled:0' in info: break
                    time.sleep(.025)
                else: raise AssertionError('AOF rewrite timed out')
                assert b'aof_last_bgrewrite_status:ok' in info
            s.stop()
            paths=[s.directory/'dump.rdb'] if mode=='rdb' else sorted((s.directory/'appendonlydir').iterdir())
            files={str(p.relative_to(s.directory)):base64.b64encode(p.read_bytes()).decode() for p in paths if p.is_file()}
            fixtures['cases'].append({'mode':mode,'options':options,'expected':expected,'files':files})
        finally: s.close()
    print(json.dumps(fixtures,indent=2))

def verify():
    fixtures=json.loads(Path(__file__).with_name('upgrade-fixtures.json').read_text())
    for case in fixtures['cases']:
        s=Server()
        try:
            s.stop()
            for name,data in case['files'].items():
                target=s.directory/name
                assert target.resolve().is_relative_to(s.directory.resolve())
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(base64.b64decode(data))
            s.extra=case['options']; s.start()
            r=s.client.command
            for db in (0,1):
                r('SELECT',db)
                assert r('DBSIZE')==11,(case['mode'],db,'key count')
                assert [normalize(r(*q)) for q in QUERIES]==case['expected'],(case['mode'],db,'contents')
                for key,kind in TYPES.items():
                    assert r('TYPE',key)==kind.encode()
                    assert r('PEXPIRETIME',key)==EXPIRY
                r('BF.ADD','bf','new-item')
                r('CMS.INCRBY','cms','present',2)
                r('CF.DEL','cf','present')
                r('TOPK.INCRBY','topk','new-item',1000)
                r('TDIGEST.ADD','td',1000)
                assert r('BF.EXISTS','bf','new-item')==1
                assert r('CMS.QUERY','cms','present')==[14]
                assert r('CF.COUNT','cf','present')==1
                assert b'new-item' in r('TOPK.LIST','topk')
                assert float(r('TDIGEST.MAX','td'))==1000
            print(case['mode']+': all five legacy types, empty keys, two DBs, TTLs and continued writes passed')
        finally: s.close()

if __name__=='__main__':
    if '--generate' in sys.argv: generate()
    else: verify()
