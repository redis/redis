#pragma once
//-------------------------------------------------------------------------------------------------
// <copyright file="balcondition.h" company="Outercurve Foundation">
//   Copyright (c) 2004, Outercurve Foundation.
//   This software is released under Microsoft Reciprocal License (MS-RL).
//   The license and further copyright text can be found in the file
//   LICENSE.TXT at the root directory of the distribution.
// </copyright>
//
// <summary>
// Bootstrapper Application Layer condition utility.
// </summary>
//-------------------------------------------------------------------------------------------------


#ifdef __cplusplus
extern "C" {
#endif

typedef struct _BAL_CONDITION
{
    LPWSTR sczCondition;
    LPWSTR sczMessage;
} BAL_CONDITION;


typedef struct _BAL_CONDITIONS
{
    BAL_CONDITION* rgConditions;
    DWORD cConditions;
} BAL_CONDITIONS;


/*******************************************************************
 BalConditionsParseFromXml - loads the conditions from the UX manifest.

********************************************************************/
DAPI_(HRESULT) BalConditionsParseFromXml(
    __in BAL_CONDITIONS* pConditions,
    __in IXMLDOMDocument* pixdManifest,
    __in_opt WIX_LOCALIZATION* pWixLoc
    );


/*******************************************************************
 BalConditionEvaluate - evaluates condition against the provided IBurnCore.

 NOTE: psczMessage is optional.
********************************************************************/
DAPI_(HRESULT) BalConditionEvaluate(
    __in BAL_CONDITION* pCondition,
    __in IBootstrapperEngine* pEngine,
    __out BOOL* pfResult,
    __out_z_opt LPWSTR* psczMessage
    );


/*******************************************************************
 BalConditionsUninitialize - uninitializes any conditions previously loaded.

********************************************************************/
DAPI_(void) BalConditionsUninitialize(
    __in BAL_CONDITIONS* pConditions
    );


#ifdef __cplusplus
}
#endif
