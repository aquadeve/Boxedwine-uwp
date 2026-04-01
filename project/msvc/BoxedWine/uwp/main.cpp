/*
 * Boxedwine Android Emulator - UWP / Xbox One Entry Point
 */

#include "SDL2/SDL.h"

#include <windows.h>
#include <wrl.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>

#ifdef _MSC_VER
#pragma warning(disable : 4447)
#pragma comment(lib, "runtimeobject.lib")
#endif

#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>

 // Android emulator core
#include "../../../../source/android/android_emulator.h"
#include "../../../../source/android/angle_renderer.h"

// Capture the UI-thread dispatcher so libuwp can dispatch file-pickers back
// to the correct thread from the SDL game thread.
extern "C" __declspec(dllimport) void uwp_CaptureUIDispatcher();
extern "C" __declspec(dllimport) void uwp_PickAFile(char* buffer);
extern "C" __declspec(dllimport) bool uwp_CopyFileToLocal(const char* source_path, char* dest_buffer);

// -------------------------------------------------------------------------
// SDL hints must be set before SDL_Init()
// -------------------------------------------------------------------------
static void configure_sdl_hints_for_uwp()
{
#ifdef SDL_HINT_OPENGL_ES_DRIVER
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif

#ifdef SDL_HINT_MOUSE_TOUCH_EVENTS
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif

#ifdef SDL_HINT_JOYSTICK_HIDAPI_XBOX
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
#endif

#ifdef SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "1");
#endif

#ifdef SDL_HINT_VIDEO_WIN_D3DCOMPILER
    SDL_SetHint(SDL_HINT_VIDEO_WIN_D3DCOMPILER, "d3dcompiler_47.dll");
#endif
}

// -------------------------------------------------------------------------
// Helper: build a gamepad button bitmask from an SDL_GameController
// -------------------------------------------------------------------------
static uint32_t poll_gamepad_buttons(SDL_GameController* gc)
{
    uint32_t mask = 0;

    if (!gc) {
        return 0;
    }

    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             mask |= AGAMEPAD_A;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             mask |= AGAMEPAD_B;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             mask |= AGAMEPAD_X;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             mask |= AGAMEPAD_Y;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= AGAMEPAD_L1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= AGAMEPAD_R1;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          mask |= AGAMEPAD_SELECT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         mask |= AGAMEPAD_START;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     mask |= AGAMEPAD_L3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    mask |= AGAMEPAD_R3;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       mask |= AGAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     mask |= AGAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     mask |= AGAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    mask |= AGAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE))         mask |= AGAMEPAD_GUIDE;

    return mask;
}

// -------------------------------------------------------------------------
// SDL_main - called by SDL after the WinRT environment is set up
// -------------------------------------------------------------------------
extern "C" int SDL_main(int argc, char* argv[])
{
    uwp_CaptureUIDispatcher();

    AndroidEmulatorConfig config = {};
    config.screen_width = 1280;
    config.screen_height = 720;
#if defined(_DEBUG)
    config.verbosity = 3;  /* Maximum verbosity in debug builds */
#else
    config.verbosity = 2;
#endif
    config.apk_path = nullptr;
    config.main_lib = nullptr;
    config.data_dir = nullptr;

    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--apk") == 0 || std::strcmp(argv[i], "-apk") == 0) && i + 1 < argc) {
            config.apk_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            config.screen_width = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            config.screen_height = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            config.main_lib = argv[++i];
        }
        else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            config.data_dir = argv[++i];
        }
        else if (std::strcmp(argv[i], "--verbose") == 0) {
            config.verbosity = 3;
        }
        else if (std::strcmp(argv[i], "--quiet") == 0) {
            config.verbosity = 0;
        }
        else if (i + 1 == argc && std::strstr(argv[i], ".apk")) {
            config.apk_path = argv[i];
        }
    }

    static char picked_path[256] = {};
    if (!config.apk_path) {
        uwp_PickAFile(picked_path);
        if (picked_path[0] != '\0' && std::strstr(picked_path, ".apk")) {
            config.apk_path = picked_path;
        }
    }

    // For APK paths that came from file activation (argv) rather than the
    // file picker, copy the file into LocalFolder so that fopen() works
    // under the UWP sandbox.  uwp_PickAFile already does this internally.
    static char local_apk_path[256] = {};
    if (config.apk_path && config.apk_path != picked_path) {
        if (uwp_CopyFileToLocal(config.apk_path, local_apk_path)) {
            config.apk_path = local_apk_path;
        }
        // If the copy fails (e.g. broadFileSystemAccess not granted),
        // keep the original path — fopen may still work if the path
        // is already inside the sandbox.
    }

    if (!config.apk_path) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "No APK file specified.\n\n"
            "Usage: boxedwine-android --apk <path-to.apk>\n\n"
            "Open an .apk file from File Explorer, or pass the path "
            "as a command-line argument.",
            nullptr
        );
        return 1;
    }

    if (config.verbosity >= 1) {
        SDL_Log("Boxedwine Android Emulator starting...");
        SDL_Log("APK: %s", config.apk_path);
        SDL_Log("Screen: %dx%d", config.screen_width, config.screen_height);
    }

    configure_sdl_hints_for_uwp();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "Boxedwine Android",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        config.screen_width, config.screen_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    if (config.verbosity >= 1) {
        SDL_Log("OpenGL ES renderer: %s", (const char*)glGetString(GL_RENDERER));
        SDL_Log("OpenGL ES version: %s", (const char*)glGetString(GL_VERSION));
    }

    AngleRenderer* gl_renderer = angle_renderer_create(config.screen_width, config.screen_height);
    if (!gl_renderer) {
        SDL_Log("angle_renderer_create failed");
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GameController* gamepad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gamepad = SDL_GameControllerOpen(i);
            if (gamepad) {
                const char* name = SDL_GameControllerName(gamepad);
                SDL_Log("Gamepad connected: %s", name ? name : "(unknown)");
                break;
            }
        }
    }

    float virtual_cursor_x = config.screen_width / 2.0f;
    float virtual_cursor_y = config.screen_height / 2.0f;

    AndroidEmulator* emu = new AndroidEmulator{};
#if defined(_DEBUG)
    SDL_Log("[EMU DEBUG] main: calling android_emulator_init (apk='%s')", config.apk_path);
#endif
    if (!android_emulator_init(emu, &config)) {
#if defined(_DEBUG)
        SDL_Log("[EMU DEBUG] main: android_emulator_init FAILED");
#endif
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "Boxedwine Android",
            "Failed to initialise Android emulator.\n"
            "Check that the APK contains ARMv7/ARM64 native libraries.",
            window
        );
        angle_renderer_destroy(gl_renderer);
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (config.verbosity >= 1) {
        SDL_Log("Android emulator initialised successfully");
    }

    bool quit = false;
    const unsigned CPU_STEPS_PER_FRAME = 1000000;
    const float CURSOR_SPEED = 10.0f;
    const int16_t STICK_DEADZONE = 8000;

    while (!quit && emu->cpu.running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                quit = true;
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                android_emulator_touch(
                    emu,
                    event.type == SDL_MOUSEBUTTONDOWN ? 0 : 1,
                    event.button.x,
                    event.button.y,
                    0
                );
                break;

            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            case SDL_FINGERMOTION: {
                int action = (event.type == SDL_FINGERDOWN) ? 0 :
                    (event.type == SDL_FINGERUP) ? 1 : 2;
                int fx = (int)(event.tfinger.x * config.screen_width);
                int fy = (int)(event.tfinger.y * config.screen_height);
                android_emulator_touch(emu, action, fx, fy, (int)event.tfinger.fingerId);
                break;
            }

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                android_emulator_key(
                    emu,
                    event.type == SDL_KEYDOWN ? 0 : 1,
                    event.key.keysym.sym
                );
                break;

            case SDL_CONTROLLERDEVICEADDED:
                if (!gamepad) {
                    gamepad = SDL_GameControllerOpen(event.cdevice.which);
                    if (gamepad && config.verbosity >= 1) {
                        const char* name = SDL_GameControllerName(gamepad);
                        SDL_Log("Gamepad connected: %s", name ? name : "(unknown)");
                    }
                }
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                if (gamepad &&
                    event.cdevice.which ==
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))) {
                    SDL_Log("Gamepad disconnected");
                    SDL_GameControllerClose(gamepad);
                    gamepad = nullptr;
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    config.screen_width = event.window.data1;
                    config.screen_height = event.window.data2;
                }
                break;
            }
        }

        if (gamepad) {
            uint32_t buttons = poll_gamepad_buttons(gamepad);
            int16_t lx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t ly = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
            int16_t rx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
            int16_t ry = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
            int16_t lt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            int16_t rt = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

            android_emulator_gamepad(emu, buttons, lx, ly, rx, ry, lt, rt);

            if (rx > STICK_DEADZONE || rx < -STICK_DEADZONE) {
                virtual_cursor_x += (float)rx / 32767.0f * CURSOR_SPEED;
            }
            if (ry > STICK_DEADZONE || ry < -STICK_DEADZONE) {
                virtual_cursor_y += (float)ry / 32767.0f * CURSOR_SPEED;
            }

            if (virtual_cursor_x < 0) virtual_cursor_x = 0;
            if (virtual_cursor_y < 0) virtual_cursor_y = 0;
            if (virtual_cursor_x >= config.screen_width)  virtual_cursor_x = (float)(config.screen_width - 1);
            if (virtual_cursor_y >= config.screen_height) virtual_cursor_y = (float)(config.screen_height - 1);

            static bool a_was_pressed = false;
            bool a_pressed = (buttons & AGAMEPAD_A) != 0;
            if (a_pressed && !a_was_pressed) {
                android_emulator_touch(emu, 0, (int)virtual_cursor_x, (int)virtual_cursor_y, 0);
            }
            if (!a_pressed && a_was_pressed) {
                android_emulator_touch(emu, 1, (int)virtual_cursor_x, (int)virtual_cursor_y, 0);
            }
            a_was_pressed = a_pressed;
        }

        if (emu->cpu.running) {
            android_emulator_step(emu, CPU_STEPS_PER_FRAME);
        }

        angle_renderer_upload(gl_renderer, NULL);
        angle_renderer_draw(gl_renderer, config.screen_width, config.screen_height);

        if (gamepad) {
            angle_renderer_draw_cursor(
                gl_renderer,
                virtual_cursor_x,
                virtual_cursor_y,
                config.screen_width,
                config.screen_height
            );
        }

        SDL_GL_SwapWindow(window);
    }

    if (config.verbosity >= 1) {
        SDL_Log("Android emulator exited with code %d", emu->exit_code);
    }

    int exit_code = emu->exit_code;

    android_emulator_destroy(emu);
    if (gamepad) {
        SDL_GameControllerClose(gamepad);
    }
    angle_renderer_destroy(gl_renderer);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return exit_code;
}

// Entry point into UWP / Xbox One app
int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return SDL_WinRTRunApp(SDL_main, NULL);
}