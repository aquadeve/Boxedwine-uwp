/*
 * Boxedwine Android Emulator
 *
 * Top-level orchestration layer that:
 *   1. Loads an APK file
 *   2. Maps native ARMv7 shared libraries into emulated memory
 *   3. Resolves dynamic symbols and applies relocations
 *   4. Initialises the JNI environment
 *   5. Calls JNI_OnLoad on each library
 *   6. Calls the application's ANativeActivity_onCreate or nativeStart entry point
 *   7. Runs the ARMv7 CPU interpreter main loop
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

    /* ARMv7 CPU state */
    ArmV7State cpu;

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
 * Clean up and free all resources.
 */
void android_emulator_destroy(AndroidEmulator *emu);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_EMULATOR_H__ */
