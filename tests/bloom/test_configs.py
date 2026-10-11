"""Bloom configuration cases adapted from RedisBloom v8.11.81."""
from support import *

class testConfigs():
  def __init__(self):
    self.env = Env(decodeResponses=True)

  def test_default_configs(self):
      """Test that the various `bloom` config parameters are registered in core"""
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
    env.expect('CONFIG', 'SET', 'bf-error-rate', 0.0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-error-rate', 1.0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-error-rate', 0.01).ok()
    env.expect('CONFIG', 'SET', 'bf-initial-size', 0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-initial-size', 2**32).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-initial-size', 100).ok()
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', -1).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', 32769).error().contains('must be')
    env.expect('CONFIG', 'SET', 'bf-expansion-factor', 2).ok()

  def test_config_server_startup(self):
    # Native equivalent of module-load configuration and deprecated init_test.py.
    env = self.env
    configured = Env(decodeResponses=True, extra=(
        '--bf-error-rate', '0.02', '--bf-initial-size', '200', '--bf-expansion-factor', '3'))
    env.cmd('CONFIG', 'SET', 'bf-error-rate', '0.02', 'bf-initial-size', '200', 'bf-expansion-factor', '3')
    for setting in ('bf-error-rate', 'bf-initial-size', 'bf-expansion-factor'):
        env.assertEqual(env.cmd('CONFIG', 'GET', setting), configured.cmd('CONFIG', 'GET', setting))
    env.cmd('BF.ADD', 'bf', 'foo')
    configured.cmd('BF.ADD', 'bf', 'foo')
    env.assertEqual(env.cmd('BF.DEBUG', 'bf'), configured.cmd('BF.DEBUG', 'bf'))

  def test_config_invalid_server_startup(self):
    for option, value in (('bf-initial-size', '-1'), ('bf-initial-size', 'BF'),
                          ('bf-error-rate', '-1'), ('bf-error-rate', '2'),
                          ('bf-error-rate', 'BF'), ('bf-expansion-factor', '-1')):
        with self.env.assertRaises(AssertionError):
            Env(extra=('--' + option, value))

  def test_config_bf_debug_stats(self):
    """Test that `bf.debug` config values reflect the current config values"""
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
