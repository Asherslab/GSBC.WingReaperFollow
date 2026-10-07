// Single include point for Win32/SWELL and the REAPER API. Only the functions listed here are
// imported (REAPERAPI_MINIMAL), so the extension loads on any REAPER that has them.
#pragma once

#ifdef _WIN32
#include <windows.h>  // not LEAN_AND_MEAN: WDL's win32_utf8.h needs shellapi/commdlg
#include <commctrl.h>
#else
#include "swell/swell.h"
#endif

#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_AddExtensionsMainMenu
#define REAPERAPI_WANT_CountSelectedTracks
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_CSurf_OnMuteChangeEx
#define REAPERAPI_WANT_CSurf_OnVolumeChangeEx
#define REAPERAPI_WANT_CSurf_SetSurfaceMute
#define REAPERAPI_WANT_CSurf_SetSurfaceVolume
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowRemove
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_EnumProjExtState
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_GetGlobalAutomationOverride
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetProjExtState
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackGUID
#define REAPERAPI_WANT_GetTrackAutomationMode
#define REAPERAPI_WANT_guidToString
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_MarkProjectDirty
#define REAPERAPI_WANT_RecursiveCreateDirectory
#define REAPERAPI_WANT_RefreshToolbar2
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_SetOnlyTrackSelected
#define REAPERAPI_WANT_SetProjExtState
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_GetFXGUID
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetNamedConfigParm
#define REAPERAPI_WANT_TrackFX_GetParam
#define REAPERAPI_WANT_TrackFX_SetParam
#define REAPERAPI_WANT_TrackList_AdjustWindows
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_ValidatePtr2

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

// SWELL (and windows.h without NOMINMAX) define min/max macros that break std::min/std::max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
