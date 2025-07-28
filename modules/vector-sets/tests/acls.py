from test import TestCase
class AclCategories(TestCase):
    VECTOR_SET_COMMANDS = {
        b"VADD",
        b"VCARD",
        b"VDIM",
        b"VEMB",
        b"VGETATTR",
        b"VINFO",
        b"VISMEMBER",
        b"VLINKS",
        b"VRANDMEMBER",
        b"VREM",
        b"VSETATTR",
        b"VSIM",
    }

    def getname(self):
        return "Vector set command ACL categories"

    def test(self):
        vector_set_commands = set(self.redis.execute_command("ACL CAT VECTORSET"))
        assert self.VECTOR_SET_COMMANDS == vector_set_commands, (
            f"Expected {AclCategories.VECTOR_SET_COMMANDS}, got {vector_set_commands}"
        )

        readonly_commands = set(self.redis.execute_command("ACL CAT READ"))
        assert self.VECTOR_SET_COMMANDS & readonly_commands == {
            b"VGETATTR",
            b"VSIM",
            b"VCARD",
            b"VISMEMBER",
            b"VDIM",
            b"VRANDMEMBER",
            b"VINFO",
            b"VLINKS",
            b"VEMB",
        }

        write_commands = set(self.redis.execute_command("ACL CAT WRITE"))
        assert self.VECTOR_SET_COMMANDS & write_commands == {
            b"VADD",
            b"VREM",
            b"VSETATTR",
        }

        fast_commands = set(self.redis.execute_command("ACL CAT FAST"))
        assert self.VECTOR_SET_COMMANDS & fast_commands == {
            b"VCARD",
            b"VDIM",
            b"VEMB",
            b"VGETATTR",
            b"VINFO",
            b"VLINKS",
            b"VSETATTR",
        }
