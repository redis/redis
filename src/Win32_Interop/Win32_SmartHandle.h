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

#pragma once

#include <Windows.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <cstdint>
using namespace std;

typedef class SmartHandle
{
private:
    HANDLE m_handle;

public:
    HANDLE Assign(HANDLE h)
    {
        Close();
        m_handle = h;
        if (Invalid())
            throw std::runtime_error("invalid handle passed to constructor");
        return h;
    }

    SmartHandle()
    {
        m_handle = NULL;
    }

    SmartHandle(HANDLE handle)
    {
        m_handle = handle;
        if (Invalid())
            throw std::runtime_error("invalid handle passed to constructor");
    }

    HANDLE Assign(HANDLE h, string errorToReport)
    {
        Close();
        m_handle = h;
        if (Invalid())
            throw std::runtime_error(errorToReport);
        return h;
    }

    SmartHandle(HANDLE handle, string errorToReport)
    {
        Close();
        m_handle = handle;
        if (Invalid())
            throw std::runtime_error(errorToReport);
    }

    HANDLE Assign(HANDLE parentProcess, HANDLE parentHandleToDuplicate)
    {
        Close();
        if (!DuplicateHandle(parentProcess, parentHandleToDuplicate, GetCurrentProcess(), &m_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::system_error(GetLastError(), system_category(), "handle duplication failed");
        return m_handle;
    }

    SmartHandle(HANDLE parentProcess, HANDLE parentHandleToDuplicate)
    {
        if (!DuplicateHandle(parentProcess, parentHandleToDuplicate, GetCurrentProcess(), &m_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::system_error(GetLastError(), system_category(), "handle duplication failed");
    }

    operator PHANDLE() {
        return &m_handle;
    }

    operator HANDLE()
    {
        return m_handle;
    }

    BOOL Valid()
    {
        return (m_handle != INVALID_HANDLE_VALUE) && (m_handle != NULL);
    }

    BOOL Invalid()
    {
        return (m_handle == INVALID_HANDLE_VALUE) || (m_handle == NULL);
    }

    void Close()
    {
        if (Valid())
        {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    ~SmartHandle()
    {
        Close();
    }
} SmartHandle;

template <class T>
class SmartFileView
{
private:
    T* m_viewPtr;

public:
    T* operator->()
    {
        return m_viewPtr;
    }

    operator T* ()
    {
        return m_viewPtr;
    }

    SmartFileView()
    {
        m_viewPtr = NULL;
    }

    T* Assign(HANDLE fileMapHandle, DWORD desiredAccess, string errorToReport)
    {
        UnmapViewOfFile();
        m_viewPtr = (T*) MapViewOfFile(fileMapHandle, desiredAccess, 0, 0, sizeof(T));
        if (Invalid()) {
            if (IsDebuggerPresent()) DebugBreak();
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
        return m_viewPtr;
    }

    SmartFileView(HANDLE fileMapHandle, DWORD desiredAccess, string errorToReport)
    {
        m_viewPtr = (T*) MapViewOfFile(fileMapHandle, desiredAccess, 0, 0, sizeof(T));
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
    }

    T* Assign(HANDLE fileMapHandle, DWORD desiredAccess, DWORD fileOffsetHigh, DWORD fileOffsetLow, SIZE_T bytesToMap, string errorToReport)
    {
        UnmapViewOfFile();
        m_viewPtr = (T*) MapViewOfFile(fileMapHandle, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap);
        if (Invalid()) {
            if (IsDebuggerPresent()) DebugBreak();
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
        return m_viewPtr;
    }

    SmartFileView(HANDLE fileMapHandle, DWORD desiredAccess, DWORD fileOffsetHigh, DWORD fileOffsetLow, SIZE_T bytesToMap, string errorToReport)
    {
        m_viewPtr = (T*) MapViewOfFile(fileMapHandle, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap);
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
    }

    T* Assign(HANDLE fileMapHandle, DWORD desiredAccess, DWORD fileOffsetHigh, DWORD fileOffsetLow, SIZE_T bytesToMap, LPVOID baseAddress, string errorToReport)
    {
        UnmapViewOfFile();
        m_viewPtr = (T*) MapViewOfFileEx(fileMapHandle, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap, baseAddress);
        if (Invalid()) {
            if (IsDebuggerPresent()) DebugBreak();
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
        return m_viewPtr;
    }

    SmartFileView(HANDLE fileMapHandle, DWORD desiredAccess, DWORD fileOffsetHigh, DWORD fileOffsetLow, SIZE_T bytesToMap, LPVOID baseAddress, string errorToReport)
    {
        m_viewPtr = (T*) MapViewOfFileEx(fileMapHandle, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap, baseAddress);
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
    }

    void Remap(HANDLE fileMapHandle, DWORD desiredAccess, DWORD fileOffsetHigh, DWORD fileOffsetLow, SIZE_T bytesToMap, LPVOID baseAddress, string errorToReport)
    {
        if (Valid())
            throw new invalid_argument("m_viewPtr still valid");
        m_viewPtr = (T*) MapViewOfFileEx(fileMapHandle, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap, baseAddress);
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport.c_str());
        }
    }

    BOOL Valid()
    {
        return (m_viewPtr != NULL);
    }

    BOOL Invalid()
    {
        return (m_viewPtr == NULL);
    }

    void UnmapViewOfFile()
    {
        if (m_viewPtr != NULL)
        {
            if (!::UnmapViewOfFile(m_viewPtr))
                throw system_error(GetLastError(), system_category(), "UnmapViewOfFile failed");

            m_viewPtr = NULL;
        }
    }

    ~SmartFileView()
    {
        UnmapViewOfFile();
    }
};

typedef class SmartFileMapHandle
{
private:
    HANDLE m_handle;
    DWORD systemAllocationGranularity;

public:
    operator HANDLE()
    {
        return m_handle;
    }

    SmartFileMapHandle()
    {
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE Assign(HANDLE mmFile, DWORD protectionFlags, DWORD maxSizeHigh, DWORD maxSizeLow, string errorToReport)
    {
        Unmap();
        m_handle = CreateFileMapping(mmFile, NULL, protectionFlags, maxSizeHigh, maxSizeLow, NULL);
        if (Invalid()) {
            if (IsDebuggerPresent()) DebugBreak();
            throw std::system_error(GetLastError(), system_category(), errorToReport);
        }

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        systemAllocationGranularity = si.dwAllocationGranularity;
        return m_handle;
    }

    SmartFileMapHandle(HANDLE mmFile, DWORD protectionFlags, DWORD maxSizeHigh, DWORD maxSizeLow, string errorToReport)
    {
        m_handle = CreateFileMapping(mmFile, NULL, protectionFlags, maxSizeHigh, maxSizeLow, NULL);
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport);
        }

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        systemAllocationGranularity = si.dwAllocationGranularity;
    }

    void Unmap()
    {
        if (Valid()) {
            CloseHandle(m_handle);
        }
        m_handle = NULL;
    }

    void Remap(HANDLE mmFile, DWORD protectionFlags, DWORD maxSizeHigh, DWORD maxSizeLow, string errorToReport)
    {
        m_handle = CreateFileMapping(mmFile, NULL, protectionFlags, maxSizeHigh, maxSizeLow, NULL);
        if (Invalid()) {
            throw std::system_error(GetLastError(), system_category(), errorToReport);
        }
    }

    BOOL Valid()
    {
        return (m_handle != INVALID_HANDLE_VALUE) && (m_handle != NULL);
    }

    BOOL Invalid()
    {
        return (m_handle == INVALID_HANDLE_VALUE) || (m_handle == NULL);
    }

    ~SmartFileMapHandle()
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }

} SmartFileMapHandle;


typedef class SmartServiceHandle
{
private:
    SC_HANDLE m_handle;

public:
    operator SC_HANDLE()
    {
        return m_handle;
    }

    SmartServiceHandle & operator= (const SC_HANDLE handle)
    {
        m_handle = handle;
        return *this;
    }

    SmartServiceHandle()
    {
        m_handle = NULL;
    }

    SmartServiceHandle(const SC_HANDLE handle)
    {
        m_handle = handle;
    }

    BOOL Valid()
    {
        return (m_handle != NULL);
    }

    BOOL Invalid()
    {
        return (m_handle == NULL);
    }

    ~SmartServiceHandle()
    {
        CloseServiceHandle(m_handle);
        m_handle = NULL;
    }
} SmartServiceHandle;

typedef class SmartRegistryHandle {
private:
    HKEY m_handle;

public:
    operator HKEY() {
        return m_handle;
    }

    operator HKEY* () {
        return &m_handle;
    }

    SmartRegistryHandle & operator= (const HKEY handle) {
        m_handle = handle;
        return *this;
    }

    SmartRegistryHandle() {
        m_handle = NULL;
    }

    SmartRegistryHandle(const HKEY handle) {
        m_handle = handle;
    }

    BOOL Valid() {
        return (m_handle != NULL);
    }

    BOOL Invalid() {
        return (m_handle == NULL);
    }

    ~SmartRegistryHandle() {
        RegCloseKey(m_handle);
        m_handle = NULL;
    }
} SmartRegistryHandle;
