#!/usr/bin/env python3

import sys
from itertools import combinations
from os import uname
from multiprocessing import cpu_count
from subprocess import call

# Later, we want to test extended vaddr support.  Apparently, the "real" way of
# checking this is flaky on OS X.
bits_64 = sys.maxsize > 2**32

nparallel = cpu_count() * 2

uname = uname()[0]

if call("command -v gmake", shell=True) == 0:
    make_cmd = 'gmake'
else:
    make_cmd = 'make'

def powerset(items):
    result = []
    for i in range(len(items) + 1):
        result += combinations(items, i)
    return result

possible_compilers = []
for cc, cxx in (['gcc', 'g++'], ['clang', 'clang++']):
    try:
        cmd_ret = call([cc, "-v"])
        if cmd_ret == 0:
            possible_compilers.append((cc, cxx))
    except:
        pass
possible_compiler_opts = [
    '-m32',
]
possible_config_opts = [
    '--enable-debug',
    '--enable-prof',
    '--disable-stats',
    '--enable-opt-safety-checks',
    '--with-lg-page=16',
]
if bits_64:
    possible_config_opts.append('--with-lg-vaddr=56')

possible_malloc_conf_opts = [
    'tcache:false',
    'dss:primary',
    'percpu_arena:percpu',
    'background_thread:true',
]

print('set -e')
print('if [ -f Makefile ] ; then %(make_cmd)s relclean ; fi' % {'make_cmd':
    make_cmd})
print('autoconf')
print('rm -rf run_tests.out')
print('mkdir run_tests.out')
print('cd run_tests.out')

ind = 0
for cc, cxx in possible_compilers:
    for compiler_opts in powerset(possible_compiler_opts):
        for config_opts in powerset(possible_config_opts):
            for malloc_conf_opts in powerset(possible_malloc_conf_opts):
                if cc == 'clang' \
                  and '-m32' in possible_compiler_opts \
                  and '--enable-prof' in config_opts:
                    continue
                config_line = (
                    'EXTRA_CFLAGS=-Werror EXTRA_CXXFLAGS=-Werror '
                    + 'CC="{} {}" '.format(cc, " ".join(compiler_opts))
                    + 'CXX="{} {}" '.format(cxx, " ".join(compiler_opts))
                    + '../../configure '
                    + " ".join(config_opts) + (' --with-malloc-conf=' +
                    ",".join(malloc_conf_opts) if len(malloc_conf_opts) > 0
                    else '')
                )

                # We don't want to test large vaddr spaces in 32-bit mode.
                if ('-m32' in compiler_opts and '--with-lg-vaddr=56' in
                    config_opts):
                    continue

                # Per CPU arenas are only supported on Linux.
                linux_supported = ('percpu_arena:percpu' in malloc_conf_opts \
                  or 'background_thread:true' in malloc_conf_opts)
                # Heap profiling and dss are not supported on OS X.
                darwin_unsupported = ('--enable-prof' in config_opts or \
                  'dss:primary' in malloc_conf_opts)
                if (uname == 'Linux' and linux_supported) \
                  or (not linux_supported and (uname != 'Darwin' or \
                  not darwin_unsupported)):
                    print("""cat <<EOF > run_test_%(ind)d.sh
#!/bin/sh

set -e

abort() {
    echo "==> Error" >> run_test.log
    echo "Error; see run_tests.out/run_test_%(ind)d.out/run_test.log"
    exit 255 # Special exit code tells xargs to terminate.
}

# Environment variables are not supported.
run_cmd() {
    echo "==> \$@" >> run_test.log
    \$@ >> run_test.log 2>&1 || abort
}

echo "=> run_test_%(ind)d: %(config_line)s"
mkdir run_test_%(ind)d.out
cd run_test_%(ind)d.out

echo "==> %(config_line)s" >> run_test.log
%(config_line)s >> run_test.log 2>&1 || abort

run_cmd %(make_cmd)s all tests
run_cmd %(make_cmd)s check
run_cmd %(make_cmd)s distclean
EOF
chmod 755 run_test_%(ind)d.sh""" % {'ind': ind, 'config_line': config_line,
      'make_cmd': make_cmd})
                    ind += 1

print('for i in `seq 0 %(last_ind)d` ; do echo run_test_${i}.sh ; done | xargs'
    ' -P %(nparallel)d -n 1 sh' % {'last_ind': ind-1, 'nparallel': nparallel})
