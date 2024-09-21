README for fast_float v6.1.4

----------------------------------------------

We're using the fast_float library[1] in our (compiled-in)
floating-point fast_float_strtod implementation for faster and more
portable parsing of 64 decimal strings.

The single file fast_float.h is an amalgamation of the entire library,
which can be (re)generated with the amalgamate.py script (from the
fast_float repository) via the command

```
git clone https://github.com/fastfloat/fast_float
cd fast_float
git checkout v6.1.4
python3 ./script/amalgamate.py --license=MIT \
  > $REDIS_SRC/deps/fast_float/fast_float.h
```

[1]: https://github.com/fastfloat/fast_float
