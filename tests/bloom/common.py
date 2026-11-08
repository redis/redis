import os
from functools import partial
from RLTest import Env as _Env, Defaults
from redis import ResponseError
from packaging import version

VALGRIND = os.getenv('VALGRIND', '0') == '1'
Env = partial(_Env, terminateRetries=300 if VALGRIND else 20, terminateRetrySecs=1)
Defaults.env_factory = Env


def server_version_at_least(env, ver):
    return version.parse(env.cmd('INFO')['redis_version']) >= version.parse(ver)


def server_version_less_than(env, ver):
    return not server_version_at_least(env, ver)
