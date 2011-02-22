# Redis - catwell's tree

Differences with antirez/master:

- redis-cli remembers SELECT calls
- REDIS_SHARED_INTEGERS is 1M
- new commands:
  - COUNT (merge from Elbandi/master)
  - DECRTO
  - HMDEL
  - INCRTO
  - MHDEL
  - MHGET
  - MHLEN
  - MHSET
  - MSADD (merge from seppo0010/msadd)
  - MSREM
