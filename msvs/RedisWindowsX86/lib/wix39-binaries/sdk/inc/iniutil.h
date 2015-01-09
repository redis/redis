#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="iniutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Ini/cfg file helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#define ReleaseIni(ih) if (ih) { IniUninitialize(ih); }
#define ReleaseNullIni(ih) if (ih) { IniUninitialize(ih); ih = NULL; }

typedef void* INI_HANDLE;
typedef const void* C_INI_HANDLE;

extern const int INI_HANDLE_BYTES;

struct INI_VALUE
{
    LPCWSTR wzName;
    LPCWSTR wzValue;

    DWORD dwLineNumber;
};

HRESULT DAPI IniInitialize(
    __out_bcount(INI_HANDLE_BYTES) INI_HANDLE* piHandle
    );
void DAPI IniUninitialize(
    __in_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle
    );
HRESULT DAPI IniSetOpenTag(
    __inout_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in_z_opt LPCWSTR wzOpenTagPrefix,
    __in_z_opt LPCWSTR wzOpenTagPostfix
    );
HRESULT DAPI IniSetValueStyle(
    __inout_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in_z_opt LPCWSTR wzValuePrefix,
    __in_z_opt LPCWSTR wzValueSeparator
    );
HRESULT DAPI IniSetCommentStyle(
    __inout_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in_z_opt LPCWSTR wzLinePrefix
    );
HRESULT DAPI IniParse(
    __inout_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in LPCWSTR wzPath,
    __out_opt FILE_ENCODING *pfeEncodingFound
    );
HRESULT DAPI IniGetValueList(
    __in_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __deref_out_ecount_opt(pcValues) INI_VALUE** prgivValues,
    __out DWORD *pcValues
    );
HRESULT DAPI IniGetValue(
    __in_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in LPCWSTR wzValueName,
    __deref_out_z LPWSTR* psczValue
    );
HRESULT DAPI IniSetValue(
    __in_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in LPCWSTR wzValueName,
    __in_z_opt LPCWSTR wzValue
    );
HRESULT DAPI IniWriteFile(
    __in_bcount(INI_HANDLE_BYTES) INI_HANDLE piHandle,
    __in_z_opt LPCWSTR wzPath,
    __in FILE_ENCODING feOverrideEncoding
    );

#ifdef __cplusplus
}
#endif
