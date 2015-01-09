//-------------------------------------------------------------------------------------------------
// <copyright file="butil.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Header for bundle helper functions.
// </summary>
//-------------------------------------------------------------------------------------------------
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum BUNDLE_INSTALL_CONTEXT
{
    BUNDLE_INSTALL_CONTEXT_MACHINE,
    BUNDLE_INSTALL_CONTEXT_USER,
};

HRESULT DAPI BundleGetBundleInfo(
  __in LPCWSTR   szBundleId,                             // Bundle code
  __in LPCWSTR   szAttribute,                            // attribute name
  __out_ecount_opt(*pcchValueBuf) LPWSTR lpValueBuf,     // returned value, NULL if not desired
  __inout_opt                     LPDWORD pcchValueBuf   // in/out buffer character count
    );

HRESULT DAPI BundleEnumRelatedBundle(
  __in     LPCWSTR lpUpgradeCode,
  __in     BUNDLE_INSTALL_CONTEXT context,
  __inout  PDWORD pdwStartIndex,
  __out_ecount(MAX_GUID_CHARS+1)  LPWSTR lpBundleIdBuf
    );

#ifdef __cplusplus
}
#endif