#pragma once

#define LIBAPI extern "C" __declspec(dllexport)

// :: Screen information
LIBAPI void  uwp_GetScreenSize(int* x, int* y);
LIBAPI float uwp_GetRefreshRate();
LIBAPI void* uwp_GetWindowReference();

// :: Filepaths
LIBAPI void uwp_GetBundlePath(char* buffer);
LIBAPI void uwp_GetBundleFilePath(char* buffer, const char* filename);
LIBAPI void uwp_GetLocalDirectory(char* buffer);
LIBAPI void uwp_PickAFile(char* buffer);
LIBAPI void uwp_PickAFolder(char* buffer);

// :: Events

// If not using SDL or other helper you must occasionally call this to get anything to show on screen
LIBAPI void uwp_ProcessEvents();

// If not using SDL or other helper you must register event callbacks to read controller input
LIBAPI void uwp_RegisterGamepadCallbacks(void (*callback)(void));

// :: Threading helpers

// Call this once from the UI thread after CoreApplication::Run() has created
// the CoreWindow (e.g. at the start of SDL_main) so that file-picker dialogs
// can be dispatched back to the correct thread.  Do NOT call from WinMain —
// at that point no CoreWindow exists yet and the call would crash.
LIBAPI void uwp_CaptureUIDispatcher();
