/*
 * uwpLauncher.cpp
 *
 * UWP-specific launch-mode selector UI (compiled only when BOXEDWINE_UWP is defined).
 *
 * Registers itself as the first runOnMainUI() callback so that the mode-selector
 * overlay is shown before the Wine container browser appears.
 *
 * Supported modes
 *   LAUNCH_MODE_WINE   – original behaviour: show the Wine app launcher UI
 *   LAUNCH_MODE_APKENV – pick an Android APK via the UWP file-picker then launch
 *                        it through apkenv inside the emulated Linux kernel
 *   LAUNCH_MODE_BASH   – launch /bin/bash for debugging and manual Linux commands
 */

#ifdef BOXEDWINE_UWP

#include "boxedwine.h"
#include "startupArgs.h"
#include "uwpLauncher.h"
#include "../ui/boxedwineui.h"
#include "../ui/data/globalSettings.h"
#include "../ui/mainui.h"

// libuwp file-picker (exported from libuwp.dll)
extern "C" __declspec(dllimport) void uwp_PickAFile(char* buffer);

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static bool   g_modeSelected  = false;
static int    g_selectedMode   = LAUNCH_MODE_WINE;
// Buffer for the picked APK path.  uwp_PickAFile() internally uses sprintf_s
// with a 256-byte limit, so 512 bytes is more than sufficient.
static char   g_apkPath[512]   = "";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void applySelection() {
    StartUpArgs& args = GlobalSettings::startUpArgs;
    args.launchMode = g_selectedMode;

    if (g_selectedMode == LAUNCH_MODE_APKENV) {
        if (g_apkPath[0] != '\0') {
            args.apkPath = BString::copy(g_apkPath);
        }
        // Signal the UI loop to stop and hand control to BoxedwineData::startApp()
        args.readyToLaunch = true;
    } else if (g_selectedMode == LAUNCH_MODE_BASH) {
        // Signal ready – BoxedwineData::startApp() will call apply() which
        // defaults to /bin/bash when launchMode == LAUNCH_MODE_BASH.
        args.readyToLaunch = true;
    }
    // For LAUNCH_MODE_WINE we simply allow the normal Wine UI to take over.
}

// ---------------------------------------------------------------------------
// Per-frame render function (returned to runOnMainUI while the overlay is open)
// ---------------------------------------------------------------------------

static bool renderModeSelector() {
    if (g_modeSelected) {
        return false; // done
    }

    ImGuiIO& io = ImGui::GetIO();

    // Full-screen opaque overlay drawn on top of the main window
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::SetNextWindowFocus();

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##uwp_launcher_overlay", nullptr, wf);

    // ----- Title -----
    ImGui::PushFont(GlobalSettings::largeFontBold ? GlobalSettings::largeFontBold : nullptr);
    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Boxedwine UWP");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Select a launch mode:");
    ImGui::Spacing();
    ImGui::Spacing();

    float btnW  = GlobalSettings::scaleFloatUI(340.0f);
    float btnH  = GlobalSettings::scaleFloatUI(52.0f);
    float padX  = (io.DisplaySize.x - btnW) * 0.5f;
    if (padX < 8.0f) padX = 8.0f;

    // ----- Wine button -----
    ImGui::SetCursorPosX(padX);
    if (ImGui::Button("Run Windows App  (Wine)", ImVec2(btnW, btnH))) {
        g_selectedMode = LAUNCH_MODE_WINE;
        g_modeSelected = true;
        applySelection();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Original Boxedwine behaviour.\nRun 32-bit Windows applications via Wine.");
    }

    ImGui::Spacing();

    // ----- apkenv button -----
    ImGui::SetCursorPosX(padX);
    if (ImGui::Button("Run Android App  (apkenv)", ImVec2(btnW, btnH))) {
        // Open UWP file-picker to select an APK.
        // uwp_PickAFile() blocks this thread until the picker is dismissed,
        // then continues on the SDL game thread.
        g_apkPath[0] = '\0';
        uwp_PickAFile(g_apkPath);
        if (g_apkPath[0] != '\0') {
            g_selectedMode = LAUNCH_MODE_APKENV;
            g_modeSelected = true;
            applySelection();
        }
        // If the user cancelled the picker we stay in the overlay.
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Run an Android APK through apkenv.\nSelect an .apk file using the file picker.");
    }

    // Show the currently selected APK path (if any)
    if (g_apkPath[0] != '\0') {
        ImGui::Spacing();
        ImGui::SetCursorPosX(padX);
        ImGui::TextWrapped("APK: %s", g_apkPath);
    }

    ImGui::Spacing();

    // ----- Bash terminal button -----
    ImGui::SetCursorPosX(padX);
    if (ImGui::Button("Open Bash Terminal  (debug)", ImVec2(btnW, btnH))) {
        g_selectedMode = LAUNCH_MODE_BASH;
        g_modeSelected = true;
        applySelection();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Launch an interactive /bin/bash shell.\nUseful for running Linux commands and debugging.");
    }

    ImGui::End();

    // Keep running until done
    return !g_modeSelected;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void uwpScheduleModeSelectorUI() {
    g_modeSelected = false;
    g_selectedMode = LAUNCH_MODE_WINE;
    g_apkPath[0]   = '\0';

    // Queue the mode-selector to be rendered on every ImGui frame until the
    // user makes a selection.  runOnMainUI() accepts a bool() lambda: returning
    // true keeps it alive, returning false removes it from the queue.
    runOnMainUI([]() -> bool {
        return renderModeSelector();
    });
}

#endif // BOXEDWINE_UWP
