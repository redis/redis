#!/usr/bin/env python

from itertools import combinations

travis_template = """\
language: generic

matrix:
  include:
%s

before_script:
  - autoconf
  - ./configure ${COMPILER_FLAGS:+ \
      CC="$CC $COMPILER_FLAGS" } \
      $CONFIGURE_FLAGS
  - make -j3
  - make -j3 tests

script:
  - make check
"""

# The 'default' configuration is gcc, on linux, with no compiler or configure
# flags.  We also test with clang, -m32, --enable-debug, --enable-prof,
# --disable-stats, and --disable-tcache.  To avoid abusing travis though, we
# don't test all 2**7 = 128 possible combinations of these; instead, we only
# test combinations of up to 2 'unusual' settings, under the hope that bugs
# involving interactions of such settings are rare.
# things at once, for C(7, 0) + C(7, 1) + C(7, 2) = 29
MAX_UNUSUAL_OPTIONS = 2

os_default = 'linux'
os_unusual = 'osx'

compilers_default = 'CC=gcc'
compilers_unusual = 'CC=clang'

compiler_flag_unusuals = ['-m32']

configure_flag_unusuals = [
    '--enable-debug', '--enable-prof', '--disable-stats', '--disable-tcache',
]

all_unusuals = (
    [os_unusual] + [compilers_unusual] + compiler_flag_unusuals
    + configure_flag_unusuals
)

unusual_combinations_to_test = []
for i in xrange(MAX_UNUSUAL_OPTIONS + 1):
    unusual_combinations_to_test += combinations(all_unusuals, i)

include_rows = ""
for unusual_combination in unusual_combinations_to_test:
    os = os_default
    if os_unusual in unusual_combination:
        os = os_unusual

    compilers = compilers_default
    if compilers_unusual in unusual_combination:
        compilers = compilers_unusual

    compiler_flags = [
        x for x in unusual_combination if x in compiler_flag_unusuals]

    configure_flags = [
        x for x in unusual_combination if x in configure_flag_unusuals]

    # Filter out an unsupported configuration - heap profiling on OS X.
    if os == 'osx' and '--enable-prof' in configure_flags:
        continue

    env_string = '{} COMPILER_FLAGS="{}" CONFIGURE_FLAGS="{}"'.format(
        compilers, " ".join(compiler_flags), " ".join(configure_flags))

    include_rows += '    - os: %s\n' % os
    include_rows += '      env: %s\n' % env_string
    if '-m32' in unusual_combination and os == 'linux':
        include_rows += '      addons:\n'
	include_rows += '        apt:\n'
	include_rows += '          packages:\n'
	include_rows += '            - gcc-multilib\n'

print travis_template % include_rows
