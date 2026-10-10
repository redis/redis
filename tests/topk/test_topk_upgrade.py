from support import *
from upgrade import upgrade

class testTopKUpgrade:
    def test_existing_keys(self):
        def create(r):
            r('TOPK.RESERVE','subject',3,8,7,.9)
            r('TOPK.RESERVE','empty',3)
            r('TOPK.INCRBY','empty','',0)
            for i in range(100): r('TOPK.INCRBY','subject',i%11,3)
        def snapshot(r):
            return [r('TOPK.LIST','subject','WITHCOUNT'),r('TOPK.COUNT','subject',*range(11)),
                    r('TOPK.INFO','subject'),r('TOPK.LIST','empty'),r('TOPK.QUERY','empty','')]
        def mutate(r):
            r('TOPK.INCRBY','subject','new-item',1000)
            assert b'new-item' in r('TOPK.LIST','subject')
        upgrade(Env(),create,snapshot,mutate)
