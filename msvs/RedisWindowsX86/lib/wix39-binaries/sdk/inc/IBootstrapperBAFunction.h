//-------------------------------------------------------------------------------------------------
// <copyright file="IBootstrapperBAFunction.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
//-------------------------------------------------------------------------------------------------

#pragma once

#include <windows.h>

#include "IBootstrapperEngine.h"

interface IBootstrapperBAFunction
{
    STDMETHOD(OnDetect)() = 0;
    STDMETHOD(OnDetectComplete)() = 0;
    STDMETHOD(OnPlan)() = 0;
    STDMETHOD(OnPlanComplete)() = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

typedef HRESULT (WINAPI *PFN_BOOTSTRAPPER_BA_FUNCTION_CREATE)(
    __in IBootstrapperEngine* pEngine,
    __in HMODULE hModule,
    __out IBootstrapperBAFunction** ppBAFunction
    );

#ifdef __cplusplus
}
#endif
