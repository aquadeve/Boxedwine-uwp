# Boxedwine - How to Build

## Download the source

You can use git:

git clone https://github.com/danoon2/Boxedwine.git

or you can download a zip of the source from github

https://github.com/danoon2/Boxedwine/archive/refs/heads/master.zip

## Windows

Install Visual Studio 2022.  There is a free Community version that works which is what I use to do most of my development

https://visualstudio.microsoft.com/vs/community/

Once installed, you can open open project\msvc\BoxedWine\BoxedWine.sln

There are no dependencies.

## Windows UWP

### Requirements

- Visual Studio 2022 with the following workloads:
  - **Universal Windows Platform development** (includes the UWP SDK and C++/WinRT tooling)
  - **Desktop development with C++**
- Windows 10 SDK version 10.0.19041.0 or later
- Windows 10 version 1809 (build 17763) or later to deploy and run

### Build Dependencies

The UWP project requires pre-built third-party libraries that are not included in the source tree.
You must place them under `project\msvc\BoxedWine\deps\` before building:

| Path | Source |
|---|---|
| `deps\bin\SDL2.dll` | [aerisarn/sdl-uwp-gl](https://github.com/aerisarn/sdl-uwp-gl) – build the **VisualC-WinRT** project for the target platform |
| `deps\bin\opengl32.dll` | [aerisarn/mesa-uwp](https://github.com/aerisarn/mesa-uwp) – Gallium WGL implementation |
| `deps\bin\libgallium_wgl.dll` | (same Mesa build as above) |
| `deps\bin\libglapi.dll` | (same Mesa build as above) |
| `deps\bin\z-1.dll` | zlib – can be obtained from the zlib project or a MinGW package |
| `deps\lib\SDL2.lib` | Import library for the SDL2 DLL above |
| `deps\lib\opengl32.lib` | Import library for the Mesa opengl32 DLL above |
| `deps\lib\z.lib` | Import library for zlib |
| `deps\include\SDL2\` | SDL2 headers |
| `deps\include\glad\` | GLAD OpenGL extension loader headers |
| `deps\include\KHR\` | Khronos EGL/platform headers |
| `deps\pointer_arrow.png` | Custom cursor image used by the UWP renderer |

The `dxil.dll` redistributable required by the Mesa Gallium driver is typically found at:
```
C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64\dxil.dll   (x64)
C:\Program Files (x86)\Windows Kits\10\Redist\D3D\arm64\dxil.dll  (ARM64)
```
This is installed automatically with the Windows 10 SDK.  Verify the path matches your
SDK installation; the `uwp.vcxproj` currently references the x64 path.  Update the
`<Content Include="...">` entry in `uwp.vcxproj` if your architecture or SDK path differs.

### Build Steps

1. Open `project\msvc\BoxedWine\BoxedWine.sln` in Visual Studio 2022.
2. Set the solution configuration to **Release** (or **Debug**) and the platform to **x64**.
3. Build the **libuwp** project first (right-click → Build).
4. Then build the **BoxedWine** static library project.
5. Finally build the **uwp** application project (this creates the deployable APPX package).

The output APPX will be placed under `project\msvc\BoxedWine\AppPackages\`.

### Sideloading / Developer Mode Installation

Because the package is not signed with a trusted certificate you must enable
Developer Mode or sideloading:

1. Open **Settings → Update & Security → For developers** and turn on **Developer Mode**.
2. Open the generated `AppPackages\uwp_*\uwp_*.msixbundle` (or `.appx`) file with the
   **App Installer** application, or use PowerShell:
   ```powershell
   Add-AppxPackage -Path "AppPackages\uwp_...\uwp_....appxbundle"
   ```
3. If prompted about the signing certificate, right-click the `.pfx` file in the
   `AppPackages` folder, select **Install** → **Local Machine** →
   **Place all certificates in the following store** → **Trusted People**.

### First-Time Setup and Usage

When Boxedwine UWP starts for the first time it shows a setup screen:

1. **Wine Root Folder** – tap the browse button to pick a folder that contains a Wine
   installation (the `drive_c` directory structure).  This folder must be accessible to
   the app.  The best approach is to grant broad filesystem access:
   - Go to **Settings → Privacy → File system** and enable access for **Boxed Wine UWP**.
2. **Wine Executable** – select `wine` or `wine64` inside the Wine root you picked.
3. Press **Start** to launch.

> **Note:** Writable application data (logs, settings, container state) is stored in the
> app's private local folder which is automatically located by the UWP runtime.  You do
> **not** need to create any folders manually.

### Known Limitations

- **broadFileSystemAccess** capability is declared in the manifest.  On Windows 10 1903+
  the user must grant this permission in Settings → Privacy → File system.
- Direct3D-accelerated rendering via the Mesa Gallium driver requires the DXIL shader
  compiler (`dxil.dll`) which is part of the Windows 10 SDK Redistributables.
- Networking support is limited (the same limitation as the desktop version of Boxedwine).
- The virtual cursor is rendered by the app itself; the system cursor is hidden inside the
  window.

## Mac

You need to install XCode 15 or later.  You can do this from the App Store on your Mac.  Boxedwine supports the old Intel Macs and the new M series.

https://developer.apple.com/xcode/

To install the dependencies, open a terminal and got to where you installed the source code.  In the folder project/mac-xcode, you need to run: 

sh fetchDepends.sh

This only needs to be done once, after you download the source

After that, in XCode you just need to open project/mac-xcode/Boxedwine.xcworkspace

## Linux

You need to have GCC 12 or highter.  This means running Debian 12 or Ubuntu 23 or higher

package you might need to install:

zlib1g-dev
libminizip-dev
libsdl2-dev
libssl-dev
libcurl4-openssl-dev

To build, in the terminal go to the source directory and in, project/linux, you need to type

make

