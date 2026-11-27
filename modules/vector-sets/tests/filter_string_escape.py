from test import TestCase

class VSIMFilterStringEscapes(TestCase):
    def getname(self):
        return "VSIM FILTER string literals with escapes"

    def test(self):
        # Attribute strings that need an escape inside the expression:
        # a double quote, a single quote, a backslash and a newline.
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 2, 1, 0,
                                   f'{self.test_key}:item:1', 'SETATTR',
                                   '{"dq": "a\\"b", "sq": "O\'Brien", "bs": "x\\\\y", "nl": "l1\\nl2"}')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 2, 0, 1,
                                   f'{self.test_key}:item:2', 'SETATTR',
                                   '{"dq": "ab", "sq": "OBrien", "bs": "xy", "nl": "l1l2"}')

        def matching(expr):
            result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 2, 1, 0,
                                                'FILTER', expr)
            return [item.decode() for item in result]

        item1 = [f'{self.test_key}:item:1']
        assert matching('.dq == "a\\"b"') == item1, "Escaped double quote should match"
        assert matching(".sq == 'O\\'Brien'") == item1, "Escaped single quote should match"
        assert matching('.bs == "x\\\\y"') == item1, "Escaped backslash should match"
        assert matching('.nl == "l1\\nl2"') == item1, "Escaped newline should match"
        assert matching('"\\"b" in .dq') == item1, "Escaped double quote should match as substring"
        assert matching('.dq in ["zzz", "a\\"b"]') == item1, "Escaped double quote should match inside a tuple"

        # Quotes of the other kind need no escape and must keep working.
        assert matching("'a\"b' == .dq") == item1, "Double quote inside single quoted string"
        assert matching('.sq == "O\'Brien"') == item1, "Single quote inside double quoted string"
