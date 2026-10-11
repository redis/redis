"""Shared native Bloom/Cuckoo configuration tests from RedisBloom."""
from support import *

class testAllConfigs:
  def __init__(self):
    self.env = Env(decodeResponses=True)

  def test_default_configs(self):
      """Test that the various `bloom` config parameters were added appropriately in Redis core"""
      env = self.env
      res = env.cmd('CONFIG', 'GET', 'bf-error-rate')
      env.assertEqual(res[1], '0.01')
      res = env.cmd('CONFIG', 'GET', 'bf-initial-size')
      env.assertEqual(res[1], '100')
      res = env.cmd('CONFIG', 'GET', 'bf-expansion-factor')
      env.assertEqual(res[1], '2')
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
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 256).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-bucket-size', 2).ok()
    env.expect('CONFIG', 'SET', 'cf-initial-size', 0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-initial-size', 2**32).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-initial-size', 1024).ok()
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 65536).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-max-iterations', 20).ok()
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', -1).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', 32769).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-expansion-factor', 1).ok()
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 0).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 65537).error().contains('must be')
    env.expect('CONFIG', 'SET', 'cf-max-expansions', 32).ok()


  def test_cuckoo_startup_and_cross_validation(self):
    env=Env(decodeResponses=True,extra=('--cf-initial-size','32','--cf-bucket-size','4'))
    env.cmd('CF.ADD','cf','a')
    info=dict(zip(*[iter(env.cmd('CF.INFO','cf'))]*2))
    env.assertEqual(4,info['Bucket size'])
    env.expect('CONFIG','SET','cf-initial-size',2).error().contains('at least twice')
    env.assertEqual(['cf-initial-size','32'],env.cmd('CONFIG','GET','cf-initial-size'))
    env.cmd('CONFIG','SET','cf-initial-size',2,'cf-bucket-size',1)
    env.cmd('CF.ADD','small','a')
