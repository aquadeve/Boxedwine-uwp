/*
 * Boxedwine Android Emulator - UWP / Xbox One Entry Point
 *
 * Universal Windows Platform entry point for the Android APK emulator.
 * Integrates the ARMv7 CPU interpreter, Android ELF linker, JNI stubs,
 * and Linux syscall layer to run Android native apps on UWP devices
 * including Xbox One and Xbox Series X|S.
 *
 * Architecture:
 *   APK file -> apk_loader -> android_linker (ELF/ARMv7)
 *                          -> android_jni (JNI stubs)
 *                          -> armv7_interpreter (CPU)
 *                          -> android_syscall (Linux syscalls)
 *
 * Xbox One notes:
 *   - Primary input is gamepad (no mouse/keyboard by default)
 *   - SDL_GameController API handles Xbox controller mapping
 *   - Left stick → virtual D-pad / pointer, A button → touch
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

// OpenGL ES 2.0 (via ANGLE on UWP — D3D11 backend)
#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>

// Android emulator core
#include "../../../../source/android/android_emulator.h"
#include "../../../../source/android/angle_renderer.h"

// Capture the UI-thread dispatcher so that libuwp can later dispatch file-picker
// dialogs back to the correct thread from the SDL game thread.
extern "C" __declspec(dllimport) void uwp_CaptureUIDispatcher();

// Show a system file picker and write the selected path into buffer (max 256 chars).
extern "C" __declspec(dllimport) void uwp_PickAFile(char* buffer);

// -------------------------------------------------------------------------
// Helper: build a gamepad button bitmask from an SDL_GameController
// -------------------------------------------------------------------------
static uint32_t poll_gamepad_buttons(SDL_GameController *gc)
{
    uint32_t mask = 0;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))           mask |= AGAMEPAD_A;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))           mask |= AGAMEPAD_B;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))           mask |= AGAMEPAD_X;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))           mask |= AGAMEPAD_Y;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= AGAMEPAD_L1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= AGAMEPAD_R1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))        mask |= AGAMEPAD_SELECT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))       mask |= AGAMEPAD_START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))   mask |= AGAMEPAD_L3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))  mask |= AGAMEPAD_R3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))     mask |= AGAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))   mask |= AGAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))   mask |= AGAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))  mask |= AGAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE))       mask |= AGAMEPAD_GUIDE;
    return mask;
}

// -------------------------------------------------------------------------
// SDL_main - called by SDL after it has set up the WinRT environment.
// This is where we parse arguments and launch the Android emulator.
// -------------------------------------------------------------------------
extern "C" int SDL_main(int argc, char *argv[])
{
    // Capture the UI-thread CoreWindow dispatcher now that CoreApplication::Run()
    // has created the CoreWindow.  This must happen before any file-picker calls.
    uwp_CaptureUIDispatcher();

    // Default configuration
    AndroidEmulatorConfig config = {};
    config.screen_width  = 1280;
    config.screen_height = 720;
    config.verbosity     = 2;
    config.apk_path      = nullptr;
    config.main_lib      = nullptr;
    config.data_dir      = nullptr;

    // Parse command-line arguments
    // Accepts both --apk and -apk (single/double dash) for compatibility
    // with buildArgs() which uses single-dash format.
    // Usage: boxedwine-android [--apk <path>] [--width W] [--height H] [--lib <libname>] [--data <dir>]
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--apk") == 0 || strcmp(argv[i], "-apk") == 0) && i + 1 < argc) {
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

    // If no APK was provided (either via file activation or command-line args),
    // show a file picker so the user can select one.
    static char picked_path[256] = {};
    if (!config.apk_path) {
        uwp_PickAFile(picked_path);
        if (picked_path[0] != '\0' && strstr(picked_path, ".apk")) {
            config.apk_path = picked_path;
        }
    }

    if (!config.apk_path) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "No APK file specified.\n\n"
            "Usage: boxedwine-android --apk <path-to.apk>\n\n"
            "Open an .apk file from File Explorer, or pass\n"
            "the path as a command-line argument.",
            nullptr);
        return 1;
    }

    if (config.verbosity >= 1) {
        SDL_Log("Boxedwine Android Emulator starting...");
        SDL_Log("APK: %s", config.apk_path);
        SDL_Log("Screen: %dx%d", config.screen_width, config.screen_height);
    }

    // -------------------------------------------------------------------------
    // Xbox One / UWP hints: set before SDL_Init for correct behaviour
    // -------------------------------------------------------------------------
    // Enable Xbox controller support via HID API
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "1");
    // Ensure the virtual cursor is visible (useful on Xbox when no mouse)
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");

    // Initialise SDL for display, input, and game controllers
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // -------------------------------------------------------------------------
    // ANGLE / OpenGL ES 2.0 setup
    //
    // Request an OpenGL ES 2.0 context from SDL.  On UWP/Xbox, SDL uses ANGLE
    // which translates GLES2 → D3D11 under the hood.  This gives us a real
    // OpenGL ES context that works everywhere D3D11 is available.
    // -------------------------------------------------------------------------
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,     8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,    8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   0);   /* 2D only — no depth needed */
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow(
        "Boxedwine Android",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        config.screen_width, config.screen_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);

    if (!window) {
        SDL_Log("SDL_CreateWindow (OpenGL) failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        SDL_Log("SDL_GL_CreateContext failed: %s — ANGLE/D3D11 may not be available", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Enable vsync (1) for smooth presentation.  Set to 0 for uncapped. */
    SDL_GL_SetSwapInterval(1);

    if (config.verbosity >= 1) {
        SDL_Log("OpenGL ES renderer: %s", (const char*)glGetString(GL_RENDERER));
        SDL_Log("OpenGL ES version:  %s", (const char*)glGetString(GL_VERSION));
        SDL_Log("ANGLE D3D11 → OpenGL ES translation layer active");
    }

    /* Create the ANGLE/GLES2 framebuffer renderer */
    AngleRenderer *gl_renderer = angle_renderer_create(config.screen_width, config.screen_height);
    if (!gl_renderer) {
        SDL_Log("angle_renderer_create failed");
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // -------------------------------------------------------------------------
    // Open any game controllers already connected (Xbox One controller, etc.)
    // -------------------------------------------------------------------------
    SDL_GameController *gamepad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gamepad = SDL_GameControllerOpen(i);
            if (gamepad) {
                SDL_Log("Gamepad connected: %s", SDL_GameControllerName(gamepad));
                break; // Use the first recognised controller
            }
        }
    }
    // Virtual cursor position for gamepad pointer emulation (Xbox One)
    float virtual_cursor_x = config.screen_width  / 2.0f;
    float virtual_cursor_y = config.screen_height / 2.0f;

    // Initialise the Android emulator
    AndroidEmulator *emu = new AndroidEmulator{};
    if (!android_emulator_init(emu, &config)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "Failed to initialise Android emulator.\n"
            "Check that the APK contains ARMv7 native libraries.",
            window);
        angle_renderer_destroy(gl_renderer);
        SDL_GL_DeleteContext(gl_context);
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
    const float CURSOR_SPEED = 10.0f;
    const int16_t STICK_DEADZONE = 8000;

    while (!quit && emu->cpu.running) {
        // Process host OS events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    quit = true;
                    break;

                // --- Mouse input (PC / desktop UWP) ---
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    android_emulator_touch(emu,
                        event.type == SDL_MOUSEBUTTONDOWN ? 0 : 1,
                        event.button.x, event.button.y, 0);
                    break;

                // --- Touch input (Surface, tablets) ---
                case SDL_FINGERDOWN:
                case SDL_FINGERUP:
                case SDL_FINGERMOTION: {
                    int action = (event.type == SDL_FINGERDOWN) ? 0 :
                                 (event.type == SDL_FINGERUP)   ? 1 : 2;
                    int fx = (int)(event.tfinger.x * config.screen_width);
                    int fy = (int)(event.tfinger.y * config.screen_height);
                    android_emulator_touch(emu, action, fx, fy,
                                           (int)event.tfinger.fingerId);
                    break;
                }

                // --- Keyboard input ---
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    android_emulator_key(emu,
                        event.type == SDL_KEYDOWN ? 0 : 1,
                        event.key.keysym.sym);
                    break;

                // --- Gamepad hot-plug (Xbox One controllers) ---
                case SDL_CONTROLLERDEVICEADDED:
                    if (!gamepad) {
                        gamepad = SDL_GameControllerOpen(event.cdevice.which);
                        if (gamepad && config.verbosity >= 1)
                            SDL_Log("Gamepad connected: %s", SDL_GameControllerName(gamepad));
                    }
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (gamepad && event.cdevice.which == SDL_JoystickInstanceID(
                            SDL_GameControllerGetJoystick(gamepad))) {
                        SDL_Log("Gamepad disconnected");
                        SDL_GameControllerClose(gamepad);
                        gamepad = nullptr;
                    }
                    break;

                // --- Window resize ---
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        config.screen_width  = event.window.data1;
                        config.screen_height = event.window.data2;
                    }
                    break;
            }
        }

        // -------------------------------------------------------------------------
        // Gamepad polling — Xbox One is the primary input on console
        // -------------------------------------------------------------------------
        if (gamepad) {
            uint32_t buttons = poll_gamepad_buttons(gamepad);
            int16_t lx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t ly = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
            int16_t rx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
            int16_t ry = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
            int16_t lt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            int16_t rt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

            // Send structured gamepad event
            android_emulator_gamepad(emu, buttons, lx, ly, rx, ry, lt, rt);

            // ---- Virtual cursor via right stick (for touch emulation on Xbox) ----
            if (rx > STICK_DEADZONE || rx < -STICK_DEADZONE)
                virtual_cursor_x += (float)rx / 32767.0f * CURSOR_SPEED;
            if (ry > STICK_DEADZONE || ry < -STICK_DEADZONE)
                virtual_cursor_y += (float)ry / 32767.0f * CURSOR_SPEED;

            // Clamp cursor to screen
            if (virtual_cursor_x < 0) virtual_cursor_x = 0;
            if (virtual_cursor_y < 0) virtual_cursor_y = 0;
            if (virtual_cursor_x >= config.screen_width)  virtual_cursor_x = (float)(config.screen_width - 1);
            if (virtual_cursor_y >= config.screen_height) virtual_cursor_y = (float)(config.screen_height - 1);

            // A button = touch press at virtual cursor position
            static bool a_was_pressed = false;
            bool a_pressed = (buttons & AGAMEPAD_A) != 0;
            if (a_pressed && !a_was_pressed)
                android_emulator_touch(emu, 0, (int)virtual_cursor_x, (int)virtual_cursor_y, 0);
            if (!a_pressed && a_was_pressed)
                android_emulator_touch(emu, 1, (int)virtual_cursor_x, (int)virtual_cursor_y, 0);
            a_was_pressed = a_pressed;
        }

        // Execute emulated CPU instructions for this frame
        if (emu->cpu.running) {
            android_emulator_step(emu, CPU_STEPS_PER_FRAME);
        }

        // ---- ANGLE/GLES2 rendering ----
        // Upload the emulator's software framebuffer (when it has one).
        // TODO: Replace NULL with the actual framebuffer pointer once the
        // emulator produces pixel output (e.g. via ANativeWindow_Buffer or
        // guest eglSwapBuffers).
        angle_renderer_upload(gl_renderer, NULL);

        // Draw the emulator framebuffer as a fullscreen quad via GLES2
        angle_renderer_draw(gl_renderer, config.screen_width, config.screen_height);

        // Draw virtual cursor crosshair when gamepad is active (for Xbox)
        if (gamepad) {
            angle_renderer_draw_cursor(gl_renderer, virtual_cursor_x, virtual_cursor_y,
                                       config.screen_width, config.screen_height);
        }

        // Present via ANGLE → D3D11 swap chain
        SDL_GL_SwapWindow(window);
    }

    if (config.verbosity >= 1)
        SDL_Log("Android emulator exited with code %d", emu->exit_code);

    int exit_code = emu->exit_code;
    android_emulator_destroy(emu);
    if (gamepad) SDL_GameControllerClose(gamepad);
    angle_renderer_destroy(gl_renderer);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}

// Entry point into UWP / Xbox One app
int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR argv, int argc)
{
    // NOTE: uwp_CaptureUIDispatcher() is called from SDL_main(), not here.
    // At WinMain time, CoreApplication::Run() has not yet created a CoreWindow,
    // so CoreWindow::GetForCurrentThread() would return nullptr and crash.
    // By the time SDL_main() runs, the CoreWindow is fully initialised.

    return SDL_WinRTRunApp(SDL_main, NULL);
}
