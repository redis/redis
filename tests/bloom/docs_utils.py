from common import *


def _kv_list_to_dict(kv_list):
    if isinstance(kv_list, dict):
        return kv_list
    if not isinstance(kv_list, (list, tuple)):
        return {}
    res = {}
    for i in range(0, len(kv_list) - 1, 2):
        res[kv_list[i]] = kv_list[i + 1]
    return res


def _extract_docs_for_command(res, cmd_name):
    # Accept multiple response layouts across redis versions / clients
    names = {cmd_name, cmd_name.upper()}
    # Dict layout: { 'cf.reserve': { ... } }
    if isinstance(res, dict):
        for name in names | {cmd_name.lower()} | {cmd_name.title()}:
            if name in res:
                return _kv_list_to_dict(res[name])
        # Some clients may key by bytes
        for name in list(names):
            bname = name.encode()
            if bname in res:
                return _kv_list_to_dict(res[bname])
        # Fallback: value may itself be the docs map
        return _kv_list_to_dict(res)

    # List layout options:
    # 1) [[name, [k, v, k, v, ...]], ...]
    # 2) [name, [k, v, k, v, ...]] for a single-command response
    if isinstance(res, (list, tuple)):
        # Single-command flat layout
        if len(res) == 2 and isinstance(res[0], (bytes, str)):
            name_cmp = res[0].decode() if isinstance(res[0], bytes) else res[0]
            if name_cmp in {cmd_name, cmd_name.upper(), cmd_name.lower(), cmd_name.title()}:
                return _kv_list_to_dict(res[1])
        for entry in res:
            if isinstance(entry, (list, tuple)) and len(entry) >= 2:
                name = entry[0]
                if isinstance(name, bytes):
                    name_cmp = name.decode()
                else:
                    name_cmp = name
                if name_cmp in names:
                    return _kv_list_to_dict(entry[1])
    return {}

def assert_docs(env, cmd, *, summary, complexity, arity=None, since=None, group='bloom',
                args=None, key_pos=1, key_spec_index=0):
    docs = _extract_docs_for_command(env.cmd(f'COMMAND DOCS {cmd}'), cmd)

    env.assertTrue(bool(docs), message=f'Missing COMMAND DOCS for {cmd}')
    for field in ('summary', 'complexity', 'since', 'group', 'arguments'):
        env.assertTrue(field in docs, message=f'{cmd}: missing {field}')

    # Core fields
    if 'summary' in docs:
        env.assertEqual(docs['summary'], summary)
    if 'complexity' in docs:
        env.assertEqual(docs['complexity'], complexity)
    if arity is not None and 'arity' in docs:
        env.assertEqual(docs['arity'], arity)
    if since is not None and 'since' in docs:
        env.assertEqual(docs['since'], since)
    if group is not None and 'group' in docs:
        env.assertEqual(docs['group'], group)
    if 'module' in docs:
        assert isinstance(docs['module'], (str, bytes))
    if 'history' in docs:
        assert isinstance(docs['history'], (list, tuple))

    # Arguments
    if args is not None:
        norm_args = [_kv_list_to_dict(a) for a in (docs.get('arguments') or docs.get('args') or [])]
        env.assertEqual([a.get('name') for a in norm_args], [n[0] for n in args])
        # Key arg should reference first key spec (when provided)
        if norm_args:
            ks_idx = norm_args[0].get('key_spec_index')
            if ks_idx is not None:
                try:
                    ks_idx = int(ks_idx)
                except Exception:
                    pass
                env.assertEqual(ks_idx, key_spec_index)
        # Ensure first arg is key type when available
        if norm_args:
            first_type = norm_args[0].get('type')
            if isinstance(first_type, bytes):
                first_type = first_type.decode()
            if first_type is not None:
                assert str(first_type).lower() == 'key'

    # Key specs (optional)
    ks_val = (docs.get('key specifications') or docs.get('key-specifications') or
              docs.get('key_specs') or docs.get('key-specs'))
    if ks_val is not None and isinstance(ks_val, (list, tuple)) and ks_val:
        first_ks = _kv_list_to_dict(ks_val[0])
        bs = first_ks.get('begin_search') or first_ks.get('begin-search')
        if bs is not None:
            bs_map = _kv_list_to_dict(bs)
            idx = bs_map.get('index') or bs_map.get('by')
            if idx is not None:
                idx_map = _kv_list_to_dict(idx)
                pos = idx_map.get('pos') or idx_map.get('position')
                if isinstance(pos, (bytes, str)) and not isinstance(pos, int):
                    try:
                        pos = int(pos.decode() if isinstance(pos, bytes) else pos)
                    except Exception:
                        pass
                if isinstance(pos, int):
                    env.assertEqual(pos, key_pos)
