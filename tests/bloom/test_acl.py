"""Bloom cases from RedisBloom tests/flow/test_acl.py."""
from support import *

READ = {"bf.exists", "bf.mexists", "bf.info", "bf.card", "bf.debug", "bf.scandump"}
WRITE = {"bf.reserve", "bf.add", "bf.madd", "bf.insert", "bf.loadchunk"}


class testACL:
    def __init__(self):
        self.env = Env(decodeResponses=True)

    def test_acl_category(self):
        self.env.assertIn("bloom", self.env.cmd("ACL", "CAT"))

    def test_acl_json_commands(self):
        env = self.env
        env.assertEqual(READ | WRITE, set(env.cmd("ACL", "CAT", "bloom")))
        env.assertTrue(READ <= set(env.cmd("ACL", "CAT", "read")))
        env.assertTrue(WRITE <= set(env.cmd("ACL", "CAT", "write")))

    def test_acl_non_default_user(self):
        env = self.env
        env.expect("ACL", "SETUSER", "testusr", "on", ">123", "~*", "&*").ok()
        env.expect("AUTH", "testusr", "123").ok()
        env.expect("bf.exists a a").error().contains("NOPERM")
        env.cmd("AUTH", "default", "")
        env.cmd("ACL", "SETUSER", "testusr", "+@read")
        env.cmd("AUTH", "testusr", "123")
        for command in READ:
            env.expect(command).error().notContains("NOPERM")
        env.expect("bf.reserve bf 0.01 1000").error().contains("NOPERM")
        env.cmd("AUTH", "default", "")
        env.cmd("ACL", "SETUSER", "testusr", "+@write")
        env.cmd("AUTH", "testusr", "123")
        for command in WRITE:
            env.expect(command).error().notContains("NOPERM")
        env.cmd("AUTH", "default", "")
        env.cmd("ACL", "SETUSER", "testusr2", "on", ">123", "~*", "+@bloom")
        env.cmd("AUTH", "testusr2", "123")
        env.expect("bf.add test foo").equal(1)
        env.expect("bf.exists test foo").equal(1)
        env.expect("bf.exists test bar").equal(0)
        # No dependency on Cuckoo being loaded: use a non-Bloom core command.
        env.expect("GET test").error().contains("NOPERM")
        env.cmd("AUTH", "default", "")

    def test_acl_key_patterns(self):
        env = self.env
        env.cmd("ACL", "SETUSER", "limited", "on", ">123", "~allowed:*", "+@bloom")
        env.cmd("AUTH", "limited", "123")
        env.expect("BF.INSERT allowed:bf ITEMS foo").equal([1])
        env.expect("BF.MADD denied:bf foo").error().contains("NOPERM")
        env.expect("BF.MEXISTS denied:bf foo").error().contains("NOPERM")
        env.cmd("AUTH", "default", "")
