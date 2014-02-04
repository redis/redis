/***********************************************************************
 * Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
 * A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache License, Version 2.0 for specific language governing
 * permissions and limitations under the License.
 *
 **********************************************************************/

#include "stdafx.h"
#include "watcher.h"

//
// Purpose:
//   Allocate memory and copy string
//    If destination is not NULL, free the existing pointer
//
// Parameters:
//   value is source string, dest is destination reference
//
// Return value:
//   TRUE or FALSE
//
BOOL CopyString(wchar_t * value, wchar_t ** dest)
{
    size_t len = wcslen(value) + 1;
    if (*dest != NULL) 
        free (*dest);
    *dest = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (*dest == NULL)
    {
        _set_errno(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (wcscpy_s(*dest, len, value) == 0)
    {
        return TRUE;
    }
    return FALSE;
}

//
// Purpose:
//   Combine path and file name into a fqn
//    If fullpath is not NULL, free the existing pointer
//
// Parameters:
//   path, filename, fullpath is reference for FQN
//
// Return value:
//   TRUE or FALSE
//
BOOL CombineFilePath(wchar_t * path, wchar_t * filename, wchar_t ** fullpath)
{
    wchar_t * combpath;
    size_t pathlen = wcslen(path);
    size_t len = pathlen + wcslen(filename) + 2;

    combpath = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (combpath == NULL)
    {
        _set_errno(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (wcscpy_s(combpath, len, path) != 0)
    {
        free (combpath);
        return FALSE;
    }

    if (pathlen > 0 && path[pathlen - 1] != L'\\')
    {
        if (wcscat_s(combpath, len, L"\\") != 0)
        {
            free (combpath);
            return FALSE;
        }
    }

    if (wcscat_s(combpath, len, filename) != 0)
    {
        free (combpath);
        return FALSE;
    }

    if (*fullpath != NULL) 
        free (*fullpath);
    *fullpath = combpath;
    return TRUE;
}

//
// Purpose:
//   Get the current working directory
//    If fullpath is not NULL, free the existing pointer
//
// Parameters:
//   fullpath is reference for path
//
// Return value:
//   TRUE or FALSE
//
BOOL GetCurrentDir(wchar_t ** fullpath)
{
    wchar_t * combpath;
    size_t pathlen = 0;

    pathlen = GetCurrentDirectory(0, NULL);
    if (pathlen > 0)
    {
        combpath = (wchar_t *)malloc(pathlen * sizeof(wchar_t));
        if (combpath == NULL)
        {
            _set_errno(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }

        GetCurrentDirectory(pathlen, combpath);
        if (*fullpath != NULL) 
            free (*fullpath);
        *fullpath = combpath;
        return TRUE;
    }
    return FALSE;
}

//
// Purpose:
//   Get the current module path
//    If fullpath is not NULL, free the existing pointer
//    Used for running as service instead of CurrentDir
//
// Parameters:
//   fullpath is reference for path
//
// Return value:
//   TRUE or FALSE
//
BOOL GetModulePath(wchar_t ** fullpath)
{
    wchar_t * combpath;
    size_t pathlen = MAX_PATH;
    wchar_t * pos;
    wchar_t * lastSep;

    combpath = (wchar_t *)malloc(pathlen * sizeof(wchar_t));
    while (combpath != NULL && (pathlen == GetModuleFileName(NULL, combpath, pathlen)))
    {
        free(combpath);
        pathlen = pathlen * 2;
        combpath = (wchar_t *)malloc(pathlen * sizeof(wchar_t));
    }

    if (combpath == NULL)
    {
        _set_errno(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    // terminate at last slash
    pos = combpath;
    lastSep = NULL;

    while (*pos != L'\0')
    {
        if (*pos == L'\\')
            lastSep = pos;
        pos++;
    }
    if (lastSep != NULL)
    {
        *lastSep = L'\0';
        if (*fullpath != NULL)
            free (*fullpath);
        *fullpath = combpath;
        return TRUE;
    }

    return FALSE;
}

//
// Purpose:
//   Remove extra whitespace around a configuration token
//
// Parameters:
//   string
//
// Return value:
//   string
//
const wchar_t wspace[] = L" \t\r\n";
wchar_t * Trim(wchar_t * buf)
{
    wchar_t * sp = buf;
    wchar_t * ep = buf + wcslen(buf) - 1;

    while (sp < ep && wcschr(wspace, *sp) != NULL)
        sp++;
    while (sp < ep && wcschr(wspace, *ep) != NULL)
        ep--;
    ep++;
    *ep = L'\0';
    return sp;
}

