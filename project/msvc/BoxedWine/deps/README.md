# Dependencies

The UWP project's runtime dependencies:

- **SDL2** – [aerisarn/sdl-uwp-gl](https://github.com/aerisarn/sdl-uwp-gl) (VisualC-WinRT project)
- **ANGLE** – Translates OpenGL ES 2.0 → D3D11; source included at `uwp/Angle/`
  - Build `uwp/Angle/winrt/10/src/angle.sln` for libEGL.dll + libGLESv2.dll
- **zlib** – z-1.dll for APK ZIP extraction

Mesa/OpenGL desktop libraries (opengl32.dll, libgallium_wgl.dll, libglapi.dll, GLAD) are
**no longer used** by the UWP/Xbox build.  All rendering goes through ANGLE's D3D11 backend.
