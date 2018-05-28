#!/bin/bash
#
#  Copyright (C) 2018 Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright notice(s),
#     this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright notice(s),
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
#  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
#  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
#  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set -e

export JE_PREFIX=jemk_

cd memkind
EXTRA_CONF=$@
if [ ! -f ./jemalloc/obj/lib/libjemalloc_pic.a ]; then
        cd jemalloc
	if [ ! -f configure ]; then
		autoconf
	fi
	mkdir -p obj
	cd obj
	../configure --with-jemalloc-prefix=$JE_PREFIX --enable-autogen --with-version=4.5.0-0-g0 --with-private-namespace=mk_

	make -j`nproc`
	cd ../..
fi

if [ ! -f VERSION ]; then
	./autogen.sh
fi

if [ ! -f ./configure ]; then
	./autogen.sh
fi

if [ ! -f ./Makefile ]; then
        ./configure $EXTRA_CONF
fi
#use V=1 for full cmdlines of build
make all -j $EXTRA_CONF

