# Dependencies

The UWP project's runtime dependencies:

- **SDL2** – Build from `lib/sdl2/VisualC-WinRT/UWP_VS2015/SDL-UWP.sln` (standard SDL2 with EGL/ANGLE)
  - Must be built with `SDL_VIDEO_OPENGL_EGL` and `SDL_VIDEO_OPENGL_ES2` enabled
  - Do **not** use the aerisarn/sdl-uwp-gl fork (that targeted Mesa/WGL which is no longer used)
- **ANGLE** – Translates OpenGL ES 2.0 → D3D11; source included at `uwp/Angle/`
  - Build `uwp/Angle/winrt/10/src/angle.sln` for libEGL.dll + libGLESv2.dll
- **zlib** – z-1.dll for APK ZIP extraction

Mesa/OpenGL desktop libraries (opengl32.dll, libgallium_wgl.dll, libglapi.dll, GLAD) are
**no longer used** by the UWP/Xbox build.  All rendering goes through ANGLE's D3D11 backend.
