//-------------------------------------------------------------------------------------------------
// <copyright file="downloadengine.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Setup chainer/bootstrapper download engine for WiX toolset.
// </summary>
//-------------------------------------------------------------------------------------------------

#pragma once


#ifdef __cplusplus
extern "C" {
#endif

typedef	HRESULT (WINAPI *LPAUTHENTICATION_ROUTINE)(
    __in LPVOID pVoid,
    __in HINTERNET hUrl,
    __in long lHttpCode,
    __out BOOL* pfRetrySend,
    __out BOOL* pfRetry
    );

typedef int (WINAPI *LPCANCEL_ROUTINE)(
    __in HRESULT hrError,
    __in_z_opt LPCWSTR wzError,
    __in BOOL fAllowRetry,
    __in_opt LPVOID pvContext
    );

// structs
typedef struct _DOWNLOAD_SOURCE
{
    LPWSTR sczUrl;
    LPWSTR sczUser;
    LPWSTR sczPassword;
} DOWNLOAD_SOURCE;

typedef struct _DOWNLOAD_CACHE_CALLBACK
{
    LPPROGRESS_ROUTINE pfnProgress;
    LPCANCEL_ROUTINE pfnCancel;
    LPVOID pv;
} DOWNLOAD_CACHE_CALLBACK;

typedef struct _DOWNLOAD_AUTHENTICATION_CALLBACK
{
    LPAUTHENTICATION_ROUTINE pfnAuthenticate;
    LPVOID pv;
} DOWNLOAD_AUTHENTICATION_CALLBACK;


// functions

HRESULT DAPI DownloadUrl(
    __in DOWNLOAD_SOURCE* pDownloadSource,
    __in DWORD64 dw64AuthoredDownloadSize,
    __in LPCWSTR wzDestinationPath,
    __in_opt DOWNLOAD_CACHE_CALLBACK* pCache,
    __in_opt DOWNLOAD_AUTHENTICATION_CALLBACK* pAuthenticate
    );


#ifdef __cplusplus
}
#endif
