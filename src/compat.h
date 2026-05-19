#pragma once
// Version-compatibility shims for AC26 ↔ AC27+
// AC27+ renamed ACAPI_LibPart_* → ACAPI_LibraryPart_*, and replaced dispatch
// functions (ACAPI_Environment, ACAPI_Automate, ACAPI_Goodies, ACAPI_Interface)
// with direct function calls. This file maps old names and wraps dispatch calls.

#include "ACAPinc.h"

// AC28+ removed __ACENV_CALL (it was always empty on macOS anyway)
#ifndef __ACENV_CALL
#define __ACENV_CALL
#endif

#ifdef ServerMainVers_2700  // AC27 or newer

// ── LibPart → LibraryPart ──────────────────────────────────────────────────
#define ACAPI_LibPart_GetNum    ACAPI_LibraryPart_GetNum
#define ACAPI_LibPart_Get       ACAPI_LibraryPart_Get
#define ACAPI_LibPart_GetParams ACAPI_LibraryPart_GetParams
#define ACAPI_LibPart_Create    ACAPI_LibraryPart_Create
#define ACAPI_LibPart_Save      ACAPI_LibraryPart_Save

// ── Menu registration ──────────────────────────────────────────────────────
#define ACAPI_Register_Menu       ACAPI_MenuItem_RegisterMenu
#define ACAPI_Install_MenuHandler ACAPI_MenuItem_InstallMenuHandler

// ── Additional headers ────────────────────────────────────────────────────
#include "ACAPI_Environment.h"
#include "ACAPI_Automate.h"
#include "ACAPI_Goodies.h"
#include "ACAPI_Interface.h"

// ── Library management ────────────────────────────────────────────────────
inline GSErrCode Compat_GetLibraries(GS::Array<API_LibraryInfo>* libs, Int32* embIdx)
    { return ACAPI_LibraryManagement_GetLibraries(libs, embIdx); }
inline GSErrCode Compat_SetLibraries(const GS::Array<API_LibraryInfo>* libs)
    { return ACAPI_LibraryManagement_SetLibraries(libs); }
inline GSErrCode Compat_ReloadLibraries()
    { return ACAPI_ProjectOperation_ReloadLibraries(); }
inline GSErrCode Compat_GetStorySettings(API_StoryInfo* info)
    { return ACAPI_ProjectSetting_GetStorySettings(info); }

// ── LibPart parameters (formerly ACAPI_Goodies) ───────────────────────────
inline GSErrCode Compat_OpenParameters(API_ParamOwnerType* owner)
    { return ACAPI_LibraryPart_OpenParameters(owner); }
inline GSErrCode Compat_GetActParameters(API_GetParamsType* params)
    { return ACAPI_LibraryPart_GetActParameters(params); }
inline GSErrCode Compat_CloseParameters()
    { return ACAPI_LibraryPart_CloseParameters(); }

// ── Progress window (formerly ACAPI_Interface / APIIo_*) ──────────────────
inline GSErrCode Compat_InitProcessWindow(const GS::UniString* title, Int32* nPhase)
    { return ACAPI_ProcessWindow_InitProcessWindow(title, nPhase); }
inline GSErrCode Compat_SetNextProcessPhase(const GS::UniString* subtitle, Int32* maxval)
    { return ACAPI_ProcessWindow_SetNextProcessPhase(subtitle, maxval); }
inline GSErrCode Compat_SetProcessValue(Int32* val)
    { return ACAPI_ProcessWindow_SetProcessValue(val); }
inline GSErrCode Compat_CloseProcessWindow()
    { return ACAPI_ProcessWindow_CloseProcessWindow(); }
inline bool Compat_IsProcessCanceled()
    { return ACAPI_ProcessWindow_IsProcessCanceled() != NoError; }

#else  // AC26

// ── Library management ────────────────────────────────────────────────────
inline GSErrCode Compat_GetLibraries(GS::Array<API_LibraryInfo>* libs, Int32* embIdx)
    { return ACAPI_Environment(APIEnv_GetLibrariesID, libs, embIdx); }
inline GSErrCode Compat_SetLibraries(const GS::Array<API_LibraryInfo>* libs)
    { return ACAPI_Environment(APIEnv_SetLibrariesID, const_cast<GS::Array<API_LibraryInfo>*>(libs)); }
inline GSErrCode Compat_ReloadLibraries()
    { return ACAPI_Automate(APIDo_ReloadLibrariesID, nullptr, nullptr); }
inline GSErrCode Compat_GetStorySettings(API_StoryInfo* info)
    { return ACAPI_Environment(APIEnv_GetStorySettingsID, info, nullptr); }

// ── LibPart parameters ────────────────────────────────────────────────────
inline GSErrCode Compat_OpenParameters(API_ParamOwnerType* owner)
    { return ACAPI_Goodies(APIAny_OpenParametersID, owner, nullptr); }
inline GSErrCode Compat_GetActParameters(API_GetParamsType* params)
    { return ACAPI_Goodies(APIAny_GetActParametersID, params, nullptr); }
inline GSErrCode Compat_CloseParameters()
    { return ACAPI_Goodies(APIAny_CloseParametersID, nullptr, nullptr); }

// ── Progress window ───────────────────────────────────────────────────────
inline GSErrCode Compat_InitProcessWindow(const GS::UniString* title, Int32* nPhase)
    { return ACAPI_Interface(APIIo_InitProcessWindowID, const_cast<GS::UniString*>(title), nPhase); }
inline GSErrCode Compat_SetNextProcessPhase(const GS::UniString* subtitle, Int32* maxval)
    { return ACAPI_Interface(APIIo_SetNextProcessPhaseID, const_cast<GS::UniString*>(subtitle), maxval); }
inline GSErrCode Compat_SetProcessValue(Int32* val)
    { return ACAPI_Interface(APIIo_SetProcessValueID, val, nullptr); }
inline GSErrCode Compat_CloseProcessWindow()
    { return ACAPI_Interface(APIIo_CloseProcessWindowID, nullptr, nullptr); }
inline bool Compat_IsProcessCanceled()
    { bool stop = false; ACAPI_Interface(APIIo_IsProcessCanceledID, &stop, nullptr); return stop; }

#endif
