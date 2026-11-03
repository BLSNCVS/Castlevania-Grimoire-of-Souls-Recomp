# Building GoSRecomp

The build runs on Linux (x86-64) and cross-compiles Windows x64 binaries with clang-cl and lld-link. It is a sequence of scripts rather than a single command: analysis of the game binary, translation of the game code, the native builds, and the conversion of the game data.

Every step reads the files of the macOS app, version 1.4.2. Keep them and everything produced from them out of the repository.

## Requirements

| What | Used for |
|---|---|
| clang-cl / lld-link 18 (`clang-cl-18`, `lld-link-18`) | all Windows binaries |
| [xwin](https://github.com/Jake-Shadle/xwin) `splat` output (MSVC CRT + Windows SDK) | `MSVC_SYSROOT` |
| Unity 2020.3.30f1 Editor with **Windows Build Support (IL2CPP)** | libil2cpp sources (`Editor/Data/il2cpp`), `UnityPlayer.dll`, `WindowsPlayer.exe`, `baselib.dll(.lib)`, `UnityCrashHandler64.exe` |
| Python 3.10+ with `lief`, `capstone`, `UnityPy` (`unicorn` for the differential tests) | analysis, translation, data conversion |
| [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) | `script.json`, `il2cpp.h`, `dump.cs`, `stringliteral.json` |
| Wine (or Windows) | running `hlslc.exe` (d3dcompiler_47) for the shader conversion |
| [glslang](https://github.com/KhronosGroup/glslang) 15.4.0 and [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) vulkan-sdk-1.3.296.0 sources | the effects plugin (`THIRD_PARTY`) |
| 7-Zip 24 or newer | extracting the `.dmg` (older p7zip fails on its HFS+ compressed files) |

Environment variables used below:

```
export MSVC_SYSROOT=/path/to/xwin/splat
export CASE_ALIASES=/path/to/case_aliases        # see "Header case" below
export UNITY_IL2CPP=/path/to/Unity/Editor/Data/il2cpp
export UNITY_WIN64=/path/to/WindowsPlayer/files    # baselib.dll.lib and the player files
export THIRD_PARTY=/path/to/sources                # glslang/ and SPIRV-Cross/
export WORK=/path/to/work                          # intermediate files, outside the repo
```

### Header case

Windows headers are included with inconsistent letter case. Compile once, then let `recomp/winbuild/fix_case.py` read the "file not found" errors from the compiler logs and create case-alias symlinks in `CASE_ALIASES`; repeat until the errors are gone.

### glslang

glslang expects a generated `glslang/build_info.h`. Create it from `build_info.h.tmpl` with the version 15.4.0 filled in (major 15, minor 4, patch 0, empty flavor).

## 1. Game files

Extract from the `.dmg`:

```
7z x game.dmg "*/Castlevania Grimoire of Souls.app/Contents/*" -o$WORK/dmg
```

and take the arm64 slices of the game code and of the effects plugin:

```
APP="$WORK/dmg/.../Castlevania Grimoire of Souls.app/Contents"
lipo -thin arm64 "$APP/Frameworks/GameAssembly.dylib" -output $WORK/GameAssembly_arm64.dylib
```

Run Il2CppDumper on `GameAssembly.dylib` with `Resources/Data/il2cpp_data/Metadata/global-metadata.dat`, writing into `$WORK/dump`.

## 2. Analysis

These produce the function tables and type information the translator needs (each script documents its arguments in its header):

| Output | Tool |
|---|---|
| `registration.json`: code pointers of the IL2CPP registration tables | `tools/recomp/registration_tables.py <arm64> 0x3ad2790 registration.json 0x3ad3308` |
| `ft_all.json`: functions to translate | `tools/analysis/function_table.py`, `tools/recomp/classify_text.py` (generated code vs. the libil2cpp runtime) |
| runtime function names | `tools/recomp/match_runtime.py` against a libil2cpp built with symbols (`tools/recomp/align_object.py` helps) |
| `type_classes.json`: how value types are passed under AAPCS64 | `tools/recomp/classify_types.py dump/il2cpp.h dump/script.json type_classes.json` |
| `lsda.json`: exception tables | `tools/recomp/lsda.py <arm64> lsda.json` |
| `ft_gap.json`: compiler-outlined helpers outside the tables | `tools/recomp/leaf_closure.py`, then `tools/recomp/gap_functions.py` |

## 3. Translation

```
GAME_ARM64=$WORK/GameAssembly_arm64.dylib RECOMP_OUT=$WORK/recomp GAP_OUT=$WORK/recomp_gap \
    tools/recomp/translate_gameassembly.sh
```

Then the glue generated from the same inputs:

```
python3 -I tools/recomp/gen_bridges.py $WORK/registration.json $WORK/dump/script.json $WORK/type_classes.json $WORK/bridges
python3 -I tools/recomp/icall_targets.py $UNITY_IL2CPP/libil2cpp.icalls $WORK/dump/script.json $WORK/ft_all.json $WORK/icall_targets.json $WORK/recomp
python3 -I tools/recomp/gen_icall_imports.py $UNITY_IL2CPP/libil2cpp.icalls $WORK/icall_targets.json recomp/winbuild/icall_imports.cpp
python3 -I tools/recomp/gen_libc_imports.py $WORK/GameAssembly_arm64.dylib recomp/winbuild/libc_imports.cpp
```

(`icall_imports.cpp` and `libc_imports.cpp` are checked in; regenerate them only when the tools change.)

## 4. GameAssembly.dll

```
RECOMP_OUT=$WORK/recomp GAP_OUT=$WORK/recomp_gap BRIDGES_OUT=$WORK/bridges BUILD_DIR=$WORK/build \
    recomp/winbuild/build_gameassembly.sh
```

This compiles libil2cpp, the Boehm GC and zlib from the Unity sources, the runtime glue, the offline server, and the ~500 translated files, and links `GameAssembly.dll`. The largest translated functions need a deep compiler stack (`ulimit -s unlimited`, set by the script) and a few GB of memory per compiler process; lower `JOBS` if memory runs out.

At run time `GameAssembly.dll` loads the game's own `GameAssembly.dylib` as data (from `GoS_Data`) for its constants and metadata tables.

## 5. Plugins

```
plugins/apple/build.sh $WORK/plugins                          # stand-ins for the macOS-only plugins
plugins/spfx/build.sh "$APP/PlugIns/SPFXUnityPlugin.bundle/Contents/MacOS/SPFXUnityPlugin" $WORK/spfx
```

The effects plugin is translated the same way as the game code; at run time it loads the original macOS plugin (`SPFXUnityPlugin.macho`, copied by `Setup.bat`) as data.

## 6. Game data conversion

The macOS data is built for Metal. The Windows player needs:

- `globalgamemanagers` declaring Direct3D 11: `tools/shaders/patch_build_settings.py`.
- Direct3D 11 versions of every shader: `tools/shaders/convert_shaders.py` over the serialized files (`globalgamemanagers.assets`, `resources.assets`, `sharedassets*.assets`, `Resources/unity_builtin_extra`) and over the asset bundles that contain shaders (`tools/analysis/survey_assets.py` lists them).
- Direct3D 11 versions of the compute shaders (post-processing): `tools/shaders/convert_compute.py`, over `sharedassets1.assets` and the post-processing bundle.

Both converters translate Metal (MSL generated by HLSLcc) to HLSL and compile it with `hlslc.exe`, built from `tools/shaders/hlslc.cpp` and run next to `d3dcompiler_47.dll`. See [docs/SHADERS.md](docs/SHADERS.md).

## 7. Packaging

The release ships no game files: the converted data goes out as binary patches against the user's own files.

```
tools/patch/build_windows.sh $WORK/patch                       # gosr_patch.exe, run by Setup.bat
tools/patch/make_overlay.sh "$APP/Resources/Data" $WORK/converted $WORK/plugins_all \
    "$UNITY_WIN64/Data/Resources/unity default resources" $WORK/overlay.7z
# unity default resources: from the Windows player variation of the Unity install,
# PlaybackEngines/windowsstandalonesupport/Variations/win64_nondevelopment_il2cpp/Data/Resources
```

`$WORK/converted` holds the converted files from step 6 at their paths under `Data`, and `$WORK/plugins_all` the DLLs from step 5. `make_overlay.sh` writes a patch for every converted file with `tools/patch/gosr_patch.cpp`, checks that each patch rebuilds the converted file exactly, and packs the patches with the plugins and the Windows player's own `unity default resources`.

A release folder holds:

```
GoS.exe                     WindowsPlayer.exe from the Unity Windows player, renamed
UnityPlayer.dll, baselib.dll, UnityCrashHandler64.exe
GameAssembly.dll            step 4
Setup.bat, Play.bat, Play with crash log.bat, gosr.ini, README.txt   packaging/windows/
LICENSE, THIRD_PARTY_NOTICES.md
_tools/7z.exe, 7z.dll, License.txt                                   7-Zip
_tools/gosr_patch.exe, overlay.7z                                    this step
```

`Setup.bat` extracts the data, `GameAssembly.dylib` and the effects plugin from the user's `.dmg`, lays the overlay over the data, and applies the patches. A patch refuses a file whose size or checksum differ, so another version of the game stops the setup with a message instead of producing a broken install.

## Development aids

- `recomp/tests/difftest.cpp` with `tools/recomp/difftest.py` and `tools/recomp/insn_difftest.py`: differential tests of translated code against the arm64 original run in Unicorn.
- `recomp/tests/boot_test.cpp`: boots the IL2CPP runtime from a `GameAssembly.dll` and lists the game's classes.
- Environment variables read by the runtime: `GOSR_VERBOSE` (unbound symbols at startup), `GOSR_TRACE_NRE` (managed exception traces), `GOSR_SAMPLE_SECONDS` / `GOSR_SAMPLE_MS` (main-thread stack samples in the log), `GOSR_SPFX_TRACE`, `GOSR_SPFX_TRACE_DRAWS`, `GOSR_SPFX_DUMP_SHADERS`, `GOSR_SPFX_LINK_STATS`, `GOSR_SPFX_SYNC_SHADERS`, `GOSR_SPFX_WORKERS` (effects plugin), `GOSR_DEBUG_MENU_SHOW=<x>[,<seconds>]` (shows the debug menu at startup, for automated tests).
