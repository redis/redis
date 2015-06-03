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

#include "win32_types.h"

#include "Win32_variadicFunctor.h"

#include <windows.h>
#include <stdexcept>
#include <map>
using namespace std;

DLLMap& DLLMap::getInstance() {
	static DLLMap    instance; // Instantiated on first use. Guaranteed to be destroyed.
	return instance;
}

DLLMap::DLLMap() { };

LPVOID DLLMap::getProcAddress(string dll, string functionName)
{
	if (find(dll) == end()) {
		HMODULE mod = LoadLibraryA(dll.c_str());
		if (mod == NULL) {
			throw system_error(GetLastError(), system_category(), "LoadLibrary failed");
		}
		(*this)[dll] = mod;
	}

	HMODULE mod = (*this)[dll];
	LPVOID fp = GetProcAddress(mod, functionName.c_str());
	if (fp == nullptr) {
		throw system_error(GetLastError(), system_category(), "LoadLibrary failed");
	}

	return fp;
}

DLLMap::~DLLMap()
{
	for each(auto modPair in (*this))
	{
		FreeLibrary(modPair.second);
	}
}
