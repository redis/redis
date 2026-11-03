from common import *

class testConfigs():
  def __init__(self):
    self.env = Env(decodeResponses=True)

  def test_default_configs(self):
      """Test that the various `bloom` config parameters were added appropriately in module load"""
      env = self.env
      res = env.cmd('CONFIG', 'GET', 'bf-error-rate')
      env.assertEqual(res[1], '0.01')
      res = env.cmd('CONFIG', 'GET', 'bf-initial-size')
      env.assertEqual(res[1], '100')
      res = env.cmd('CONFIG', 'GET', 'bf-expansion-factor')
      env.assertEqual(res[1], '2')
  def test_config_set(self):
    """Test that the various `bloom` config parameters may be set"""
    env = self.env
    env.cmd('CONFIG', 'SET', 'bf-error-rate', '0.02')
    res = env.cmd('CONFIG', 'GET', 'bf-error-rate')
    env.assertEqual(res[1], '0.02')
    env.cmd('CONFIG', 'SET', 'bf-initial-size', '200')
    res = env.cmd('CONFIG', 'GET', 'bf-initial-size')
    env.assertEqual(res[1], '200')
    env.cmd('CONFIG', 'SET', 'bf-expansion-factor', '3')
    res = env.cmd('CONFIG', 'GET', 'bf-expansion-factor')
    env.assertEqual(res[1], '3')
  def test_config_set_invalid(self):
    """Test that the various `bloom` config parameters may not be set to invalid values"""
    env = self.env
    env.expect('CONFIG', 'SET', 'bf-error-rate', 0.0).error().contains('must be in the range')
    env.expect('CONFIG', 'SET', 'bf-error-rate', 1.0).error().contains('must be in the range')
    env.expect('CONFIG', 'SET', 'bf-error-rate', 0.01).ok()
    env.expect('CONFIG', 'SET', 'bf-initial-size', 0).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'bf-initial-size', 2**32).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'bf-initial-size', 100).ok()
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', -1).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', 32769).error().contains('argument must be between')
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', 2).ok()
  def test_config_bf_debug_stats(self):
    """Test that `bf.debug` and `cf.debug` config values reflect the current config values"""
    env = self.env

    # Default config values are 0.01, 100, 2
    env.cmd('bf.add', 'default_bf1', 'foo')
    default_bf1_debug_info = env.cmd('bf.debug', 'default_bf1')
    # Custom config values are 0.02, 200, 3
    env.cmd('bf.reserve', 'custom_bf1', 0.02, 200, 'expansion', 3)
    env.cmd('bf.add', 'custom_bf1', 'foo')
    custom_bf1_debug_info = env.cmd('bf.debug', 'custom_bf1')

    env.cmd('CONFIG', 'SET', 'bf-error-rate', 0.02, 'bf-initial-size', 200, 'bf-expansion-factor', 3)
    # Default config values are 0.02, 200, 3
    env.cmd('bf.add', 'default_bf2', 'foo')
    default_bf2_debug_info = env.cmd('bf.debug', 'default_bf2')
    # Custom config values are 0.01, 100, 2
    env.cmd('bf.reserve', 'custom_bf2', 0.01, 100, 'expansion', 2)
    env.cmd('bf.add', 'custom_bf2', 'foo')
    custom_bf2_debug_info = env.cmd('bf.debug', 'custom_bf2')

    env.assertEqual(default_bf1_debug_info, custom_bf2_debug_info)
    env.assertEqual(default_bf2_debug_info, custom_bf1_debug_info)
    env.assertNotEqual(default_bf1_debug_info, default_bf2_debug_info)
    env.assertNotEqual(custom_bf1_debug_info, custom_bf2_debug_info)


def test_config_server_startup():
  import tempfile
  from pathlib import Path
  with tempfile.TemporaryDirectory() as directory:
    config = Path(directory) / 'redis.conf'
    config.write_text('bf-error-rate 0.02\nbf-initial-size 200\nbf-expansion-factor 3\n')
    configured = Env(decodeResponses=True, redisConfigFile=str(config), freshEnv=True)
    try:
      for name, value in [('bf-error-rate', '0.02'), ('bf-initial-size', '200'), ('bf-expansion-factor', '3')]:
        configured.assertEqual(configured.cmd('CONFIG', 'GET', name)[1], value)
    finally:
      configured.stop()



def test_invalid_server_startup_config(env):
    # Native equivalents of the old INITIAL_SIZE / ERROR_RATE load-argument tests.
    import subprocess
    for args in (
        ['--bf-initial-size', '-1'], ['--bf-initial-size', 'BF'],
        ['--bf-error-rate', '-1'], ['--bf-error-rate', '2'],
        ['--bf-error-rate', 'BF'], ['--bf-initial-size'],
        ['--unknown-bloom-option', 'value'],
    ):
        result = subprocess.run([Defaults.binary, '--port', '0', '--save', '', *args],
                                capture_output=True, text=True, timeout=10)
        env.assertNotEqual(result.returncode, 0)
        output = result.stderr + result.stdout
        expected = ('Unresolved Configuration(s) Detected'
                    if args[0] == '--unknown-bloom-option' else 'FATAL CONFIG FILE ERROR')
        env.assertTrue(expected in output, message=output)
