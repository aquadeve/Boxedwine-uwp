/*
 * Boxedwine Android Emulator - UWP Entry Point
 *
 * Universal Windows Platform entry point for the Android APK emulator.
 * Integrates the ARMv7 CPU interpreter, Android ELF linker, JNI stubs,
 * and Linux syscall layer to run Android native apps on UWP devices.
 *
 * Architecture:
 *   APK file -> apk_loader -> android_linker (ELF/ARMv7)
 *                          -> android_jni (JNI stubs)
 *                          -> armv7_interpreter (CPU)
 *                          -> android_syscall (Linux syscalls)
 *
 * Reference code:
 *   referenceCode/apkenv/    (Thomas Perl's apkenv)
 *   referenceCode/Bridge/    (FLinux UWP Android bridge)
 */

#include "SDL2/SDL.h"
#include <wrl.h>
#include <string>
#include <sstream>

#ifdef _MSC_VER
#pragma warning(disable : 4447)
#pragma comment(lib, "runtimeobject.lib")
#endif

// Android emulator core
#include "../../../../source/android/android_emulator.h"

// Capture the UI-thread dispatcher so that libuwp can later dispatch file-picker
// dialogs back to the correct thread from the SDL game thread.
extern "C" __declspec(dllimport) void uwp_CaptureUIDispatcher();

// -------------------------------------------------------------------------
// SDL_main - called by SDL after it has set up the WinRT environment.
// This is where we parse arguments and launch the Android emulator.
// -------------------------------------------------------------------------
extern "C" int SDL_main(int argc, char *argv[])
{
    // Default configuration
    AndroidEmulatorConfig config = {};
    config.screen_width  = 1280;
    config.screen_height = 720;
    config.verbosity     = 2;
    config.apk_path      = nullptr;
    config.main_lib      = nullptr;
    config.data_dir      = nullptr;

    // Parse command-line arguments
    // Usage: boxedwine-android [--apk <path>] [--width W] [--height H] [--lib <libname>] [--data <dir>]
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apk") == 0 && i + 1 < argc) {
            config.apk_path = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            config.screen_width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            config.screen_height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            config.main_lib = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            config.data_dir = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            config.verbosity = 3;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            config.verbosity = 0;
        } else if (i + 1 == argc && strstr(argv[i], ".apk")) {
            // Bare argument ending in .apk treated as the APK path
            config.apk_path = argv[i];
        }
    }

    if (!config.apk_path) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "No APK file specified.\n\n"
            "Usage: boxedwine-android --apk <path-to.apk>\n\n"
            "Drag and drop an APK file onto the application, or pass\n"
            "the path as a command-line argument.",
            nullptr);
        return 1;
    }

    if (config.verbosity >= 1) {
        SDL_Log("Boxedwine Android Emulator starting...");
        SDL_Log("APK: %s", config.apk_path);
        SDL_Log("Screen: %dx%d", config.screen_width, config.screen_height);
    }

    // Initialise SDL for display and input
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Boxedwine Android",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        config.screen_width, config.screen_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialise the Android emulator
    AndroidEmulator emu = {};
    if (!android_emulator_init(&emu, &config)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "Failed to initialise Android emulator.\n"
            "Check that the APK contains ARMv7 native libraries.",
            window);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (config.verbosity >= 1)
        SDL_Log("Android emulator initialised successfully");

    // -------------------------------------------------------------------------
    // Main loop: pump SDL events + step the ARMv7 CPU interpreter
    // -------------------------------------------------------------------------
    bool quit = false;
    const unsigned CPU_STEPS_PER_FRAME = 1000000; // ~1M instructions per frame

    while (!quit && emu.cpu.running) {
        // Process host OS events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    quit = true;
                    break;

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    android_emulator_touch(&emu,
                        event.type == SDL_MOUSEBUTTONDOWN ? 0 : 1,
                        event.button.x, event.button.y, 0);
                    break;

                case SDL_FINGERDOWN:
                case SDL_FINGERUP:
                case SDL_FINGERMOTION: {
                    int action = (event.type == SDL_FINGERDOWN) ? 0 :
                                 (event.type == SDL_FINGERUP)   ? 1 : 2;
                    int fx = (int)(event.tfinger.x * config.screen_width);
                    int fy = (int)(event.tfinger.y * config.screen_height);
                    android_emulator_touch(&emu, action, fx, fy,
                                           (int)event.tfinger.fingerId);
                    break;
                }

                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    android_emulator_key(&emu,
                        event.type == SDL_KEYDOWN ? 0 : 1,
                        event.key.keysym.sym);
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        config.screen_width  = event.window.data1;
                        config.screen_height = event.window.data2;
                    }
                    break;
            }
        }

        // Execute emulated CPU instructions for this frame
        if (emu.cpu.running) {
            android_emulator_step(&emu, CPU_STEPS_PER_FRAME);
        }

        // Render a placeholder frame (the framebuffer would normally be
        // mapped from emulated memory; here we just present a clear frame)
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }

    if (config.verbosity >= 1)
        SDL_Log("Android emulator exited with code %d", emu.exit_code);

    int exit_code = emu.exit_code;
    android_emulator_destroy(&emu);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}

// Entry point into UWP app
int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR argv, int argc)
{
    // Capture the UI-thread CoreWindow dispatcher before SDL moves execution to a
    // background thread.  This allows libuwp to show file-picker dialogs correctly.
    uwp_CaptureUIDispatcher();

    return SDL_WinRTRunApp(SDL_main, NULL);
}
