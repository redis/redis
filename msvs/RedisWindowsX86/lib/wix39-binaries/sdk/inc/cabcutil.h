#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="cabcutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
//
// <summary>
//    Header for cabinet creation helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#include <fci.h>
#include <fcntl.h>
#include <msi.h>

// Callback from PFNFCIGETNEXTCABINET CabCGetNextCabinet method
// First argument is the name of splitting cabinet without extension e.g. "cab1"
// Second argument is name of the new cabinet that would be formed by splitting e.g. "cab1b.cab"
// Third argument is the file token of the first file present in the splitting cabinet
typedef void (__stdcall * FileSplitCabNamesCallback)(LPWSTR, LPWSTR, LPWSTR);

#define CAB_MAX_SIZE 0x7FFFFFFF   // (see KB: Q174866)

#ifdef __cplusplus
extern "C" {
#endif

extern const int CABC_HANDLE_BYTES;

// time vs. space trade-off
typedef enum COMPRESSION_TYPE
{
    COMPRESSION_TYPE_NONE, // fastest
    COMPRESSION_TYPE_LOW,
    COMPRESSION_TYPE_MEDIUM,
    COMPRESSION_TYPE_HIGH, // smallest
    COMPRESSION_TYPE_MSZIP
} COMPRESSION_TYPE;

// functions
HRESULT DAPI CabCBegin(
    __in_z LPCWSTR wzCab,
    __in_z LPCWSTR wzCabDir,
    __in DWORD dwMaxFiles,
    __in DWORD dwMaxSize,
    __in DWORD dwMaxThresh,
    __in COMPRESSION_TYPE ct,
    __out_bcount(CABC_HANDLE_BYTES) HANDLE *phContext
    );
HRESULT DAPI CabCNextCab(
    __in_bcount(CABC_HANDLE_BYTES) HANDLE hContext
    );
HRESULT DAPI CabCAddFile(
    __in_z LPCWSTR wzFile,
    __in_z_opt LPCWSTR wzToken,
    __in_opt PMSIFILEHASHINFO pmfHash,
    __in_bcount(CABC_HANDLE_BYTES) HANDLE hContext
    );
HRESULT DAPI CabCFinish(
    __in_bcount(CABC_HANDLE_BYTES) HANDLE hContext,
    __in_opt FileSplitCabNamesCallback fileSplitCabNamesCallback
    );
void DAPI CabCCancel(
    __in_bcount(CABC_HANDLE_BYTES) HANDLE hContext
    );

#ifdef __cplusplus
}
#endif
