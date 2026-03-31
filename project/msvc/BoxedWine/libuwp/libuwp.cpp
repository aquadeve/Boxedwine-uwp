/*
    Various helpers for UWP apps, add a new function if you need to interop between a dll and UWP calls
*/
#include "pch.h"
#include "libuwp.h"

#include <functional>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Foundation.h>
#include "winrt/Windows.Foundation.Collections.h"
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Pickers.Provider.h>
#include <winrt/Windows.UI.ViewManagement.h>


static int width = 0;
static int height = 0;
static winrt::Windows::UI::Core::CoreDispatcher g_uiDispatcher{ nullptr };

using namespace winrt::Windows;
using namespace ApplicationModel::Core;
using namespace Foundation;
using namespace Graphics::Display::Core;
using namespace Storage;
using namespace Storage::AccessCache;
using namespace UI::Core;
using namespace UI::ViewManagement;


void uwp_CaptureUIDispatcher()
{
    // Must be called from the UI thread after CoreApplication::Run() has created
    // the CoreWindow (e.g. at the start of SDL_main).  Calling earlier — such as
    // from WinMain — will crash because no CoreWindow exists yet.
    auto coreWindow = CoreWindow::GetForCurrentThread();
    if (coreWindow) {
        g_uiDispatcher = coreWindow.Dispatcher();
    }
}

void uwp_GetBundlePath(char* buffer)
{
    sprintf_s(buffer, 256, "%s", winrt::to_string(ApplicationModel::Package::Current().InstalledPath()).c_str());
}

void uwp_GetBundleFilePath(char* buffer, const char *filename)
{
    sprintf_s(buffer, 256, "%s\\%s", winrt::to_string(ApplicationModel::Package::Current().InstalledPath()).c_str(), filename);
}

void uwp_GetLocalDirectory(char* buffer)
{
    auto localFolder = ApplicationData::Current().LocalFolder();
    std::string path = winrt::to_string(localFolder.Path());
    sprintf_s(buffer, 256, "%s\\", path.c_str());
}

// Helper: dispatch a work item to the UI thread and block the calling thread until
// the work item signals the provided HANDLE.
static void runOnUIThread(std::function<void(HANDLE)> work)
{
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        // Failed to create synchronisation event; run inline as fallback.
        HANDLE dummy = nullptr;
        work(dummy);
        return;
    }

    if (g_uiDispatcher) {
        g_uiDispatcher.RunAsync(CoreDispatcherPriority::Normal, [work, hEvent]() {
            work(hEvent);
        });
    } else {
        // No dispatcher captured yet; try to run inline (works when already on UI thread).
        work(hEvent);
    }

    WaitForSingleObject(hEvent, INFINITE);
    CloseHandle(hEvent);
}

// TODO: Restrict types?
void uwp_PickAFile(char* buffer)
{
    std::wstring selected;

    runOnUIThread([&selected](HANDLE hDone) {
        Pickers::FileOpenPicker filePicker;
        filePicker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
        filePicker.FileTypeFilter().ReplaceAll({ L"*" });

        filePicker.PickSingleFileAsync().Completed(
            [&selected, hDone](auto op, auto status) {
                if (status == AsyncStatus::Completed) {
                    auto file = op.GetResults();
                    if (file) {
                        selected = std::wstring(file.Path().c_str());
                    }
                }
                if (hDone) SetEvent(hDone);
            });
    });

    std::string pathStr(selected.begin(), selected.end());
    sprintf_s(buffer, 256, "%s", pathStr.c_str());
}

// Inspired by aerisarns impl in the gzdoom port
void uwp_PickAFolder(char* buffer)
{
    std::wstring selected;

    runOnUIThread([&selected](HANDLE hDone) {
        Pickers::FolderPicker folderPicker;
        folderPicker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
        folderPicker.FileTypeFilter().ReplaceAll({ L"*" });

        folderPicker.PickSingleFolderAsync().Completed(
            [&selected, hDone](auto op, auto status) {
                if (status == AsyncStatus::Completed) {
                    auto folder = op.GetResults();
                    if (folder) {
                        // Application now has read/write access to all contents in the picked folder
                        StorageApplicationPermissions::FutureAccessList().AddOrReplace(L"PickedFolderToken", folder);
                        selected = std::wstring(folder.Path().c_str());
                    }
                }
                if (hDone) SetEvent(hDone);
            });
    });

    std::string pathStr(selected.begin(), selected.end());
    sprintf_s(buffer, 256, "%s", pathStr.c_str());
}


void uwp_GetScreenSize(int* x, int* y)
{
    if (width == 0) {
        HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
        width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
        height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
    }

    *x = width;
    *y = height;
}

float uwp_GetRefreshRate()
{
    return static_cast<float>(HdmiDisplayInformation::GetForCurrentView().GetCurrentDisplayMode().RefreshRate());
}

void* uwp_GetWindowReference()
{
    return reinterpret_cast<void*>(winrt::get_abi(CoreWindow::GetForCurrentThread()));
}

void uwp_ProcessEvents()
{
    CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
}

// Register gamepad event hooks to method inside of calling program
void uwp_RegisterGamepadCallbacks(void (*callback)(void))
{
    namespace WGI = winrt::Windows::Gaming::Input;

    try
    {
        WGI::Gamepad::GamepadAdded([callback](auto&&, const WGI::Gamepad) {
            callback();
        });

        WGI::Gamepad::GamepadRemoved([callback](auto&&, const WGI::Gamepad) {
            callback();
        });
    }
    catch (winrt::hresult_error)
    {
    }

}
