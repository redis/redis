from common import *

def is_redis_version_smaller_than(con, _version, is_cluster=False):
    res = con.execute_command('INFO')
    ver = ""
    if is_cluster:
        try:
            ver = ((list(res.values()))[0])['redis_version']
        except:
            ver = res['redis_version']
        #print(((list(res.values()))[0]))
    else:
        ver = res['redis_version']
    return version.parse(ver) < version.parse(_version)


def redis_version(con, is_cluster=False):
    res = con.execute_command('INFO')
    ver = ""
    if is_cluster:
        try:
            ver = list(res.values())[0]['redis_version']
        except:
            ver = res['redis_version']
    else:
        ver = res['redis_version']
    return version.parse(ver)


class testResp3():
    def __init__(self):
        self.env = Env(protocol=3)

    def test_bf_resp3(self):
        env = self.env
        r_ver = redis_version(env)
        if r_ver < version.parse("7.0.0"):
            env.skip()
        # if r_ver >= version.parse("7.0.0"):
        #     env.skipOnCluster() # TODO: remove when redis-py is fixed
        with env.getClusterConnectionIfNeeded() as r:
            r_ver = redis_version(r)
            if r_ver < version.parse("7.0.0"):
                env.skip()

        env.cmd('FLUSHALL')
        res = env.cmd('bf.add', 'test', 'item')
        assert type(res) == bool
        assert res == True

        res = env.cmd('bf.card', 'test')
        assert res == 1

        res = env.cmd('bf.EXISTS', 'test', 'item')
        assert type(res) == bool
        assert res == True

        res = env.cmd('bf.info', 'test')
        assert res == {b'Capacity': 100, b'Size': 240, b'Number of filters': 1,
            b'Number of items inserted': 1, b'Expansion rate': 2}

        res = env.cmd('bf.insert', 'test', 'ITEMS', 'item2', 'item3', 'item2')
        assert type(res[0]) == bool
        assert res == [True, True, False]

        res = env.cmd('bf.madd', 'test', 'item4', 'item5', 'item2')
        assert type(res[0]) == bool
        assert res == [True, True, False]

        res = env.cmd('bf.MEXISTS', 'test', 'item4', 'item5', 'item6')
        assert type(res[0]) == bool
        assert res == [True, True, False]

