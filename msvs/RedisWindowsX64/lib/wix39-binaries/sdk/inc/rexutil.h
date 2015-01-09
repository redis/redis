#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="rexutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Resource Cabinet Extract Utilities
// </summary>
//-------------------------------------------------------------------------------------------------

#include <sys\stat.h>
#include <fdi.h>

#ifdef __cplusplus
extern "C" {
#endif

// defines
#define FILETABLESIZE 40

// structs
struct MEM_FILE 
{
    LPCBYTE vpStart;
    UINT  uiCurrent;
    UINT  uiLength;
};

typedef enum FAKE_FILE_TYPE { NORMAL_FILE, MEMORY_FILE } FAKE_FILE_TYPE;

typedef HRESULT (*REX_CALLBACK_PROGRESS)(BOOL fBeginFile, LPCWSTR wzFileId, LPVOID pvContext);
typedef VOID (*REX_CALLBACK_WRITE)(UINT cb);


struct FAKE_FILE // used __in internal file table
{
    BOOL fUsed;
    FAKE_FILE_TYPE fftType;
    MEM_FILE mfFile; // State for memory file
    HANDLE hFile; // Handle for disk  file
};

// functions
HRESULT RexInitialize();
void RexUninitialize();

HRESULT RexExtract(
    __in_z LPCSTR szResource,
    __in_z LPCWSTR wzExtractId,
    __in_z LPCWSTR wzExtractDir,
    __in_z LPCWSTR wzExtractName,
    __in REX_CALLBACK_PROGRESS pfnProgress,
    __in REX_CALLBACK_WRITE pfnWrite,
    __in LPVOID pvContext
    );

#ifdef __cplusplus
}
#endif

