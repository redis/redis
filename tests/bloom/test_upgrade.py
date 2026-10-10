from support import *
from upgrade import upgrade

class testBloomUpgrade:
    def test_existing_keys(self):
        def create(r):
            r('BF.RESERVE','subject',.01,100)
            r('BF.ADD','subject','present')
            r('BF.RESERVE','empty',.01,100)
        def snapshot(r):
            return [r('BF.MEXISTS','subject','present','absent'),r('BF.CARD','empty')]
        def mutate(r):
            r('BF.ADD','subject','new-item')
            assert r('BF.EXISTS','subject','new-item')==1
        upgrade(Env(),create,snapshot,mutate)
