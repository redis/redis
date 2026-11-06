#!/bin/bash

set -e

# The purpose of this script is to install build dependencies and set
# $build_env to a function that sets appropriate environment variables,
# to enable (mingw32|mingw64) environment if we want to compile with gcc, or
# (mingw32|mingw64) + vcvarsall.bat if we want to compile with cl.exe

if [[ "$TRAVIS_OS_NAME" != "windows" ]]; then
    echo "Incorrect \$TRAVIS_OS_NAME: expected windows, got $TRAVIS_OS_NAME"
    exit 1
fi

[[ ! -f C:/tools/msys64/msys2_shell.cmd ]] && rm -rf C:/tools/msys64
choco uninstall -y mingw
choco upgrade --no-progress -y msys2

msys_shell_cmd="cmd //C RefreshEnv.cmd && set MSYS=winsymlinks:nativestrict && C:\\tools\\msys64\\msys2_shell.cmd"

msys2() { $msys_shell_cmd -defterm -no-start -msys2 -c "$*"; }
mingw32() { $msys_shell_cmd -defterm -no-start -mingw32 -c "$*"; }
mingw64() { $msys_shell_cmd -defterm -no-start -mingw64 -c "$*"; }

if [[ "$CROSS_COMPILE_32BIT" == "yes" ]]; then
    mingw=mingw32
    mingw_gcc_package_arch=i686
else
    mingw=mingw64
    mingw_gcc_package_arch=x86_64
fi

if [[ "$CC" == *"gcc"* ]]; then
    $mingw pacman -S --noconfirm --needed \
        autotools \
        git \
        mingw-w64-${mingw_gcc_package_arch}-make \
	    mingw-w64-${mingw_gcc_package_arch}-gcc \
	    mingw-w64-${mingw_gcc_package_arch}-binutils
    build_env=$mingw
elif [[ "$CC" == *"cl"* ]]; then
    $mingw pacman -S --noconfirm --needed \
        autotools \
	    git \
	    mingw-w64-${mingw_gcc_package_arch}-make \
	    mingw-w64-${mingw_gcc_package_arch}-binutils

    # In order to use MSVC compiler (cl.exe), we need to correctly set some environment
    # variables, namely PATH, INCLUDE, LIB and LIBPATH. The correct values of these
    # variables are set by a batch script "vcvarsall.bat". The code below generates
    # a batch script that calls "vcvarsall.bat" and prints the environment variables.
    #
    # Then, those environment variables are transformed from cmd to bash format and put
    # into a script $apply_vsenv. If cl.exe needs to be used from bash, one can
    # 'source $apply_vsenv' and it will apply the environment variables needed for cl.exe
    # to be located and function correctly.
    #
    # At last, a function "mingw_with_msvc_vars" is generated which forwards user input
    # into a correct mingw (32 or 64) subshell that automatically performs 'source $apply_vsenv',
    # making it possible for autotools to discover and use cl.exe.
    vcvarsall="vcvarsall.tmp.bat"
    echo "@echo off" > $vcvarsall
    echo "call \"c:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\\\vcvarsall.bat\" $USE_MSVC" >> $vcvarsall
    echo "set" >> $vcvarsall

    apply_vsenv="./apply_vsenv.sh"
    cmd //C $vcvarsall | grep -E "^PATH=" | sed -n -e 's/\(.*\)=\(.*\)/export \1=$PATH:"\2"/g' \
        -e 's/\([a-zA-Z]\):[\\\/]/\/\1\//g' \
        -e 's/\\/\//g' \
        -e 's/;\//:\//gp' > $apply_vsenv
    cmd //C $vcvarsall | grep -E "^(INCLUDE|LIB|LIBPATH)=" | sed -n -e 's/\(.*\)=\(.*\)/export \1="\2"/gp' >> $apply_vsenv

    cat $apply_vsenv
    mingw_with_msvc_vars() { $msys_shell_cmd -defterm -no-start -$mingw -c "source $apply_vsenv && ""$*"; }
    build_env=mingw_with_msvc_vars

    rm -f $vcvarsall
else
    echo "Unknown C compiler: $CC"
    exit 1
fi

echo "Build environment function: $build_env"
