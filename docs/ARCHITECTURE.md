# Architecture

## The split

*Castlevania: Grimoire of Souls* is a Unity 2020.3.30f1 IL2CPP game. Its macOS `GameAssembly.dylib` holds two very different halves:

- **Game code**: the C# of the game, compiled by IL2CPP to C++ and then to arm64. It is self-contained generated code that talks to the runtime through a small, known interface.
- **Runtime**: libil2cpp (metadata, GC, icalls) and the engine glue. A native Windows build of exactly this Unity version exists, with libil2cpp sources.

GoSRecomp recompiles only the game code and runs it on the native Windows engine:

```
              macOS original                              Windows port

   Unity player (Metal, Cocoa)                   UnityPlayer.dll (D3D11, Win32), unmodified
   GameAssembly.dylib                            GameAssembly.dll
     |- game code (arm64)          --translate-->   |- game code (C++ from arm64, x64)
     `- libil2cpp (arm64)          --replace---->   |- libil2cpp built from Unity's sources
                                                   `- runtime glue, offline server, extras
   Plugins: SPFX effects, Apple   --translate-->  SPFXUnityPlugin.dll (OpenGL on D3D11)
            services              --replace---->  small stand-in DLLs
   Data (Metal shaders)           --convert---->  data with D3D11 shaders
```

## Translation

`tools/recomp/arm64_recomp.py` turns each arm64 function into a C++ function over a register context (`recomp::Ctx`: `x0`-`x30`, `v0`-`v31`, flags, `sp`). Direct calls become C++ calls; indirect calls go through `CallIndirect`, which finds the translated function for a guest address or calls a native one. Calls into the runtime (`__stubs`) are bound to native handlers by guest address. Exceptions follow the original LSDA tables. See [RECOMP_DESIGN.md](RECOMP_DESIGN.md).

The original binary is loaded as data at run time (`recomp/runtime/macho_image.cpp`): translated code reads its constants, metadata and registration tables from it, rebased.

## Runtime glue (`recomp/winbuild`)

- `codegen_registration.cpp`: hands the game's registration tables to libil2cpp, after `recomp_bridge.cpp` pointed every code pointer at a native entry thunk (`gen_bridges.py`), so the engine can call translated code.
- `runtime_imports.cpp`, `icall_imports.cpp`, `icall_marshal.cpp`, `libc_imports.cpp`: what the game code calls in the runtime, libc and the engine (engine internal calls are resolved by name and called with the Windows x64 convention).
- `field_layout_clang_abi.cpp`: libil2cpp's field layout with the clang rules the game was built with (derived classes reuse a base class's tail padding).
- `crash_report.cpp`: unhandled exception reports and an optional main-thread sampler.

## Offline server

The Apple Arcade release plays offline: each request class (`bsNetReq*`) has an `EmulateServerProcess` that answers from the local save, and the game uses it. A request that would still take the network path (`bsSession.SendRequest`, a coroutine) is caught by `recomp/server/local_server.cpp`, which replaces that coroutine (recompiler `--override`): it decodes the request, answers with an empty successful response and logs the endpoint to `gosr_server.log`. Nothing ever opens a connection. See [server/OFFLINE.md](server/OFFLINE.md).

## Effects plugin (`plugins/spfx`)

SPFX draws the particle effects with OpenGL. Its arm64 code is translated like the game code. The host loads the original plugin as data, binds its imports (libc, libc++, pthreads, OpenGL), runs its static initializers, and presents an OpenGL Core device to it. Its OpenGL calls are recorded (`spfx_gl.cpp`) and drawn with Unity's Direct3D 11 device (`gl_d3d11.cpp`); its GLSL programs are translated to HLSL through glslang and SPIRV-Cross, compiled on worker threads and cached on disk.

## Data conversion (`tools/shaders`)

The data was built for Metal only. `patch_build_settings.py` declares Direct3D 11 in `globalgamemanagers`; `convert_shaders.py` and `convert_compute.py` add D3D11 variants of every shader and compute shader, translated from the Metal ones. See [SHADERS.md](SHADERS.md).

Releases carry the result as binary patches (`tools/patch`): `Setup.bat` applies them to the files of the user's own disk image, so no game file is redistributed.

## Windows extras

- `display.cpp`: frame rate cap, vsync, window mode and size from `gosr.ini`, by wrapping the engine calls the game makes (`Application.targetFrameRate`, `QualitySettings.vSyncCount`).
- `mouse_keys.cpp`: mouse buttons answer for keys a PC keyboard lacks (F13-F15, Command), so they can be bound in the game's keyboard options; `battle_hooks.cpp` names them.
- `debug_menu.cpp`: the F8 debug menu, the controller table switch, and the per-frame hook (on the game's resident `gs.Update`) that runs them on the main thread through the IL2CPP API.
- `battle_hooks.cpp`: native hooks inside translated functions (recompiler `--hook`): invincibility and the damage factor, mouse button names, and controller axes 5-28 (the macOS build of `gsJoystick.IsTrigger` only reads axes 1-4; the Windows tables put a PlayStation right stick vertical and an Xbox d-pad there).
