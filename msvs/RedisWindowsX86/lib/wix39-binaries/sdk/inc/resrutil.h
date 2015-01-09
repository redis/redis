//-------------------------------------------------------------------------------------------------
// <copyright file="resrutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Resource read helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#pragma once


#ifdef __cplusplus
extern "C" {
#endif

HRESULT DAPI ResGetStringLangId(
    __in_opt LPCWSTR wzPath,
    __in UINT uID,
    __out WORD *pwLangId
    );

HRESULT DAPI ResReadString(
    __in HINSTANCE hinst,
    __in UINT uID,
    __deref_out_z LPWSTR* ppwzString
    );

HRESULT DAPI ResReadStringAnsi(
    __in HINSTANCE hinst,
    __in UINT uID,
    __deref_out_z LPSTR* ppszString
    );

HRESULT DAPI ResReadData(
    __in_opt HINSTANCE hinst,
    __in_z LPCSTR szDataName,
    __deref_out_bcount(*pcb) PVOID *ppv,
    __out DWORD *pcb
    );

HRESULT DAPI ResExportDataToFile(
    __in_z LPCSTR szDataName,
    __in_z LPCWSTR wzTargetFile,
    __in DWORD dwCreationDisposition
    );

#ifdef __cplusplus
}
#endif

