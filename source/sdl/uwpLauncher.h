#pragma once

/*
 * uwpLauncher.h
 *
 * UWP-specific launch-mode selector.
 *
 * On UWP builds (BOXEDWINE_UWP) this provides a full-screen ImGui overlay
 * that lets the user choose between:
 *   - Wine  (Windows app emulation, original Boxedwine behaviour)
 *   - apkenv (Android APK emulation via apkenv)
 *   - Bash  (interactive /bin/bash terminal for debugging)
 *
 * Call uwpScheduleModeSelectorUI() once from boxedmain() **after**
 * KNativeSystem::init() has been called so that SDL/ImGui are ready.
 * The selection is applied to GlobalSettings::startUpArgs before the normal
 * UI loop hands control to BoxedwineData::startApp().
 */

#ifdef BOXEDWINE_UWP

// Schedule the mode-selector overlay to appear on the next ImGui frame.
// Must be called after KNativeSystem::init() and BoxedwineData::init().
void uwpScheduleModeSelectorUI();

#endif // BOXEDWINE_UWP
