"""Verify registration of every migrated command, its docs and ACL category."""
import json
from pathlib import Path
from support import *

class testFamilyMetadata:
    def test_all_commands(self):
        env=Env(decodeResponses=True)
        root=Path(__file__).resolve().parents[2]/'src'/'commands'
        for prefix,category,count in [('bf','bloom',11),('cf','cuckoo',14),
                                      ('cms','cms',6),('topk','topk',7),('tdigest','tdigest',14)]:
            files=sorted(root.glob(prefix+'.*.json'))
            env.assertEqual(count,len(files))
            names=[]
            for path in files:
                name,spec=next(iter(json.loads(path.read_text()).items()))
                names.append(name.lower())
                reply=env.cmd('COMMAND','DOCS',name)[1]
                docs=dict(zip(reply[::2],reply[1::2]))
                env.assertEqual(spec['summary'],docs['summary'])
                env.assertEqual(spec['complexity'],docs['complexity'])
                env.assertEqual(spec['since'],docs['since'])
                env.assertEqual(category,docs['group'])
                env.assertNotIn('module',docs)
                args=[dict(zip(a[::2],a[1::2])) for a in docs.get('arguments',[])]
                env.assertEqual([a['name'].lower() for a in spec.get('arguments',[])],[a['name'] for a in args])
                info=env.cmd('COMMAND','INFO',name)[0]
                env.assertEqual(spec['arity'],info[1])
                env.assertNotIn('module',info[2])
            env.assertEqual(sorted(names),sorted(env.cmd('ACL','CAT',category)))
