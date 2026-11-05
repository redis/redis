from common import *

class testACL():
  def __init__(self):
    self.env = Env(decodeResponses=True)
  
  def test_acl_category(self):
      """Test that the various `bloom` categories was added appropriately in module load"""
      env = self.env
      res = env.cmd('ACL', 'CAT')
      [env.assertTrue(cat in res) for cat in ['bloom']]

  def test_acl_json_commands(self):
      """Tests that the RedisBloom commands are registered to the various `bloom` ACL categories"""

      env = self.env
      if not server_version_at_least(self.env, '7.99.99'):
         env.skip()

      # Use a set since the order of the response is not consistent.
      BLOOM_COMMANDS = set([
        "bf.reserve", "bf.add", "bf.madd", "bf.insert", "bf.exists",  "bf.mexists", 
        "bf.info", "bf.card", "bf.debug",  "bf.scandump",  "bf.loadchunk",
      ])
      res = env.cmd('ACL', 'CAT', 'bloom')
      env.assertEqual(set(res), BLOOM_COMMANDS)

      # Check that one of our commands is listed in a non-bloom category
      res = env.cmd('ACL', 'CAT', 'read')
      env.assertTrue('bf.exists' in res)

  def test_acl_non_default_user(self):
      """Tests that a user with a non-default ACL can't access the `bloom` category"""

      env = self.env
      env.cmd('FLUSHALL')

      # Create a user with no command permissions (full keyspace and pubsub access)
      env.expect('ACL', 'SETUSER', 'testusr', 'on', '>123', '~*', '&*').true()
      env.expect('AUTH', 'testusr', '123').true()

      # Such a user shouldn't be able to run any `bloom` commands (or any other commands)
      env.expect('bf.exists a a').error().contains(
          "User testusr has no permissions to run the 'bf.exists' command")

      # Add `read` permissions to `testusr`
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr', '+@read').true()
      env.expect('AUTH', 'testusr', '123').true()

      READ_BF_COMMANDS = [
        "bf.exists", "bf.mexists", "bf.info", "bf.card", "bf.debug", "bf.scandump",
      ]

      # `testusr` should now be able to run `read` commands such as `bf.exists`
      for cmd in READ_BF_COMMANDS:
          env.expect(cmd).error().notContains("User testusr has no permissions")

      # `testusr` should not be able to run `bloom` commands that are not `read`
      env.expect('bf.reserve bf 0.01 1000').error().contains(
          "User testusr has no permissions to run the 'bf.reserve' command")

      # Add `write` permissions to `testusr`
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr', '+@write').true()
      env.expect('AUTH', 'testusr', '123').true()

      WRITE_BF_COMMANDS = [
        "bf.reserve", "bf.add", "bf.madd", "bf.insert", "bf.loadchunk",
      ]

      # `testusr` should now be able to run `write` commands
      for cmd in WRITE_BF_COMMANDS:
          env.expect(cmd).error().notContains("User testusr has no permissions")

      # Add `testusr2` and give it `bloom` permissions
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr2', 'on', '>123', '~*', '&*').true()
      env.expect('ACL', 'SETUSER', 'testusr2', '+@bloom').true()
      env.expect('AUTH', 'testusr2', '123').true()

      # `testusr2` should be able to run all `bloom` commands, both `read` and `write`
      env.expect('bf.add', 'test', 'foo').true()
      env.expect('bf.exists', 'test', 'foo').equal(1)
      env.expect('bf.exists', 'test', 'bar').equal(0)

      # `testusr2` should not be able to run unrelated string commands
      env.expect('set string value').error().contains(
          "User testusr2 has no permissions to run the 'set' command")
