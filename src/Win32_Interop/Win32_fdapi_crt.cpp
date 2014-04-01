/*
* Copyright (c), Microsoft Open Technologies, Inc.
* All rights reserved.
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*  - Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*  - Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "Win32_fdapi_crt.h"
#include <io.h>
#include <stdlib.h>

int crt_close(int fd) {
	return _close(fd);
}

int crt_read(int fd, void *buffer, unsigned int count) {
	return _read(fd, buffer, count);
}

int crt_write(int fd, const void *buffer, unsigned int count) {
	return _write(fd, buffer, count);
}

int crt_open(const char *filename, int oflag, int pmode) {
	return _open(filename, oflag, pmode);
}

intptr_t crtget_osfhandle(int fd) {
	return _get_osfhandle(fd);
}

int crtsetmode(int fd, int mode) {
	return ::_setmode(fd, mode);
}

int crt_isatty(int fd) {
	return _isatty(fd);
}

int crt_access(const char *pathname, int mode) {
	return _access(pathname, mode);
}

__int64 crt_lseek64(int fd, __int64 offset, int origin) {
	return _lseeki64(fd, offset, origin);
}

