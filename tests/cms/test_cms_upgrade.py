from support import *
from upgrade import upgrade

class testCMSUpgrade:
    def test_existing_keys(self):
        def create(r):
            r('CMS.INITBYDIM','subject',100,5)
            r('CMS.INCRBY','subject','present',12)
            r('CMS.INITBYDIM','empty',100,5)
        def snapshot(r):
            return [r('CMS.QUERY','subject','present'),r('CMS.INFO','empty')]
        def mutate(r):
            r('CMS.INCRBY','subject','present',2)
            assert r('CMS.QUERY','subject','present')==[14]
        upgrade(Env(),create,snapshot,mutate)
