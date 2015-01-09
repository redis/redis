#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="wcalog.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
// 
// <summary>
//    Private header for internal logging functions
// </summary>
//-------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

BOOL WIXAPI IsVerboseLogging();
HRESULT WIXAPI SetVerboseLoggingAtom(BOOL bValue);

#ifdef __cplusplus
}
#endif
