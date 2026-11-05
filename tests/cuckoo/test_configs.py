from common import *

class testConfigs():
  def __init__(self):
    self.env = Env(decodeResponses=True)

  def test_default_configs(self):
      env = self.env
      res = env.cmd('CONFIG', 'GET', 'cf-bucket-size')
      env.assertEqual(res[1], '2')
      res = env.cmd('CONFIG', 'GET', 'cf-initial-size')
      env.assertEqual(res[1], '1024')
      res = env.cmd('CONFIG', 'GET', 'cf-max-iterations')
      env.assertEqual(res[1], '20')
      res = env.cmd('CONFIG', 'GET', 'cf-expansion-factor')
      env.assertEqual(res[1], '1')
      res = env.cmd('CONFIG', 'GET', 'cf-max-expansions')
      env.assertEqual(res[1], '32')


  def test_config_set(self):
    env = self.env
    env.cmd('CONFIG', 'SET', 'cf-bucket-size', '3')
    res = env.cmd('CONFIG', 'GET', 'cf-bucket-size')
    env.assertEqual(res[1], '3')
    env.cmd('CONFIG', 'SET', 'cf-initial-size', '2048')
    res = env.cmd('CONFIG', 'GET', 'cf-initial-size')
    env.assertEqual(res[1], '2048')
    env.cmd('CONFIG', 'SET', 'cf-max-iterations', '30')
    res = env.cmd('CONFIG', 'GET', 'cf-max-iterations')
    env.assertEqual(res[1], '30')
    env.cmd('CONFIG', 'SET', 'cf-expansion-factor', '2')
    res = env.cmd('CONFIG', 'GET', 'cf-expansion-factor')
    env.assertEqual(res[1], '2')
    env.cmd('CONFIG', 'SET', 'cf-max-expansions', '64')
    res = env.cmd('CONFIG', 'GET', 'cf-max-expansions')
    env.assertEqual(res[1], '64')


  def test_config_set_invalid(self):
    env = self.env
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 0).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 256).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 2).ok()
    env.expect('CONFIG', 'SET', 'cf-initial-size', 0).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-initial-size', 2**32).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-initial-size', 1024).ok()
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 0).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 65536).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 20).ok()
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', -1).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', 32769).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', 1).ok()
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 0).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 65537).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 32).ok()


def test_config_server_startup():
    import tempfile
    from pathlib import Path
    values = [('cf-bucket-size', '3'), ('cf-initial-size', '2048'),
              ('cf-max-iterations', '30'), ('cf-expansion-factor', '2'),
              ('cf-max-expansions', '3')]
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / 'redis.conf'
        config.write_text(''.join(f'{name} {value}\n' for name, value in values))
        env = Env(decodeResponses=True, redisConfigFile=str(config), freshEnv=True)
        try:
            for name, value in values:
                env.assertEqual(env.cmd('CONFIG', 'GET', name)[1], value)
            env.cmd('CF.ADD', 'cf', 'item')
            info = env.cmd('CF.INFO', 'cf')
            info = dict(zip(info[::2], info[1::2]))
            env.assertEqual(info['Bucket size'], 3)
            env.assertEqual(info['Max iterations'], 30)
            env.assertEqual(info['Expansion rate'], 2)
        finally:
            env.stop()
