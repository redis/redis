fast_float_strtod - C Implementation
====================================

This is a pure C implementation of fast string to double conversion,
originally based on the fast_float C++ library[1].

The implementation was converted to C to remove the C++ dependency from
Redis. Only the functionality needed by Redis is implemented:

- Parsing of decimal floating-point strings
- Support for leading plus sign (+)
- Support for inf/infinity and nan special values
- Scientific notation (e/E exponent)

The algorithm uses:
1. Fast path (Clinger's algorithm) for numbers that can be exactly
   represented: mantissa <= 2^53 and exponent in [-22, 22]
2. Fallback to standard strtod() for complex cases to ensure
   correctly-rounded results

Original fast_float library:
  https://github.com/fastfloat/fast_float
  by Daniel Lemire and João Paulo Magalhaes

License: MIT (see fast_float_strtod.c for full license text)

[1]: https://github.com/fastfloat/fast_float
