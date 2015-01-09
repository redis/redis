#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="osutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Operating system helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OS_VERSION
{
    OS_VERSION_UNKNOWN,
    OS_VERSION_WINNT,
    OS_VERSION_WIN2000,
    OS_VERSION_WINXP,
    OS_VERSION_WIN2003,
    OS_VERSION_VISTA,
    OS_VERSION_WIN2008,
    OS_VERSION_WIN7,
    OS_VERSION_WIN2008_R2,
    OS_VERSION_FUTURE
} OS_VERSION;

void DAPI OsGetVersion(
    __out OS_VERSION* pVersion,
    __out DWORD* pdwServicePack
    );
HRESULT DAPI OsCouldRunPrivileged(
    __out BOOL* pfPrivileged
    );
HRESULT DAPI OsIsRunningPrivileged(
    __out BOOL* pfPrivileged
    );
HRESULT DAPI OsIsUacEnabled(
    __out BOOL* pfUacEnabled
    );

#ifdef __cplusplus
}
#endif
