# Check that slaves are reconfigured at a latter time if they are partitioned.
#
# Here we should test:
# 1) That slaves point to the new master after failover.
# 2) That partitioned slaves point to new master when they are partitioned
#    away during failover and return at a latter time.
