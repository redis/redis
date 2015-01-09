#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="jsonutil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    JavaScript Object Notation (JSON) helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JSON_TOKEN
{
    JSON_TOKEN_NONE,
    JSON_TOKEN_ARRAY_START,
    JSON_TOKEN_ARRAY_VALUE,
    JSON_TOKEN_ARRAY_END,
    JSON_TOKEN_OBJECT_START,
    JSON_TOKEN_OBJECT_KEY,
    JSON_TOKEN_OBJECT_VALUE,
    JSON_TOKEN_OBJECT_END,
    JSON_TOKEN_VALUE,
} JSON_TOKEN;

typedef struct _JSON_VALUE
{
} JSON_VALUE;

typedef struct _JSON_READER
{
    CRITICAL_SECTION cs;
    LPWSTR sczJson;

    LPWSTR pwz;
    JSON_TOKEN token;
} JSON_READER;

typedef struct _JSON_WRITER
{
    CRITICAL_SECTION cs;
    LPWSTR sczJson;

    JSON_TOKEN* rgTokenStack;
    DWORD cTokens;
    DWORD cMaxTokens;
} JSON_WRITER;


DAPI_(HRESULT) JsonInitializeReader(
    __in_z LPCWSTR wzJson,
    __in JSON_READER* pReader
    );

DAPI_(void) JsonUninitializeReader(
    __in JSON_READER* pReader
    );

DAPI_(HRESULT) JsonReadNext(
    __in JSON_READER* pReader,
    __out JSON_TOKEN* pToken,
    __out JSON_VALUE* pValue
    );

DAPI_(HRESULT) JsonReadValue(
    __in JSON_READER* pReader,
    __in JSON_VALUE* pValue
    );

DAPI_(HRESULT) JsonInitializeWriter(
    __in JSON_WRITER* pWriter
    );

DAPI_(void) JsonUninitializeWriter(
    __in JSON_WRITER* pWriter
    );

DAPI_(HRESULT) JsonWriteBool(
    __in JSON_WRITER* pWriter,
    __in BOOL fValue
    );

DAPI_(HRESULT) JsonWriteNumber(
    __in JSON_WRITER* pWriter,
    __in DWORD dwValue
    );

DAPI_(HRESULT) JsonWriteString(
    __in JSON_WRITER* pWriter,
    __in_z LPCWSTR wzValue
    );

DAPI_(HRESULT) JsonWriteArrayStart(
    __in JSON_WRITER* pWriter
    );

DAPI_(HRESULT) JsonWriteArrayEnd(
    __in JSON_WRITER* pWriter
    );

DAPI_(HRESULT) JsonWriteObjectStart(
    __in JSON_WRITER* pWriter
    );

DAPI_(HRESULT) JsonWriteObjectKey(
    __in JSON_WRITER* pWriter,
    __in_z LPCWSTR wzKey
    );

DAPI_(HRESULT) JsonWriteObjectEnd(
    __in JSON_WRITER* pWriter
    );

#ifdef __cplusplus
}
#endif
