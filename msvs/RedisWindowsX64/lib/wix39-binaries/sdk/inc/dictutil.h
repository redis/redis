#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="dictutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Header for string dict helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define ReleaseDict(sdh) if (sdh) { DictDestroy(sdh); }
#define ReleaseNullDict(sdh) if (sdh) { DictDestroy(sdh); sdh = NULL; }

typedef void* STRINGDICT_HANDLE;
typedef const void* C_STRINGDICT_HANDLE;

extern const int STRINGDICT_HANDLE_BYTES;

typedef enum DICT_FLAG
{
    DICT_FLAG_NONE = 0,
    DICT_FLAG_CASEINSENSITIVE = 1
} DICT_FLAG;

HRESULT DAPI DictCreateWithEmbeddedKey(
    __out_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE* psdHandle,
    __in DWORD dwNumExpectedItems,
    __in_opt void **ppvArray,
    __in size_t cByteOffset,
    __in DICT_FLAG dfFlags
    );
HRESULT DAPI DictCreateStringList(
    __out_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE* psdHandle,
    __in DWORD dwNumExpectedItems,
    __in DICT_FLAG dfFlags
    );
HRESULT DAPI DictCreateStringListFromArray(
    __out_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE* psdHandle,
    __in_ecount(cStringArray) const LPCWSTR* rgwzStringArray,
    __in const DWORD cStringArray,
    __in DICT_FLAG dfFlags
    );
HRESULT DAPI DictCompareStringListToArray(
    __in_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE sdStringList,
    __in_ecount(cStringArray) const LPCWSTR* rgwzStringArray,
    __in const DWORD cStringArray
    );
HRESULT DAPI DictAddKey(
    __in_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE sdHandle,
    __in_z LPCWSTR szString
    );
HRESULT DAPI DictAddValue(
    __in_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE sdHandle,
    __in void *pvValue
    );
HRESULT DAPI DictKeyExists(
    __in_bcount(STRINGDICT_HANDLE_BYTES) C_STRINGDICT_HANDLE sdHandle,
    __in_z LPCWSTR szString
    );
HRESULT DAPI DictGetValue(
    __in_bcount(STRINGDICT_HANDLE_BYTES) C_STRINGDICT_HANDLE sdHandle,
    __in_z LPCWSTR szString,
    __out void **ppvValue
    );
void DAPI DictDestroy(
    __in_bcount(STRINGDICT_HANDLE_BYTES) STRINGDICT_HANDLE sdHandle
    );

#ifdef __cplusplus
}
#endif
