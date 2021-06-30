Hash table implementation related utilities.

rehashing.c
---

Visually show buckets in the two hash tables between rehashings. Also stress
test getRandomKeys() implementation, that may actually disappear from
Redis soon, However the visualization code is reusable in new bugs
investigation.

Compile with:

    cc -I ../../src/ rehashing.c ../../src/zmalloc.c ../../src/dict.c -o rehashing_test
