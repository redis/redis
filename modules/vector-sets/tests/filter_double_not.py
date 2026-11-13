from test import TestCase

class VSIMFilterDoubleNot(TestCase):
    def getname(self):
        return "VSIM FILTER consecutive unary not operators"

    def test(self):
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 2, 1, 0,
                                   f'{self.test_key}:item:1', 'SETATTR',
                                   '{"active": true, "n": 5}')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 2, 0, 1,
                                   f'{self.test_key}:item:2', 'SETATTR',
                                   '{"active": false, "n": 0}')

        def matching(expr):
            result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 2, 1, 0,
                                                'FILTER', expr)
            return sorted(item.decode() for item in result)

        item1 = [f'{self.test_key}:item:1']
        item2 = [f'{self.test_key}:item:2']
        assert matching('!!.active') == item1, "!! should be the identity on booleans"
        assert matching('not not .active') == item1, "not not should be the identity on booleans"
        assert matching('!!!.active') == item2, "!!! should be a single negation"
        assert matching('!!.n == 1') == item1, "!! binds tighter than ==, like a single !"
        assert matching('!.active or !!.active') == item1 + item2, "!! after a binary operator"
        assert matching('.n > 1 and not not .active') == item1, "not not after and"
