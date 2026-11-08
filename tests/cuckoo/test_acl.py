from common import *

class testACL():
  def __init__(self):
    self.env = Env(decodeResponses=True)

  def test_acl_category(self):
      """Test that the various `cuckoo` category is registered in core"""
      env = self.env
      res = env.cmd('ACL', 'CAT')
      [env.assertTrue(cat in res) for cat in ['cuckoo']]

  def test_acl_json_commands(self):
      """Tests that the Cuckoo commands are registered to the various `cuckoo` ACL categories"""

      env = self.env
      if not server_version_at_least(self.env, '7.99.99'):
         env.skip()

      # Use a set since the order of the response is not consistent.
      CUCKOO_COMMANDS = set([
        "cf.reserve", "cf.add", "cf.addnx", "cf.insert", "cf.insertnx",
        "cf.exists", "cf.mexists", "cf.count", "cf.del", "cf.compact",
        "cf.info", "cf.debug", "cf.scandump", "cf.loadchunk",
      ])
      res = env.cmd('ACL', 'CAT', 'cuckoo')
      env.assertEqual(set(res), CUCKOO_COMMANDS)

      # Check that one of our commands is listed in a non-cuckoo category
      res = env.cmd('ACL', 'CAT', 'read')
      env.assertTrue('cf.exists' in res)

  def test_acl_non_default_user(self):
      """Tests that a user with a non-default ACL can't access the `cuckoo` category"""

      env = self.env
      env.cmd('FLUSHALL')

      # Create a user with no command permissions (full keyspace and pubsub access)
      env.expect('ACL', 'SETUSER', 'testusr', 'on', '>123', '~*', '&*').true()
      env.expect('AUTH', 'testusr', '123').true()

      # Such a user shouldn't be able to run any `cuckoo` commands (or any other commands)
      env.expect('cf.exists a a').error().contains(
          "User testusr has no permissions to run the 'cf.exists' command")

      # Add `read` permissions to `testusr`
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr', '+@read').true()
      env.expect('AUTH', 'testusr', '123').true()

      READ_CF_COMMANDS = [
        "cf.exists", "cf.mexists", "cf.info", "cf.count", "cf.debug", "cf.scandump",
      ]

      # `testusr` should now be able to run `read` commands such as `cf.exists`
      env.expect('AUTH', 'default', '').true()
      for cmd in READ_CF_COMMANDS:
          args = ['missing', '0'] if cmd.endswith('scandump') else ['missing']
          if cmd.endswith(('exists', 'mexists', 'count')):
              args = ['missing', 'item']
          env.assertEqual(env.cmd('ACL', 'DRYRUN', 'testusr', cmd, *args), 'OK')

      env.expect('AUTH', 'testusr', '123').true()

      # `testusr` should not be able to run `cuckoo` commands that are not `read`
      env.expect('cf.reserve cf 1000').error().contains(
          "User testusr has no permissions to run the 'cf.reserve' command")

      env.expect('cf.compact cf').error().contains(
          "User testusr has no permissions to run the 'cf.compact' command")

      # Add `write` permissions to `testusr`
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr', '+@write').true()
      env.expect('AUTH', 'testusr', '123').true()

      WRITE_CF_COMMANDS = [
        ['cf.reserve', 'key', '1000'], ['cf.add', 'key', 'item'],
        ['cf.addnx', 'key', 'item'], ['cf.insert', 'key', 'ITEMS', 'item'],
        ['cf.insertnx', 'key', 'ITEMS', 'item'], ['cf.del', 'key', 'item'],
        ['cf.compact', 'key'], ['cf.loadchunk', 'key', '1', 'header'],
      ]

      # Use valid arity: core checks arity before checking command permissions.
      env.expect('AUTH', 'default', '').true()
      for command in WRITE_CF_COMMANDS:
          env.assertEqual(env.cmd('ACL', 'DRYRUN', 'testusr', *command), 'OK')

      # Add `testusr2` and give it `cuckoo` permissions
      env.expect('AUTH', 'default', '').true()
      env.expect('ACL', 'SETUSER', 'testusr2', 'on', '>123', '~*', '&*').true()
      env.expect('ACL', 'SETUSER', 'testusr2', '+@cuckoo').true()
      env.expect('AUTH', 'testusr2', '123').true()

      # `testusr2` should be able to run all `cuckoo` commands, both `read` and `write`
      env.expect('cf.add', 'test', 'foo').true()
      env.expect('cf.exists', 'test', 'foo').equal(1)
      env.expect('cf.exists', 'test', 'bar').equal(0)

      # `testusr2` should not be able to run unrelated string commands
      env.expect('set string value').error().contains(
          "User testusr2 has no permissions to run the 'set' command")
