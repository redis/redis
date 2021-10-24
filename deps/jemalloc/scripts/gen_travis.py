#!/usr/bin/env python

from itertools import combinations

travis_template = """\
language: generic
dist: precise

matrix:
  include:
%s

before_script:
  - autoconf
  - scripts/gen_travis.py > travis_script && diff .travis.yml travis_script
  - ./configure ${COMPILER_FLAGS:+ \
      CC="$CC $COMPILER_FLAGS" \
      CXX="$CXX $COMPILER_FLAGS" } \
      $CONFIGURE_FLAGS
  - make -j3
  - make -j3 tests

script:
  - make check
"""

# The 'default' configuration is gcc, on linux, with no compiler or configure
# flags.  We also test with clang, -m32, --enable-debug, --enable-prof,
# --disable-stats, and --with-malloc-conf=tcache:false.  To avoid abusing
# travis though, we don't test all 2**7 = 128 possible combinations of these;
# instead, we only test combinations of up to 2 'unusual' settings, under the
# hope that bugs involving interactions of such settings are rare.
# Things at once, for C(7, 0) + C(7, 1) + C(7, 2) = 29
MAX_UNUSUAL_OPTIONS = 2

os_default = 'linux'
os_unusual = 'osx'

compilers_default = 'CC=gcc CXX=g++'
compilers_unusual = 'CC=clang CXX=clang++'

compiler_flag_unusuals = ['-m32']

configure_flag_unusuals = [
    '--enable-debug',
    '--enable-prof',
    '--disable-stats',
    '--disable-libdl',
    '--enable-opt-safety-checks',
]

malloc_conf_unusuals = [
    'tcache:false',
    'dss:primary',
    'percpu_arena:percpu',
    'background_thread:true',
]

all_unusuals = (
    [os_unusual] + [compilers_unusual] + compiler_flag_unusuals
    + configure_flag_unusuals + malloc_conf_unusuals
)

unusual_combinations_to_test = []
for i in xrange(MAX_UNUSUAL_OPTIONS + 1):
    unusual_combinations_to_test += combinations(all_unusuals, i)

gcc_multilib_set = False
# Formats a job from a combination of flags
def format_job(combination):
    global gcc_multilib_set

    os = os_unusual if os_unusual in combination else os_default
    compilers = compilers_unusual if compilers_unusual in combination else compilers_default

    compiler_flags = [x for x in combination if x in compiler_flag_unusuals]
    configure_flags = [x for x in combination if x in configure_flag_unusuals]
    malloc_conf = [x for x in combination if x in malloc_conf_unusuals]

    # Filter out unsupported configurations on OS X.
    if os == 'osx' and ('dss:primary' in malloc_conf or \
      'percpu_arena:percpu' in malloc_conf or 'background_thread:true' \
      in malloc_conf):
        return ""
    if len(malloc_conf) > 0:
        configure_flags.append('--with-malloc-conf=' + ",".join(malloc_conf))

    # Filter out an unsupported configuration - heap profiling on OS X.
    if os == 'osx' and '--enable-prof' in configure_flags:
        return ""

    # We get some spurious errors when -Warray-bounds is enabled.
    env_string = ('{} COMPILER_FLAGS="{}" CONFIGURE_FLAGS="{}" '
	'EXTRA_CFLAGS="-Werror -Wno-array-bounds"').format(
        compilers, " ".join(compiler_flags), " ".join(configure_flags))

    job = ""
    job += '    - os: %s\n' % os
    job += '      env: %s\n' % env_string
    if '-m32' in combination and os == 'linux':
        job += '      addons:'
        if gcc_multilib_set:
            job += ' *gcc_multilib\n'
        else:
            job += ' &gcc_multilib\n'
            job += '        apt:\n'
            job += '          packages:\n'
            job += '            - gcc-multilib\n'
            gcc_multilib_set = True
    return job

include_rows = ""
for combination in unusual_combinations_to_test:
    include_rows += format_job(combination)

# Development build
include_rows += '''\
    # Development build
    - os: linux
      env: CC=gcc CXX=g++ COMPILER_FLAGS="" CONFIGURE_FLAGS="--enable-debug --disable-cache-oblivious --enable-stats --enable-log --enable-prof" EXTRA_CFLAGS="-Werror -Wno-array-bounds"
'''

# Enable-expermental-smallocx
include_rows += '''\
    # --enable-expermental-smallocx:
    - os: linux
      env: CC=gcc CXX=g++ COMPILER_FLAGS="" CONFIGURE_FLAGS="--enable-debug --enable-experimental-smallocx --enable-stats --enable-prof" EXTRA_CFLAGS="-Werror -Wno-array-bounds"
'''

# Valgrind build bots
include_rows += '''
    # Valgrind
    - os: linux
      env: CC=gcc CXX=g++ COMPILER_FLAGS="" CONFIGURE_FLAGS="" EXTRA_CFLAGS="-Werror -Wno-array-bounds" JEMALLOC_TEST_PREFIX="valgrind"
      addons:
        apt:
          packages:
            - valgrind
'''

# To enable valgrind on macosx add:
#
#  - os: osx
#    env: CC=gcc CXX=g++ COMPILER_FLAGS="" CONFIGURE_FLAGS="" EXTRA_CFLAGS="-Werror -Wno-array-bounds" JEMALLOC_TEST_PREFIX="valgrind"
#    install: brew install valgrind
#
# It currently fails due to: https://github.com/jemalloc/jemalloc/issues/1274

print travis_template % include_rows
