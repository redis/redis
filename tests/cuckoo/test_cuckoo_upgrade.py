from support import *
from upgrade import upgrade

class testCuckooUpgrade:
    def test_existing_keys(self):
        def create(r):
            r('CF.RESERVE','subject',100)
            r('CF.ADD','subject','present')
            r('CF.ADD','subject','present')
            r('CF.RESERVE','empty',100)
        def snapshot(r):
            return [r('CF.COUNT','subject','present'),r('CF.INFO','empty')]
        def mutate(r):
            r('CF.DEL','subject','present')
            assert r('CF.COUNT','subject','present')==1
        upgrade(Env(),create,snapshot,mutate)
