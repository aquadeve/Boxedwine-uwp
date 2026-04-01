/*
 * Boxedwine Android Emulator
 *
 * Top-level orchestration layer that:
 *   1. Loads an APK file
 *   2. Maps native ARMv7 or AArch64 shared libraries into emulated memory
 *   3. Resolves dynamic symbols and applies relocations
 *   4. Initialises the JNI environment
 *   5. Calls JNI_OnLoad on each library
 *   6. Calls the application's ANativeActivity_onCreate or nativeStart entry point
 *   7. Runs the ARMv7 or AArch64 CPU interpreter main loop
 *   8. Routes Linux syscalls to the Android syscall layer
 *
 * Targets Universal Windows Platform (UWP) exclusively.
 *
 * Reference code:
 *   referenceCode/apkenv/       (Thomas Perl's apkenv project)
 *   referenceCode/Bridge/       (FLinux / UWP Android bridge)
 */

#ifndef __ANDROID_EMULATOR_H__
#define __ANDROID_EMULATOR_H__

#include <stdint.h>
#include <stdbool.h>

#include "apk_loader.h"
#include "android_linker.h"
#include "android_jni.h"
#include "android_syscall.h"
#include "../emulation/cpu/arm/armv7_interpreter.h"
#include "../emulation/cpu/arm/aarch64_interpreter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration passed at startup
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Path to the APK file to run */
    const char *apk_path;

    /* Screen dimensions */
    int screen_width;
    int screen_height;

    /* Optional: specific library to load (NULL = use libmain.so or first .so) */
    const char *main_lib;

    /* Optional: data directory for the app (NULL = use "AndroidData/") */
    const char *data_dir;

    /* Verbosity level (0 = silent, 1 = errors, 2 = info, 3 = debug) */
    int verbosity;
} AndroidEmulatorConfig;

/* -------------------------------------------------------------------------
 * Emulator state
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Configuration */
    AndroidEmulatorConfig config;

    /* APK descriptor */
    ApkDescriptor apk;

    /* Flat emulated address space */
    uint8_t  *mem;
    uint32_t  mem_size;

    /* Dynamic linker */
    LinkerContext linker;

    /* JNI environment */
    AndroidJniContext jni;

    /* Syscall state */
    AndroidSyscallState syscall_state;

    /* Whether running 64-bit (AArch64) or 32-bit (ARMv7) code */
    bool is_arm64;

    /* ARMv7 CPU state (used when is_arm64 == false) */
    ArmV7State cpu;

    /* AArch64 CPU state (used when is_arm64 == true) */
    AArch64State cpu64;

    /* Whether the emulator has been initialised */
    bool initialised;

    /* Running state */
    bool running;
    int  exit_code;
} AndroidEmulator;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialise the emulator with the given configuration.
 * Allocates memory, loads the APK, maps libraries, and resolves symbols.
 * Returns true on success.
 */
bool android_emulator_init(AndroidEmulator *emu, const AndroidEmulatorConfig *config);

/**
 * Run the emulator (calls the ARMv7 CPU main loop).
 * Blocks until the emulated app calls exit() or returns from its main function.
 * Returns the exit code.
 */
int android_emulator_run(AndroidEmulator *emu);

/**
 * Pump one frame of the emulator (for integration with a host event loop).
 * Returns false when the emulated app has exited.
 */
bool android_emulator_step(AndroidEmulator *emu, unsigned max_instructions);

/**
 * Send a touch/pointer event to the emulated application.
 */
void android_emulator_touch(AndroidEmulator *emu, int action, int x, int y, int pointer_id);

/**
 * Send a key event to the emulated application.
 */
void android_emulator_key(AndroidEmulator *emu, int action, int keycode);

/**
 * Send a gamepad/controller event to the emulated application.
 * Maps Xbox/SDL gamepad buttons and axes to Android KEYCODE_BUTTON_* and
 * AXIS_* events for native apps that use the gamepad InputDevice API.
 *
 * @param button_mask  Bitmask of currently-pressed buttons (see AndroidGamepadButton enum)
 * @param left_x       Left stick X axis  (-32768 .. +32767)
 * @param left_y       Left stick Y axis  (-32768 .. +32767)
 * @param right_x      Right stick X axis (-32768 .. +32767)
 * @param right_y      Right stick Y axis (-32768 .. +32767)
 * @param left_trigger  Left trigger  (0 .. 32767)
 * @param right_trigger Right trigger (0 .. 32767)
 */
void android_emulator_gamepad(AndroidEmulator *emu,
                              uint32_t button_mask,
                              int16_t left_x,  int16_t left_y,
                              int16_t right_x, int16_t right_y,
                              int16_t left_trigger, int16_t right_trigger);

/* Android gamepad button bitmask values (matching KEYCODE_BUTTON_* layout) */
enum AndroidGamepadButton {
    AGAMEPAD_A             = (1 << 0),
    AGAMEPAD_B             = (1 << 1),
    AGAMEPAD_X             = (1 << 2),
    AGAMEPAD_Y             = (1 << 3),
    AGAMEPAD_L1            = (1 << 4),
    AGAMEPAD_R1            = (1 << 5),
    AGAMEPAD_L2            = (1 << 6),  /* left trigger as button  */
    AGAMEPAD_R2            = (1 << 7),  /* right trigger as button */
    AGAMEPAD_SELECT        = (1 << 8),  /* Xbox: View / Back       */
    AGAMEPAD_START         = (1 << 9),  /* Xbox: Menu              */
    AGAMEPAD_L3            = (1 << 10), /* left stick click        */
    AGAMEPAD_R3            = (1 << 11), /* right stick click       */
    AGAMEPAD_DPAD_UP       = (1 << 12),
    AGAMEPAD_DPAD_DOWN     = (1 << 13),
    AGAMEPAD_DPAD_LEFT     = (1 << 14),
    AGAMEPAD_DPAD_RIGHT    = (1 << 15),
    AGAMEPAD_GUIDE         = (1 << 16), /* Xbox: Guide / Nexus     */
};

/**
 * Clean up and free all resources.
 */
void android_emulator_destroy(AndroidEmulator *emu);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_EMULATOR_H__ */
