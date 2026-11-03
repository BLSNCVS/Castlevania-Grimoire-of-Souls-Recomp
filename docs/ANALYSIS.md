# Analysis of the source release

Everything below was measured on the iOS Apple Arcade release, version 1.4.1, during the first survey of the game. The port itself uses the macOS release, version 1.4.2, whose `GameAssembly.dylib` has the same layout (see [RECOMP_DESIGN.md](RECOMP_DESIGN.md)).

## Identification

| | |
|---|---|
| Bundle ID | `jp.konami.castlevania.ac` (Apple Arcade) |
| Version | 1.4.1 (build 93), iOS 13.0+, iPhone + iPad |
| Engine | Unity **2020.3.30f1** |
| Scripting backend | IL2CPP, metadata **v27** (Il2CppDumper detects 27.1), no obfuscation or metadata encryption |
| Architecture | arm64 Mach-O, **decrypted** (`cryptid 0`) |
| Data | 7,583 standard `UnityFS` asset bundles (1.2 GB), plus serialized files, 4 MP4 movies |


## Code

`Frameworks/UnityFramework.framework/UnityFramework` (83 MB) contains everything native:

| Section | Size | Contents |
|---|---|---|
| `__text` | 18.5 MB | Unity engine (iOS player, Metal renderer), libil2cpp runtime, native plugins |
| `il2cpp` | 46.5 MB | IL2CPP-generated code for all C# assemblies |

Unity links generated code into its own `il2cpp` section, which gives the recompiler an exact boundary.

* **212,119** functions in the `il2cpp` section are identified by metadata, covering all of it (`tools/analysis/function_table.py`).
* The generated code calls only **682 distinct functions** outside itself in `__text` (libil2cpp codegen helpers: class init, null/bounds exceptions, allocation, write barriers, icall resolution, etc.), plus a few libc stubs. That set is the import interface the recompiled code needs.
* Code is standard IL2CPP output (lazy metadata init via `__DATA` slots, vtable calls through `klass+offset`).

Methods per assembly (with code):

| Assembly | Methods | Types |
|---|---|---|
| Assembly-CSharp | 124,317 | 14,972 |
| mscorlib | 13,199 | 1,666 |
| Assembly-CSharp-firstpass | 7,960 | 829 |
| Unity.InputSystem | 7,390 | 595 |
| System / System.Xml / System.Core | 13,499 | |
| MsgPack + MessagePack | 4,423 | |
| UnityEngine.* modules, UI, TextMeshPro, DOTween, PostProcessing, Photon, ... | rest | |

## Native plugins (P/Invoke `__Internal`)

These are compiled into `UnityFramework` and need Windows replacements:

| Plugin | Externs | Purpose | Windows plan |
|---|---|---|---|
| `SPFXPlugin` | 59 | **SPARKGEAR** effects middleware; renders itself with Metal through a Unity native rendering callback | Recompile its arm64 code with the same recompiler, replace the Metal backend with D3D11 through Unity's native plugin interface. Hardest part. |
| `CloudKitManager` | 24 | iCloud save slots + `NSUserDefaults` | Local save files under `%APPDATA%` |
| `GCController` | 40 | Apple GameController | Not needed; Unity Input System has native XInput/HID support |
| `USndPlugin_ios` | 5 | Audio session queries | Trivial stubs |
| `JNI`, `AndroidJNI` | 253 | Android only, never called on iOS | None |
| Misc. (`bsplugin`, `NpPerf`, `UUID`, `SafeArea`, `DeviceLang`, `LocalNotifyiOS`, stickers, access point) | ~25 | Date format, timezone, profiling, notifications | Small Win32 implementations or stubs |

## Network

The client is online-first. `bsSession.SendRequest` posts MessagePack payloads over `UnityWebRequest` to about **173 PHP endpoints** (`user_data_get.php`, `stamina_recover.php`, ...). There are `netSeq*` sequences for each request, Photon for co-op, and asset downloads from a CDN.

A standalone Windows build needs a **local server emulator** that implements those endpoints from the master data tables shipped in the bundles (`Table` namespace, 740 types), unless an existing offline patch can be adopted.

## Assets

| | |
|---|---|
| Build target | iOS (platform 9) on all 7,583 bundles |
| Texture formats | ASTC 6x6: 6,718 , RGBA32: 2,511 , Alpha8: 158 , RGB24: 2 , other: 1 |
| Shaders | 90, Metal subprograms only |

Of the 90 shaders, about 30 are the game's own (`Custom/*`, `Unlit/Effect*`, `UI/Unlit/mvGlowShader`) and 8 are SPARKGEAR helpers. The rest are stock Unity, TextMeshPro and PostProcessing v2 shaders, whose source is public.

Object types are dominated by MonoBehaviour (135k), GameObject (110k), and UI. The game is mostly 2D/UI with meshes and skinned characters.
