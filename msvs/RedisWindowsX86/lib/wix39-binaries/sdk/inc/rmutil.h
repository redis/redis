#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="rmutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Header for Restart Manager utility functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _RMU_SESSION *PRMU_SESSION;

HRESULT DAPI RmuJoinSession(
    __out PRMU_SESSION *ppSession,
    __in_z LPCWSTR wzSessionKey
    );

HRESULT DAPI RmuAddFile(
    __in PRMU_SESSION pSession,
    __in_z LPCWSTR wzPath
    );

HRESULT DAPI RmuAddProcessById(
    __in PRMU_SESSION pSession,
    __in DWORD dwProcessId
    );

HRESULT DAPI RmuAddProcessesByName(
    __in PRMU_SESSION pSession,
    __in_z LPCWSTR wzProcessName
    );

HRESULT DAPI RmuAddService(
    __in PRMU_SESSION pSession,
    __in_z LPCWSTR wzServiceName
    );

HRESULT DAPI RmuRegisterResources(
    __in PRMU_SESSION pSession
    );

HRESULT DAPI RmuEndSession(
    __in PRMU_SESSION pSession
    );

#ifdef __cplusplus
}
#endif
