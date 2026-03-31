# Boxedwine Android Emulator

Boxedwine Android is a UWP application that runs Android APK native libraries on Windows 10/11 desktops and **Xbox One / Xbox Series X|S** consoles.  It achieves this by emulating an ARMv7 CPU and the Linux kernel, loading Android native `.so` libraries from APK files, and providing a stub JNI environment so that NDK-based Android apps can run.

Originally based on [Boxedwine](http://boxedwine.org) (a Wine/x86 emulator), this fork replaces Wine with an Android-compatible environment inspired by [apkenv](https://github.com/nichobi/apkenv) and the FLinux UWP bridge.

Boxedwine is open source and released under the terms of the GNU General Public License v2 (GPL).

## Features

- Loads and runs **Android APK** files containing ARMv7 native libraries
- **ARMv7 CPU interpreter** — executes ARM/Thumb instructions including VFP/NEON stubs
- **Android Linux syscall layer** — emulates the syscalls Android NDK libraries expect
- **JNI environment** — stub JNI interface table for native-activity apps
- **ELF dynamic linker** — loads `lib/armeabi-v7a/*.so` from APKs and resolves symbols
- **UWP / Xbox One** — targets Windows Universal Platform for PC, tablet, and Xbox
- **Xbox One gamepad support** — maps Xbox controller to Android input events and provides a virtual cursor for touch emulation
- Built with **C++ / SDL2** for cross-platform rendering and input

## Target Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Windows 10/11 UWP (x64) | ✅ Primary | Desktop, Surface, tablets |
| Windows 10/11 UWP (ARM64) | ✅ Supported | ARM Windows devices |
| **Xbox One** | ✅ Supported | Gamepad input, Dev Mode sideload |
| **Xbox Series X\|S** | ✅ Supported | Gamepad input, Dev Mode sideload |

## Architecture

```
┌─────────────┐
│  APK file   │  (ZIP archive containing lib/armeabi-v7a/*.so)
└──────┬──────┘
       ▼
┌──────────────┐   ┌────────────────┐   ┌──────────────────┐
│  apk_loader  │──▶│ android_linker │──▶│ armv7_interpreter │
│  (minizip)   │   │ (ELF loader)   │   │ (CPU emulation)   │
└──────────────┘   └───────┬────────┘   └────────┬─────────┘
                           │                     │
                    ┌──────▼──────┐       ┌──────▼──────┐
                    │ android_jni │       │android_syscall│
                    │ (JNI stubs) │       │(Linux kernel) │
                    └─────────────┘       └──────────────┘
```

## Xbox One / Xbox Series Setup

1. Enable **Developer Mode** on your Xbox console
2. Build the UWP project (`project/msvc/BoxedWine/uwp/`) for x64 or ARM
3. Deploy the `.appx` package to the Xbox via the **Dev Home** app or Visual Studio remote deployment
4. Place APK files on a USB drive or accessible network share
5. Launch the app and pass the APK path as an argument

**Gamepad mapping:**

| Xbox Button | Android Event |
|-------------|---------------|
| A | Touch tap (at virtual cursor) / BUTTON_A |
| B | BUTTON_B / Back |
| X / Y | BUTTON_X / BUTTON_Y |
| D-pad | DPAD_UP/DOWN/LEFT/RIGHT |
| Left stick | Virtual D-pad |
| Right stick | Virtual cursor (for touch emulation) |
| LB / RB | BUTTON_L1 / BUTTON_R1 |
| View (Back) | SELECT |
| Menu (Start) | START |

## Building

### UWP (Visual Studio 2022)

1. Open `project/msvc/BoxedWine/uwp/uwp.vcxproj` in Visual Studio 2022
2. Select **Release | x64** (or ARM64 for ARM devices)
3. Build → Deploy

### Linux (development / testing)

```bash
cd project/linux
make multiThreaded
```

## TODOs

- [ ] Full Thumb-2 instruction set coverage
- [ ] VFPv3 / NEON SIMD instruction execution
- [ ] Proper AInputQueue for NativeActivity apps
- [ ] OpenGL ES 2.0 translation to host GPU
- [ ] Audio output via Android AudioTrack emulation
- [ ] Dalvik/ART bytecode support (currently native-only)

## Documentation

- [CPU Emulation (ARMv7)](docs/CPUemulation.md)
- [Developer Debugging](docs/Developer-Debugging.md)
- [How To Build](docs/How-To-Build-Boxedwine.md)
- [Upcoming Features](docs/Roadmap-Features.md)
- [Troubleshooting](docs/Troubleshooting-Games-Apps.md)
