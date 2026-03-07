README for ffc.h v26.03.2

----------------------------------------------

We use ffc.h[1], a pure C99 port of the fast_float library[2], for our
compiled-in fast_float_strtod implementation. This provides fast and
portable parsing of decimal 64-bit floating-point strings without
requiring a C++ compiler.

The single file ffc.h is the amalgamated header from the ffc.h project.
To update, download the latest release:

```
curl -LO https://github.com/kolemannix/ffc.h/releases/download/v26.03.2/ffc.h
```

[1]: https://github.com/kolemannix/ffc.h
[2]: https://github.com/fastfloat/fast_float
